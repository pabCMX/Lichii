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

#include "common/nx2_kit_stall.h"
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
/// time_control limits. 6.0 is FPS Unlock's largest step (a 100 ms frame). MIN_STEP is the
/// smallest value tested both ways: scale 0.5 and back to off ran clean, while scale 0.1 and 0.01
/// ran clean but broke the game when left (HTPC, 2026-11-10: a jump to address 0 at "off"). A
/// step of 0 hangs the game.
constexpr float MIN_STEP = 0.5f;
constexpr float MAX_SCALE = 4.0f;
constexpr float MAX_FIXED_STEP = 6.0f;
/// time_control {trace: N}: at most 10 minutes at 60 presents a second.
constexpr u64 MAX_TRACE_PRESENTS = 36000;

std::mutex g_control_mutex;
RunControl g_control;
std::mutex g_notify_mutex;
StatusNotify g_notify;
/// notify's limits: a status bar line, shown for at most 10 minutes.
constexpr size_t MAX_NOTIFY_TEXT = 500;
constexpr int MAX_NOTIFY_MS = 600'000;

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

/// The armed step trace: the present hook copies the guest trace area into trace. memory is the
/// game's while armed; the session's end clears the hook first (StopTraceHook).
struct TraceHost {
    std::mutex mutex;
    Memory::Memory* memory{};
    u64 address{}; ///< the guest trace area
    StepTrace trace;
    std::string error; ///< why the trace stopped early, if it did
};
TraceHost g_trace;

u32 Load32(std::span<const u8> bytes, size_t offset) {
    u32 value;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

/// The step trace at each present's reply (SetPresentReplyTrace) while it runs.
void TracePresent(u64 present) {
    namespace TC = FileSys::OotTimeControl;
    std::scoped_lock lock{g_trace.mutex};
    if (g_trace.memory == nullptr || !g_trace.trace.Armed()) {
        return;
    }
    // The guest can run the routine during the copy: read the count again after it, and copy
    // again while it moved (Add drops what the newer calls may have overwritten).
    std::array<u8, TC::TraceSize> area{};
    std::array<u8, 4> after{};
    bool mapped = true;
    for (int tries = 0; tries < 3; ++tries) {
        if (ReadGuestMemory(*g_trace.memory, g_trace.address, area) ||
            ReadGuestMemory(*g_trace.memory, g_trace.address + 4, after)) {
            mapped = false;
            break;
        }
        if (Load32(after, 0) == Load32(area, 4)) {
            break;
        }
    }
    if (!mapped) {
        g_trace.error = "the trace area is no longer mapped";
    } else if (g_trace.trace.Add(present, area, Load32(after, 0))) {
        return;
    }
    g_trace.trace.Stop();
    SetPresentReplyTrace(nullptr);
    if (g_trace.error.empty()) {
        g_trace.memory->Write32(g_trace.address, TC::TraceOffMagic); // done: the routine stops tracing
    }
    g_trace.memory = nullptr;
}

} // Anonymous namespace

void StopStepTrace() {
    std::scoped_lock lock{g_trace.mutex};
    SetPresentReplyTrace(nullptr);
    g_trace.trace.Stop();
    g_trace.memory = nullptr;
}

