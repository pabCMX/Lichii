// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
#include <cstring>
#include <optional>
#include <mutex>
#include <condition_variable>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "common/nx2_live_switch.h"
#include "common/scope_exit.h"
#include "core/core.h"
#include "core/debugger/nx2_debug_tools.h"
#include "core/debugger/nx2_trainer_tools.h"
#include "core/file_sys/oot_time_control.h"
#include "tests/core/system_test_context.h"
#include "video_core/renderer_vulkan/vk_frame_dump.h"

namespace {

using namespace std::chrono_literals;
using Catch::Matchers::ContainsSubstring;
using Core::Nx2Debug::ClientGone;
using Core::Nx2Debug::Handlers;
using nlohmann::json;

/// A frontend stand-in: while not paused it presents about every 2 ms, unless presenting is
/// false (a game that hangs while it runs).
struct FakeFrontend {
    std::atomic<bool> paused{true};
    std::atomic<bool> presenting{true};
    std::atomic<bool> stop{};
    std::thread presenter{[this] {
        while (!stop) {
            if (!paused && presenting) {
                Core::g_nx2_present_count.fetch_add(1);
            }
            std::this_thread::sleep_for(2ms);
        }
    }};

    ~FakeFrontend() {
        stop = true;
        presenter.join();
    }
};

} // Anonymous namespace

TEST_CASE("switches lists the live switches and flips them together", "[nx2_trainer]") {
    ScopedSystemTestContext context;
    Core::System system;
    const auto call = [&](const json& params) {
        return Core::Nx2Debug::CallTool(system, "switches", params);
    };
    SCOPE_EXIT {
        Common::Nx2Live::g_render_trace.Set(false);
        Common::Nx2Live::g_ipc_trace.Set(false);
    };
    const json listed = call(json::object())["switches"];
    const auto switches = Core::Nx2Debug::LiveSwitches();
    REQUIRE(listed.size() == switches.size());
    for (size_t i = 0; i < switches.size(); ++i) {
        REQUIRE(listed[i]["name"] == switches[i].name);
        REQUIRE(listed[i]["env"] == switches[i].env);
        REQUIRE(listed[i]["class"] == "observer");
    }

    const json set = call({{"set", {{"render_trace", true}, {"ipc_trace", true}}}})["switches"];
    REQUIRE(Common::Nx2Live::g_render_trace());
    REQUIRE(Common::Nx2Live::g_ipc_trace());
    REQUIRE(set[0]["on"] == true);

    // A bad entry fails the whole call before anything changes.
    REQUIRE_THROWS_WITH(call({{"set", {{"ipc_trace", false}, {"no_such_switch", true}}}}),
                        ContainsSubstring("unknown switch"));
    REQUIRE_THROWS_WITH(call({{"set", {{"ipc_trace", 0}}}}), ContainsSubstring("true or false"));
    REQUIRE(Common::Nx2Live::g_ipc_trace());

    call({{"set", {{"render_trace", false}, {"ipc_trace", false}}}});
    REQUIRE_FALSE(Common::Nx2Live::g_render_trace());
    REQUIRE_FALSE(Common::Nx2Live::g_ipc_trace());
}

TEST_CASE("pause, resume and step_frames drive the frontend's run control", "[nx2_trainer]") {
    ScopedSystemTestContext context;
    Core::System system;
    FakeFrontend frontend;
    Handlers handlers;
    Core::Nx2Debug::AddTrainerHandlers(
        handlers, system,
        {.powered_on = [] { return true; }, .paused = [&] { return frontend.paused.load(); }},
        [] { return false; });
    const auto call = [&](const char* method, const json& params) {
        return handlers.at(method)(params, [] { return false; });
    };
    const u64 old_count = Core::g_nx2_present_count.load();
    SCOPE_EXIT {
        Core::Nx2Debug::SetRunControl({});
        Core::g_nx2_present_count = old_count;
    };

    REQUIRE_THROWS_WITH(call("resume", json::object()), ContainsSubstring("did not register"));
    int calls = 0;
    Core::Nx2Debug::SetRunControl([&](bool run) {
        ++calls;
        frontend.paused = !run;
    });

    REQUIRE(call("resume", json::object())["changed"] == true);
    REQUIRE(call("resume", json::object())["changed"] == false);
    REQUIRE(call("pause", json::object())["changed"] == true);
    REQUIRE(calls == 2);

    const json step = call("step_frames", {{"frames", 5}});
    REQUIRE(step["complete"] == true);
    REQUIRE(step["advanced"].get<u64>() >= 5);
    REQUIRE(step["paused"] == true);
    REQUIRE(frontend.paused);

    // A step from a running game pauses first, and a step that times out still pauses.
    call("resume", json::object());
    Core::Nx2Debug::SetRunControl([&](bool run) { frontend.paused = !run; });
    REQUIRE(call("step_frames", {{"frames", 2}})["paused"] == true);
    frontend.presenting = false;
    const json stuck = call("step_frames", {{"frames", 600}, {"timeout_s", 1}});
    REQUIRE(stuck["complete"] == false);
    REQUIRE(frontend.paused);

    // A client that leaves before the queued resume runs: a pause is queued after the resume.
    std::vector<bool> queued;
    Core::Nx2Debug::SetRunControl([&](bool run) { queued.push_back(run); });
    REQUIRE_THROWS(handlers.at("step_frames")({{"frames", 1}}, [] { return true; }));
    REQUIRE(queued == std::vector<bool>{true, false});

    for (const json& bad : {json{{"frames", 0}}, json{{"frames", 601}}, json{{"frames", "1"}}}) {
        REQUIRE_THROWS_WITH(call("step_frames", bad), ContainsSubstring("frames must be"));
    }
    REQUIRE_THROWS_WITH(call("step_frames", json{{"frames", 1.5}}), ContainsSubstring("a whole number"));
}

