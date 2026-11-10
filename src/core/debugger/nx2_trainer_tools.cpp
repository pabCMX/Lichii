// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

#include <fmt/format.h>

#include "common/nx2_live_switch.h"
#include "core/core.h"
#include "core/debugger/nx2_debug_tools.h"
#include "core/debugger/nx2_trainer_tools.h"
#include "core/file_sys/oot_time_control.h"
#include "core/hle/kernel/k_process.h"
#include "core/memory.h"

namespace Core::Nx2Debug {

using nlohmann::json;
using namespace std::chrono_literals;

namespace {

/// Presents one step_frames call runs at most (10 s at 60 fps).
constexpr u64 MAX_STEP_FRAMES = 600;
/// time_control limits. 6.0 is FPS Unlock's largest step (a 100 ms frame). The game hangs on a
/// step of 0 (live, 2026-11-10): no value below MIN_STEP, the smallest one tested live (scale).
constexpr float MIN_STEP = 0.01f;
constexpr float MAX_SCALE = 4.0f;
constexpr float MAX_FIXED_STEP = 6.0f;

std::mutex g_control_mutex;
RunControl g_control;

struct LiveSwitch {
    LiveSwitchInfo info;
    bool (*get)();
    void (*set)(bool);
};

// Observers only: they log or count and change nothing the game sees. docs/nx2/trainer-kit.md
// lists the switches left out and why.
const std::array<LiveSwitch, 4> SWITCHES{{
    {{"render_trace", "EDEN_NX2_RENDER_TRACE", "observer"},
     [] { return Common::Nx2Live::g_render_trace(); },
     [](bool on) { Common::Nx2Live::g_render_trace.Set(on); }},
    {{"ipc_trace", "EDEN_NX2_IPC_TRACE", "observer"},
     [] { return Common::Nx2Live::g_ipc_trace(); },
     [](bool on) { Common::Nx2Live::g_ipc_trace.Set(on); }},
    {{"ipc_summary", "EDEN_NX2_IPC_SUMMARY", "observer"},
     [] { return Common::Nx2Live::g_ipc_summary(); },
     [](bool on) { Common::Nx2Live::g_ipc_summary.Set(on); }},
    {{"vdec_trace", "EDEN_NX2_VDEC_TRACE", "observer"},
     [] { return Common::Nx2Live::g_vdec_trace(); },
     [](bool on) { Common::Nx2Live::g_vdec_trace.Set(on); }},
}};

const std::array<LiveSwitchInfo, SWITCHES.size()> SWITCH_INFO = [] {
    std::array<LiveSwitchInfo, SWITCHES.size()> info;
    std::ranges::transform(SWITCHES, info.begin(), &LiveSwitch::info);
    return info;
}();

struct TrainerState {
    System& system;
    RunState run;
    std::function<bool()> stopping;
    CaptureStart capture;
};

/// After a pause, the GPU may still finish the frames queued before it. A capture requested
/// after this wait takes the next frame, the one the step presents.
constexpr auto GPU_DRAIN = 100ms;

u64 Presents() {
    return g_nx2_present_count.load(std::memory_order_relaxed);
}

std::chrono::milliseconds Timeout(const json& params, int fallback_s) {
    return std::chrono::seconds{(std::clamp)(params.value("timeout_s", fallback_s), 1, 120)};
}

void Control(bool run) {
    std::scoped_lock lock{g_control_mutex};
    if (!g_control) {
        throw std::runtime_error{"this frontend did not register pause and resume"};
    }
    g_control(run);
}

/// Polls until done() or the timeout; false on timeout, when the session ends or the client
/// leaves.
template <typename Done>
bool WaitFor(const TrainerState& state, const ClientGone& client_gone,
             std::chrono::milliseconds timeout, Done&& done) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (state.stopping() || client_gone() || std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

void RequireGame(const TrainerState& state) {
    if (!state.run.powered_on()) {
        throw std::runtime_error{"no game is running"};
    }
}

/// Asks the frontend for the run state and waits until the system has it.
void SetRunning(const TrainerState& state, bool run, const ClientGone& client_gone,
                std::chrono::milliseconds timeout) {
    if (state.run.paused() != run) {
        return;
    }
    Control(run);
    if (!WaitFor(state, client_gone, timeout, [&] { return state.run.paused() != run; })) {
        throw std::runtime_error{fmt::format("the game did not {} within {} s",
                                             run ? "resume" : "pause",
                                             std::chrono::duration_cast<std::chrono::seconds>(timeout).count())};
    }
}

json Pause(TrainerState& state, const json& params, const ClientGone& client_gone) {
    RequireGame(state);
    const bool was_paused = state.run.paused();
    SetRunning(state, false, client_gone, Timeout(params, 5));
    return {{"paused", true}, {"changed", !was_paused}, {"presents", Presents()}};
}

json Resume(TrainerState& state, const json& params, const ClientGone& client_gone) {
    RequireGame(state);
    const bool was_paused = state.run.paused();
    SetRunning(state, true, client_gone, Timeout(params, 5));
    return {{"paused", false}, {"changed", was_paused}, {"presents", Presents()}};
}

// Resumes until `frames` more guest presents, then pauses again. Present-granular: the pause
// lands a present or two late, and the GPU drains a frame behind the CPU. With capture, the
// capture is requested while the game is paused and the GPU has drained, with two presents left:
// the frame end of the first takes the request and arms the frame dump (RasterizerVulkan::
// TickFrame), which records the next frame, the last present stepped. So a capture steps at least
// two presents, and frames: 1 with capture advances 2. The capture is of exactly capture_present;
// the frontend's pause can land after it ("overshoot" presents), as for any step.
json StepFrames(TrainerState& state, const json& params, const ClientGone& client_gone) {
    RequireGame(state);
    const json& value = params.at("frames");
    if (!value.is_number_integer() || value.get<s64>() < 1 ||
        value.get<u64>() > MAX_STEP_FRAMES) {
        throw std::runtime_error{fmt::format("frames must be 1 to {}", MAX_STEP_FRAMES)};
    }
    const u64 frames = value.get<u64>();
    const json capture = params.value("capture", json{false});
    const bool capturing = capture.is_object() || (capture.is_boolean() && capture.get<bool>());
    if (capturing && !state.capture) {
        throw std::runtime_error{"this server cannot capture frames"};
    }
    const auto timeout = Timeout(params, static_cast<int>(frames / 10 + 5));
    SetRunning(state, false, client_gone, 5s);
    const u64 start = Presents();
    // Runs until n more presents than at its call, then pauses on every path, so a timeout or a
    // lost client does not leave the game running.
    const auto step = [&](u64 n) {
        const u64 from = Presents();
        bool done = false;
        try {
            SetRunning(state, true, client_gone, 5s);
            done = WaitFor(state, client_gone, timeout, [&] { return Presents() >= from + n; });
        } catch (const std::exception&) {
            // The resume may still be queued in the frontend: queue a pause after it.
            Control(false);
            throw;
        }
        SetRunning(state, false, [] { return false; }, 5s);
        return done;
    };
    const u64 total = capturing ? (std::max<u64>)(frames, 2) : frames;
    bool complete = !capturing || total == 2 ? true : step(total - 2);
    json result;
    if (capturing && complete) {
        std::this_thread::sleep_for(GPU_DRAIN);
        const u64 before = Presents();
        const auto end = state.capture(capture.is_object() ? capture : json::object());
        try {
            complete = step(2);
        } catch (const std::exception&) {
            end(true); // no capture of a later frame
            throw;
        }
        if (!complete) {
            end(true);
            throw std::runtime_error{"the step did not present within the timeout; no capture"};
        }
        result["capture"] = end(false);
        result["capture_present"] = before + 2;
        // The frontend's pause can land a present or two after the captured frame (present-granular).
        result["overshoot"] = Presents() - (before + 2);
    } else if (!capturing) {
        complete = step(frames);
    }
    result.update({{"requested", frames},
                   {"advanced", Presents() - start},
                   {"complete", complete},
                   {"paused", true},
                   {"presents", Presents()}});
    return result;
}

const char* ModeName(u32 mode) {
    switch (static_cast<FileSys::OotTimeControl::Mode>(mode)) {
    case FileSys::OotTimeControl::Mode::Off:
        return "off";
    case FileSys::OotTimeControl::Mode::Scale:
        return "scale";
    case FileSys::OotTimeControl::Mode::Fixed:
        return "fixed";
    }
    return "unknown";
}

struct Block {
    u32 magic;
    u32 mode;
    float value;
    u32 reserved;
};
static_assert(sizeof(Block) == FileSys::OotTimeControl::BlockSize);

Block ReadBlock(Memory::Memory& memory, u64 address) {
    std::array<u8, sizeof(Block)> bytes{};
    if (ReadGuestMemory(memory, address, bytes)) {
        throw std::runtime_error{fmt::format("the control block at {:#x} is not mapped", address)};
    }
    return std::bit_cast<Block>(bytes);
}

json Describe(const Block& block) {
    const u32 mode = block.magic == FileSys::OotTimeControl::Magic ? block.mode : 0;
    json result = {{"mode", ModeName(mode)}};
    if (mode != 0) {
        result["value"] = block.value;
    }
    return result;
}

// Sets how the game's step is changed each frame (OotTimeControl): "off", "scale" (step * value)
// or "fixed" (value). Without mode it only reports.
json TimeControl(TrainerState& state, const json& params, const ClientGone&) {
    namespace TC = FileSys::OotTimeControl;
    std::optional<TC::Mode> mode;
    float value = 0.0f;
    if (params.contains("mode")) {
        const std::string name = params.at("mode").get<std::string>();
        if (name == "off") {
            mode = TC::Mode::Off;
        } else if (name == "scale" || name == "fixed") {
            mode = name == "scale" ? TC::Mode::Scale : TC::Mode::Fixed;
            const float high = *mode == TC::Mode::Scale ? MAX_SCALE : MAX_FIXED_STEP;
            const json& given = params.at("value");
            value = given.is_number() ? given.get<float>() : -1.0f;
            if (!std::isfinite(value) || value < MIN_STEP || value > high) {
                throw std::runtime_error{fmt::format(
                    "{} value must be {} to {} (a step of 0 hangs the game)", name, MIN_STEP, high)};
            }
        } else {
            throw std::runtime_error{
                fmt::format("unknown mode \"{}\": want off, scale or fixed", name)};
        }
    }
    RequireGame(state);
    const u64 address = TC::BlockAddress();
    if (address == 0) {
        throw std::runtime_error{"time control is not installed: boot Ocarina of Time with "
                                 "EDEN_NX2_OOT_TIME_CONTROL=1 (the log then shows \"Applying time "
                                 "control\")"};
    }
    Kernel::KProcess* const process = state.system.ApplicationProcess();
    if (process == nullptr) {
        throw std::runtime_error{"no application process is running"};
    }
    Memory::Memory& memory = process->GetMemory();
    const Block previous = ReadBlock(memory, address);
    const bool ours = previous.magic == TC::Magic;
    if (!ours && (previous.magic | previous.mode | std::bit_cast<u32>(previous.value) |
                  previous.reserved) != 0) {
        throw std::runtime_error{fmt::format(
            "the control block at {:#x} is not zero and has no magic: the game uses it, so time "
            "control stays off",
            address)};
    }
    json result = {{"address", fmt::format("{:#x}", address)}, {"previous", Describe(previous)}};
    if (!mode) {
        result.update(Describe(previous));
        return result;
    }
    // The value first: the routine reads the mode, then the value, at its next call.
    memory.Write32(address + offsetof(Block, value), std::bit_cast<u32>(value));
    memory.Write32(address + offsetof(Block, mode), static_cast<u32>(*mode));
    memory.Write32(address + offsetof(Block, magic), TC::Magic);
    result.update(Describe(ReadBlock(memory, address)));
    return result;
}

// Lists the live switches; {set: {name: bool, ...}} flips them first. Unknown names change nothing.
json Switches(TrainerState&, const json& params, const ClientGone&) {
    const json set = params.value("set", json::object());
    if (!set.is_object()) {
        throw std::runtime_error{"set must be an object of switch names and booleans"};
    }
    std::vector<std::pair<const LiveSwitch*, bool>> changes;
    for (const auto& [name, on] : set.items()) {
        const auto it = std::ranges::find(SWITCHES, name, [](const LiveSwitch& entry) {
            return entry.info.name;
        });
        if (it == SWITCHES.end()) {
            throw std::runtime_error{fmt::format("unknown switch \"{}\"", name)};
        }
        if (!on.is_boolean()) {
            throw std::runtime_error{fmt::format("switch \"{}\" wants true or false", name)};
        }
        changes.emplace_back(&*it, on.get<bool>());
    }
    for (const auto& [entry, on] : changes) {
        entry->set(on);
    }
    json list = json::array();
    for (const LiveSwitch& entry : SWITCHES) {
        list.push_back({{"name", entry.info.name},
                        {"env", entry.info.env},
                        {"class", entry.info.kind},
                        {"on", entry.get()}});
    }
    return {{"switches", std::move(list)}};
}

} // Anonymous namespace

void SetRunControl(RunControl control) {
    std::scoped_lock lock{g_control_mutex};
    g_control = std::move(control);
}

std::span<const LiveSwitchInfo> LiveSwitches() {
    return SWITCH_INFO;
}

void AddTrainerHandlers(Handlers& handlers, System& system, std::function<bool()> stopping,
                        CaptureStart capture) {
    RunState run{
        .powered_on = [&system] { return system.IsPoweredOn(); },
        .paused = [&system] { return system.IsPaused(); },
    };
    AddTrainerHandlers(handlers, system, std::move(run), std::move(stopping), std::move(capture));
}

void AddTrainerHandlers(Handlers& handlers, System& system, RunState run,
                        std::function<bool()> stopping, CaptureStart capture) {
    const auto state = std::make_shared<TrainerState>(system, std::move(run), std::move(stopping),
                                                      std::move(capture));
    const auto add = [&](const char* name,
                         json (*tool)(TrainerState&, const json&, const ClientGone&)) {
        handlers.emplace(name, [state, tool](const json& params, const ClientGone& client_gone) {
            return tool(*state, params, client_gone);
        });
    };
    add("pause", Pause);
    add("resume", Resume);
    add("step_frames", StepFrames);
    add("time_control", TimeControl);
    add("switches", Switches);
}

} // namespace Core::Nx2Debug