namespace {

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
    if (!value.is_number_integer()) {
        throw std::runtime_error{
            fmt::format("frames must be a whole number, 1 to {} (got {})", MAX_STEP_FRAMES, value.dump())};
    }
    if (value.get<s64>() < 1 || value.get<u64>() > MAX_STEP_FRAMES) {
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

/// How fast a block runs the game against its own step at 60 FPS: off 1, scale and fixed their
/// value (fixed v is v / 60 s a frame; with s0 at 60 the game's own step is 1 / 60 s).
float Speed(const Block& block) {
    const u32 mode = block.magic == FileSys::OotTimeControl::Magic ? block.mode : 0;
    return mode == 1 || mode == 2 ? block.value : 1.0f;
}

/// Writes a mode so that every state a game call can see in between is the old setting, the
/// new one, or the old mode at the new value (scale v and fixed v run about as fast). "off" only
/// clears the mode word: writing its value first left one call at scale 0 or fixed 0, a step of
/// 0 that hangs the game.
void WriteBlock(Memory::Memory& memory, u64 address, FileSys::OotTimeControl::Mode mode, float value) {
    if (mode != FileSys::OotTimeControl::Mode::Off) {
        memory.Write32(address + offsetof(Block, value), std::bit_cast<u32>(value));
    }
    memory.Write32(address + offsetof(Block, mode), static_cast<u32>(mode));
    memory.Write32(address + offsetof(Block, magic), FileSys::OotTimeControl::Magic);
}

json Describe(const Block& block) {
    const u32 mode = block.magic == FileSys::OotTimeControl::Magic ? block.mode : 0;
    json result = {{"mode", ModeName(mode)}};
    if (mode != 0) {
        result["value"] = block.value;
    }
    return result;
}

/// Arms (presents > 0) or stops (0) the step trace at the guest trace area. A stop keeps the
/// trace's results for the next reads. Refuses an area the game seems to use: one that is not
/// zero and has neither trace magic.
void StepTraceControl(Memory::Memory& memory, u64 area_address, u64 presents) {
    namespace TC = FileSys::OotTimeControl;
    if (presents != 0 && !Common::KitStall::HooksEnabled()) {
        throw std::runtime_error{"the step trace needs the kit's present hook, which "
                                 "EDEN_NX2_KIT_HOOKS=0 turned off"};
    }
    std::array<u8, TC::TraceSize> area{};
    if (ReadGuestMemory(memory, area_address, area)) {
        throw std::runtime_error{fmt::format("the trace area at {:#x} is not mapped", area_address)};
    }
    const u32 magic = Load32(area, 0);
    if (magic != TC::TraceMagic && magic != TC::TraceOffMagic &&
        std::ranges::any_of(area, [](u8 byte) { return byte != 0; })) {
        throw std::runtime_error{fmt::format(
            "the trace area at {:#x} is not zero and has no trace magic: the game uses it", area_address)};
    }
    std::scoped_lock lock{g_trace.mutex};
    if (presents == 0) {
        SetPresentReplyTrace(nullptr);
        g_trace.trace.Stop();
        g_trace.memory = nullptr;
        memory.Write32(area_address, TC::TraceOffMagic);
        return;
    }
    g_trace.trace.Arm(Load32(area, 4), presents);
    g_trace.error.clear();
    g_trace.memory = &memory;
    g_trace.address = area_address;
    SetPresentReplyTrace(TracePresent);
    memory.Write32(area_address, TC::TraceMagic); // last: the routine traces from its next call
}

// Sets how the game's step is changed each frame (OotTimeControl): "off", "scale" (step * value)
// or "fixed" (value). Without mode it only reports. trace: N traces the routine's calls for N
// presents (0 stops), trace_read returns the trace with its first records.
json TimeControl(TrainerState& state, const json& params, const ClientGone& client_gone) {
    namespace TC = FileSys::OotTimeControl;
    std::optional<TC::Mode> mode;
    float value = 0.0f;
    std::optional<u64> trace;
    if (params.contains("trace")) {
        const json& given = params.at("trace");
        if (!given.is_number_integer() || given.get<s64>() < 0 ||
            given.get<u64>() > MAX_TRACE_PRESENTS) {
            throw std::runtime_error{fmt::format(
                "trace is the presents to trace, a whole number, 1 to {}, or 0 to stop (got {})",
                MAX_TRACE_PRESENTS, given.dump())};
        }
        trace = given.get<u64>();
    }
    const bool trace_read = params.value("trace_read", false);
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
                    "{} value must be {} to {} (the game broke when it left scale 0.1 or 0.01, and a "
                    "step of 0 hangs it; use pause and step_frames to stop time)",
                    name, MIN_STEP, high)};
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
    } else {
        const auto stages = TimeRamp(Speed(previous), *mode == TC::Mode::Off ? 1.0f : value);
        if (!stages.empty() && state.run.paused()) {
            throw std::runtime_error{"resume first: a step more than 2x faster goes up in steps over "
                                     "running frames"};
        }
        // Each stage for at least one present, so the game's step never grows more than 2x
        // from one frame to the next. Leaving off for scale or fixed, the stages are scale.
        const TC::Mode stage_mode = *mode == TC::Mode::Off ? TC::Mode::Scale : *mode;
        json ramp = json::array();
        for (const float stage : stages) {
            WriteBlock(memory, address, stage_mode, stage);
            ramp.push_back(stage);
            const u64 at = Presents();
            if (!WaitFor(state, client_gone, 2s, [&] { return Presents() > at; })) {
                throw std::runtime_error{fmt::format(
                    "the ramp stopped at {} {}: no present within 2 s; call again to go on",
                    ModeName(static_cast<u32>(stage_mode)), stage)};
            }
        }
        WriteBlock(memory, address, *mode, value);
        result.update(Describe(ReadBlock(memory, address)));
        if (!ramp.empty()) {
            result["ramp"] = std::move(ramp);
        }
    }
    if (trace) {
        StepTraceControl(memory, address + (TC::TraceOffset - TC::BlockOffset), *trace);
    }
    std::scoped_lock lock{g_trace.mutex};
    if (trace || trace_read || g_trace.trace.Armed()) {
        // Records only when asked or at the stop: the panel's report polls must stay small.
        result["trace"] = g_trace.trace.Summary(trace_read || trace == u64{0});
        if (!g_trace.error.empty()) {
            result["trace"]["error"] = g_trace.error;
        }
    }
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