TEST_CASE("step_frames with capture records the last stepped present, as the frame dump schedules it",
          "[nx2_trainer]") {
    // The GPU side as RasterizerVulkan::TickFrame runs it at each frame end: a pending request
    // arms the real FrameDumpSchedule, whose Tick then starts recording the NEXT frame and
    // finishes at that frame's end.
    std::mutex mutex;
    std::condition_variable changed;
    Vulkan::FrameDumpSchedule schedule;
    bool pending = false;
    std::optional<u64> recording, captured;
    std::atomic<bool> paused{true}, presenting{true}, stop{};
    std::thread presenter{[&] {
        while (!stop) {
            if (!paused && presenting) {
                const u64 present = Core::g_nx2_present_count.fetch_add(1) + 1;
                std::scoped_lock lock{mutex};
                if (pending && schedule.Arm(false, false, 1)) {
                    pending = false;
                }
                switch (schedule.Tick(0.0)) {
                case Vulkan::FrameDumpSchedule::Step::Start:
                    recording = present + 1;
                    break;
                case Vulkan::FrameDumpSchedule::Step::Finish:
                    captured = recording;
                    changed.notify_all();
                    break;
                case Vulkan::FrameDumpSchedule::Step::None:
                    break;
                }
            }
            std::this_thread::sleep_for(2ms);
        }
    }};
    SCOPE_EXIT {
        stop = true;
        presenter.join();
    };
    ScopedSystemTestContext context;
    Core::System system;
    std::vector<std::string> events;
    Handlers handlers;
    Core::Nx2Debug::AddTrainerHandlers(
        handlers, system, {.powered_on = [] { return true; }, .paused = [&] { return paused.load(); }},
        [] { return false; },
        [&](const json& params) {
            std::scoped_lock lock{mutex};
            events.push_back(fmt::format("request paused={} sampled={}", paused.load(),
                                         params.value("sampled", false)));
            pending = true;
            captured.reset();
            return [&](bool cancel) -> json {
                std::unique_lock lock{mutex};
                if (cancel) {
                    pending = false;
                    events.push_back("cancel");
                    return {};
                }
                if (!changed.wait_for(lock, 2s, [&] { return captured.has_value(); })) {
                    throw std::runtime_error{"capture timed out"};
                }
                events.push_back("wait");
                return {{"recorded_present", *captured}};
            };
        });
    const auto call = [&](const json& params) {
        return handlers.at("step_frames")(params, [] { return false; });
    };
    const u64 old_count = Core::g_nx2_present_count.load();
    SCOPE_EXIT {
        Core::Nx2Debug::SetRunControl({});
        Core::g_nx2_present_count = old_count;
    };
    Core::Nx2Debug::SetRunControl([&](bool run) { paused = !run; });

    for (const u64 frames : {1, 2, 5}) {
        events.clear();
        const u64 before = Core::g_nx2_present_count.load();
        const json result = call({{"frames", frames}, {"capture", {{"sampled", true}}}});
        REQUIRE(events == std::vector<std::string>{"request paused=true sampled=true", "wait"});
        REQUIRE(result["complete"] == true);
        REQUIRE(paused);
        // The capture is of the last stepped present: at least 2, one arms the frame dump.
        REQUIRE(result["capture"]["recorded_present"] == result["capture_present"]);
        REQUIRE(result["capture_present"].get<u64>() == before + (std::max<u64>)(frames, 2));
        REQUIRE(result["advanced"].get<u64>() >= (std::max<u64>)(frames, 2));
    }

    // A frontend whose pause lands one present late: the capture stays of capture_present, and the
    // reply says how far the game ran past it.
    Core::Nx2Debug::SetRunControl([&](bool run) {
        if (!run) {
            const u64 at = Core::g_nx2_present_count.load();
            while (Core::g_nx2_present_count.load() < at + 1) {
                std::this_thread::sleep_for(1ms);
            }
        }
        paused = !run;
    });
    events.clear();
    {
        const u64 before = Core::g_nx2_present_count.load();
        const json late = call({{"frames", 1}, {"capture", true}});
        REQUIRE(late["capture"]["recorded_present"] == late["capture_present"]);
        REQUIRE(late["capture_present"].get<u64>() == before + 2);
        REQUIRE(late["overshoot"].get<u64>() >= 1);
        REQUIRE(late["presents"].get<u64>() == late["capture_present"].get<u64>() + late["overshoot"].get<u64>());
    }
    Core::Nx2Debug::SetRunControl([&](bool run) { paused = !run; });

    // A step that presents nothing withdraws the request: no capture of a later frame.
    events.clear();
    presenting = false;
    REQUIRE_THROWS_WITH(call({{"frames", 1}, {"capture", true}, {"timeout_s", 1}}), ContainsSubstring("no capture"));
    REQUIRE(events == std::vector<std::string>{"request paused=true sampled=false", "cancel"});
    REQUIRE(paused);
}

