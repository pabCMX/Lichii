// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "common/nx2_live_switch.h"
#include "common/scope_exit.h"
#include "core/core.h"
#include "core/debugger/nx2_debug_tools.h"
#include "core/debugger/nx2_trainer_tools.h"
#include "core/file_sys/oot_time_control.h"
#include "tests/core/system_test_context.h"

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
                        ContainsSubstring("scale value must be 0 to 4"));
    REQUIRE_THROWS_WITH(call({{"mode", "fixed"}, {"value", -0.1}}),
                        ContainsSubstring("fixed value must be 0 to 6"));
    REQUIRE_THROWS_WITH(call({{"mode", "fixed"}, {"value", "1"}}),
                        ContainsSubstring("fixed value must be"));
    REQUIRE_THROWS(call({{"mode", "scale"}}));
    // The first Publish takes what an earlier Apply in this process left pending, the second clears.
    FileSys::OotTimeControl::Publish("main", 0);
    FileSys::OotTimeControl::Publish("main", 0);
    REQUIRE_THROWS_WITH(call({{"mode", "scale"}, {"value", 0.5}}),
                        ContainsSubstring("EDEN_NX2_OOT_TIME_CONTROL=1"));
    REQUIRE_THROWS_WITH(call(json::object()), ContainsSubstring("not installed"));
}