// {text, ms?}: shows text in the status bar for ms (default 5000). Answers at once; the frontend
// shows it on its own thread.
json Notify(TrainerState&, const json& params, const ClientGone&) {
    for (const auto& [key, value] : params.items()) {
        if (key != "text" && key != "ms") {
            throw std::runtime_error{fmt::format("unknown key \"{}\": want text or ms", key)};
        }
    }
    if (!params.contains("text") || !params.at("text").is_string()) {
        throw std::runtime_error{"text must be a string"};
    }
    std::string text = params.at("text").get<std::string>();
    if (text.size() > MAX_NOTIFY_TEXT) {
        throw std::runtime_error{fmt::format("text has {} bytes; at most {}", text.size(),
                                             MAX_NOTIFY_TEXT)};
    }
    // One status bar line.
    std::ranges::replace(text, '\n', ' ');
    std::ranges::replace(text, '\r', ' ');
    const json ms_value = params.value("ms", json{5000});
    if (!ms_value.is_number_integer() || ms_value.get<s64>() < 1 ||
        ms_value.get<s64>() > MAX_NOTIFY_MS) {
        throw std::runtime_error{fmt::format("ms must be an integer of 1 to {}", MAX_NOTIFY_MS)};
    }
    const int ms = ms_value.get<int>();
    std::scoped_lock lock{g_notify_mutex};
    if (!g_notify) {
        throw std::runtime_error{"this frontend has no status bar"};
    }
    g_notify(text, ms);
    return {{"shown", true}, {"ms", ms}};
}

} // Anonymous namespace

std::vector<float> TimeRamp(float from, float to) {
    std::vector<float> stages;
    if (!(from > 0.0f) || !std::isfinite(from) || !std::isfinite(to)) {
        return stages;
    }
    // Doubling from `from` while the target is more than a doubling away; the last write, to
    // `to`, is then at most TIME_RAMP_FACTOR faster. At most 32 stages (a factor of 4e9).
    for (float current = from; to > current * TIME_RAMP_FACTOR && stages.size() < 32;) {
        current *= TIME_RAMP_FACTOR;
        stages.push_back(current);
    }
    return stages;
}