TEST_CASE("time_control checks its mode and value and needs the patch", "[nx2_trainer]") {
    ScopedSystemTestContext context;
    Core::System system;
    Handlers handlers;
    Core::Nx2Debug::AddTrainerHandlers(
        handlers, system, {.powered_on = [] { return true; }, .paused = [] { return false; }},
        [] { return false; });
    const auto call = [&](const json& params) {
        return handlers.at("time_control")(params, [] { return false; });
    };
    REQUIRE_THROWS_WITH(call({{"mode", "slow"}}), ContainsSubstring("unknown mode"));
    REQUIRE_THROWS_WITH(call({{"mode", "scale"}, {"value", 4.5}}),
                        ContainsSubstring("scale value must be 0.5 to 4"));
    REQUIRE_THROWS_WITH(call({{"mode", "fixed"}, {"value", -0.1}}),
                        ContainsSubstring("fixed value must be 0.5 to 6"));
    REQUIRE_THROWS_WITH(call({{"mode", "fixed"}, {"value", 0}}), ContainsSubstring("hangs it"));
    // T9: scale 0.1 and 0.01 broke the game when left; only values tested both ways are taken.
    REQUIRE_THROWS_WITH(call({{"mode", "scale"}, {"value", 0.1}}), ContainsSubstring("0.5 to 4"));
    REQUIRE_THROWS_WITH(call({{"mode", "scale"}, {"value", 0.01}}), ContainsSubstring("left scale 0.1 or 0.01"));
    REQUIRE_THROWS_WITH(call({{"mode", "fixed"}, {"value", "1"}}),
                        ContainsSubstring("fixed value must be"));
    REQUIRE_THROWS(call({{"mode", "scale"}}));
    REQUIRE_THROWS_WITH(call({{"trace", -1}}), ContainsSubstring("trace is the presents to trace, a whole number, 1 to 36000"));
    REQUIRE_THROWS_WITH(call({{"trace", 36001}}), ContainsSubstring("1 to 36000"));
    REQUIRE_THROWS_WITH(call({{"trace", 2.5}}), ContainsSubstring("or 0 to stop"));
    // The first Publish takes what an earlier Apply in this process left pending, the second clears.
    FileSys::OotTimeControl::Publish("main", 0);
    FileSys::OotTimeControl::Publish("main", 0);
    REQUIRE_THROWS_WITH(call({{"mode", "scale"}, {"value", 0.5}}),
                        ContainsSubstring("EDEN_NX2_OOT_TIME_CONTROL=1"));
    REQUIRE_THROWS_WITH(call(json::object()), ContainsSubstring("not installed"));
}

namespace {

/// A guest trace area: magic, count, then the record of call i at slot i % 8.
struct TraceArea {
    std::array<u8, FileSys::OotTimeControl::TraceSize> bytes{};
    u32 count{};

    void Call(float s1, float s0, float own, float step) {
        const std::array<float, 4> values{s1, s0, own, step};
        std::memcpy(bytes.data() + 16 + (count % 8) * 16, values.data(), sizeof(values));
        ++count;
        std::memcpy(bytes.data() + 4, &count, sizeof(count));
    }
};

} // Anonymous namespace

TEST_CASE("The step trace stamps calls with their present and sums them up", "[nx2_trainer]") {
    using Core::Nx2Debug::StepTrace;
    TraceArea area;
    area.Call(60, 60, 1, 1); // before the arm
    StepTrace trace;
    trace.Arm(area.count, 4);
    area.Call(9, 9, 9, 9); // in progress at the arm: skipped
    area.Call(60, 60, 1, 1);
    area.Call(30, 60, 0.5f, 0.5f);
    REQUIRE(trace.Add(100, area.bytes));
    REQUIRE(trace.Add(101, area.bytes)); // a present without a call (a load)
    for (int i = 0; i < 10; ++i) {
        area.Call(60, 59.5f + 0.5f * (i % 2), 1, 1); // ten calls in one present: three dropped
    }
    REQUIRE(trace.Add(102, area.bytes));
    area.Call(30, 60, 0.5f, 0.25f);
    REQUIRE_FALSE(trace.Add(103, area.bytes)); // the fourth present: done
    const json summary = trace.Summary(true);
    REQUIRE(summary["presents"] == 4);
    REQUIRE(summary["calls"] == 10);
    REQUIRE(summary["dropped"] == 3);
    REQUIRE(summary["first_present"] == 100);
    REQUIRE(summary["last_present"] == 103);
    REQUIRE(summary["calls_per_present"] == json{{"0", 1}, {"1", 1}, {"2", 1}, {"10", 1}});
    REQUIRE(summary["s0"]["distinct"] == json{59.5f, 60.0f});
    REQUIRE(summary["s1"]["min"] == 30.0f);
    REQUIRE(summary["step"]["distinct"] == json{0.25f, 0.5f, 1.0f});
    REQUIRE(summary["own_step"]["distinct"] == json{0.5f, 1.0f});
    REQUIRE(summary["records"].size() == 10);
    REQUIRE(summary["records"][0] == json{100, 60.0f, 60.0f, 1.0f, 1.0f});
    REQUIRE(summary["records"][9] == json{103, 30.0f, 60.0f, 0.5f, 0.25f});
    REQUIRE_FALSE(trace.Summary(false).contains("records"));
}

TEST_CASE("The step trace drops records the guest overwrote during the copy", "[nx2_trainer]") {
    using Core::Nx2Debug::StepTrace;
    TraceArea area;
    StepTrace trace;
    trace.Arm(area.count, 10);
    for (int i = 0; i < 7; ++i) {
        area.Call(60, 60, 1, static_cast<float>(i)); // calls 0 (skipped) to 6
    }
    // Four more calls ran while the host copied: they overwrote calls 0 to 3 (and call 11 may be
    // writing over call 3), so of calls 1 to 6 only 4 to 6 are intact.
    REQUIRE(trace.Add(100, area.bytes, area.count + 4));
    const json summary = trace.Summary(true);
    REQUIRE(summary["calls"] == 3);
    REQUIRE(summary["dropped"] == 3);
    REQUIRE(summary["records"][0][4] == 4.0f);
    REQUIRE(summary["calls_per_present"] == json{{"6", 1}});
}

TEST_CASE("time_control ramps a speed-up over 2x by doublings, one a present", "[nx2_trainer]") {
    using Core::Nx2Debug::TimeRamp;
    REQUIRE(TimeRamp(1.0f, 6.0f) == std::vector<float>{2.0f, 4.0f}); // off -> fixed 6: 2, 4, then 6
    REQUIRE(TimeRamp(0.5f, 6.0f) == std::vector<float>{1.0f, 2.0f, 4.0f});
    REQUIRE(TimeRamp(0.5f, 1.0f).empty()); // scale 0.5 -> off: 2x, tested clean
    REQUIRE(TimeRamp(1.0f, 2.0f).empty());
    REQUIRE(TimeRamp(6.0f, 0.5f).empty()); // slowing down is never ramped
    // The T9 failure, a jump from scale 0.01 to off, would go through six stages, then off.
    REQUIRE(TimeRamp(0.01f, 1.0f).size() == 6);
    for (const auto& [from, to] : {std::pair{0.01f, 1.0f}, {0.5f, 6.0f}, {0.7f, 4.0f}}) {
        float last = from;
        for (const float stage : TimeRamp(from, to)) {
            REQUIRE(stage <= last * Core::Nx2Debug::TIME_RAMP_FACTOR * 1.0001f);
            last = stage;
        }
        REQUIRE(to <= last * Core::Nx2Debug::TIME_RAMP_FACTOR * 1.0001f);
    }
    REQUIRE(TimeRamp(0.0f, 1.0f).empty()); // no earlier speed: nothing to ramp from
}