void StepTrace::Field::Add(float value) {
    if (std::isnan(value)) {
        ++nan;
        return;
    }
    min = seen ? (std::min)(min, value) : value;
    max = seen ? (std::max)(max, value) : value;
    seen = true;
    if (distinct.size() < MaxDistinct || distinct.contains(value)) {
        distinct.insert(value);
    } else {
        more = true;
    }
}

void StepTrace::Arm(u32 count, u64 presents_wanted) {
    *this = StepTrace{};
    armed = true;
    next = count + 1;
    wanted = presents_wanted;
}

bool StepTrace::Add(u64 present, std::span<const u8> area, std::optional<u32> count_after) {
    namespace TC = FileSys::OotTimeControl;
    if (!armed || area.size() < TC::TraceSize) {
        return false;
    }
    const u32 count = Load32(area, 4);
    // Calls since the last present. Call j writes record j % 8 over call j - 8, and call
    // `newest` (the count after the copy) can be in the middle of its first write, so only the
    // calls after newest - 8 are intact: at most TraceRecords - 1 when the count held still. The
    // rest are counted as dropped.
    const s32 fresh_signed = static_cast<s32>(count - next);
    const u32 fresh = fresh_signed > 0 ? static_cast<u32>(fresh_signed) : 0;
    const u32 newest = count_after.value_or(count);
    const s32 intact = static_cast<s32>(count + (TC::TraceRecords - 1) - newest);
    const u32 readable = (std::min<u32>)(fresh, intact > 0 ? static_cast<u32>(intact) : 0);
    dropped += fresh - readable;
    for (u32 call = count - readable; call != count; ++call) {
        const size_t at = 16 + (call % TC::TraceRecords) * TC::TraceRecordSize;
        std::array<float, 4> values;
        for (size_t i = 0; i < values.size(); ++i) {
            values[i] = std::bit_cast<float>(Load32(area, at + 4 * i));
            fields[i].Add(values[i]);
        }
        if (kept.size() < KeptRecords) {
            kept.emplace_back(present, values);
        }
        ++calls;
    }
    if (fresh_signed > 0) {
        next = count;
    }
    first_present = presents == 0 ? present : first_present;
    last_present = present;
    ++per_present[fresh];
    return ++presents < wanted;
}

json StepTrace::Summary(bool records) const {
    json per = json::object();
    for (const auto& [n, count] : per_present) {
        per[std::to_string(n)] = count;
    }
    json result = {{"armed", armed},       {"presents", presents},   {"wanted", wanted},
                   {"calls", calls},       {"dropped", dropped},     {"first_present", first_present},
                   {"last_present", last_present}, {"calls_per_present", std::move(per)}};
    static constexpr std::array<const char*, 4> NAMES{"s1", "s0", "own_step", "step"};
    for (size_t i = 0; i < NAMES.size(); ++i) {
        json field = {{"distinct", std::vector<float>(fields[i].distinct.begin(), fields[i].distinct.end())},
                      {"more_distinct", fields[i].more}, {"nan", fields[i].nan}};
        if (fields[i].seen) {
            field["min"] = fields[i].min;
            field["max"] = fields[i].max;
        }
        result[NAMES[i]] = std::move(field);
    }
    if (records) {
        json list = json::array();
        for (const auto& [present, values] : kept) {
            list.push_back({present, values[0], values[1], values[2], values[3]});
        }
        result["records"] = std::move(list);
    }
    return result;
}

void SetRunControl(RunControl control) {
    std::scoped_lock lock{g_control_mutex};
    g_control = std::move(control);
}

void SetStatusNotify(StatusNotify notify) {
    std::scoped_lock lock{g_notify_mutex};
    g_notify = std::move(notify);
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
    add("notify", Notify);
}

} // namespace Core::Nx2Debug
