// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "common/scope_exit.h"
#include "core/core.h"
#include "core/debugger/nx2_debug_tools.h"
#include "hid_core/hid_core.h"
#include "tests/core/system_test_context.h"

TEST_CASE("The trainer overlay reads what the game is given from both controllers",
          "[core][nx2_debug_server]") {
    using namespace Core::HID;
    ScopedSystemTestContext context;
    Core::System system;
    auto& player1 = *system.HIDCore().GetEmulatedController(NpadIdType::Player1);
    auto& handheld = *system.HIDCore().GetEmulatedController(NpadIdType::Handheld);
    player1.SetNpadStyleIndex(NpadStyleIndex::Fullkey);
    player1.Connect();
    const u64 old_count = Core::g_nx2_present_count.load();
    SCOPE_EXIT {
        Core::g_nx2_present_count = old_count;
        player1.SetInputInjector({});
        handheld.SetInputInjector({});
        player1.Disconnect();
        handheld.Disconnect();
    };
    Core::g_nx2_present_count = 41;

    player1.SetInputInjector([] {
        return std::optional{InjectedInput{.buttons = NpadButton::A | NpadButton::Plus,
                                           .left = AnalogStickState{.x = 1000, .y = -2000},
                                           .right = AnalogStickState{.x = 0, .y = 30000}}};
    });
    // Not connected: its input is not the game's.
    handheld.SetInputInjector([] {
        return std::optional{InjectedInput{
            .buttons = NpadButton::ZL, .left = AnalogStickState{.x = 20000, .y = 0}, .right = {}}};
    });
    auto view = Core::Nx2Debug::ReadGamePad(system);
    REQUIRE(view.present == 41);
    REQUIRE((view.buttons & NpadButton::A) == NpadButton::A);
    REQUIRE((view.buttons & NpadButton::Plus) == NpadButton::Plus);
    REQUIRE((view.buttons & NpadButton::ZL) == NpadButton::None);
    REQUIRE(view.left.x == 1000);
    REQUIRE(view.left.y == -2000);
    REQUIRE(view.right.y == 30000);
    REQUIRE_FALSE(view.recording);
    REQUIRE_FALSE(view.replaying);

    // Both connected: buttons ORed, each stick the larger deflection.
    handheld.SetNpadStyleIndex(NpadStyleIndex::Handheld);
    handheld.Connect();
    view = Core::Nx2Debug::ReadGamePad(system);
    REQUIRE((view.buttons & (NpadButton::A | NpadButton::ZL)) == (NpadButton::A | NpadButton::ZL));
    REQUIRE(view.left.x == 20000);
    REQUIRE(view.right.y == 30000);
}

TEST_CASE("The trainer overlay's REC count is the recorder's own", "[core][nx2_debug_server]") {
    using namespace Core::HID;
    ScopedSystemTestContext context;
    Core::System system;
    auto& player1 = *system.HIDCore().GetEmulatedController(NpadIdType::Player1);
    player1.SetNpadStyleIndex(NpadStyleIndex::Fullkey);
    player1.Connect();
    // A stick that moves at every present, so each present is its own step.
    player1.SetInputInjector([] {
        const auto x = s32(Core::g_nx2_present_count.load() % 20000);
        return std::optional{
            InjectedInput{.buttons = {}, .left = AnalogStickState{.x = x, .y = 0}, .right = {}}};
    });
    const u64 old_count = Core::g_nx2_present_count.load();
    const auto record = [&](const char* action) {
        return Core::Nx2Debug::CallTool(system, "input_record", {{"action", action}});
    };
    SCOPE_EXIT {
        if (Core::Nx2Debug::ReadGamePad(system).recording) {
            static_cast<void>(record("stop"));
        }
        player1.SetInputInjector({});
        player1.Disconnect();
        Core::g_nx2_present_count = old_count;
    };
    const auto present = [] {
        const u64 count = ++Core::g_nx2_present_count;
        if (const auto hook = Core::g_nx2_present_hook.load()) {
            hook(count);
        }
        if (const auto reply = Core::g_nx2_present_reply_hook.load()) {
            reply(count);
        }
    };

    static_cast<void>(record("start"));
    for (int i = 0; i < 3; ++i) {
        present();
    }
    auto view = Core::Nx2Debug::ReadGamePad(system);
    REQUIRE(view.recording);
    REQUIRE(view.recorded == record("status")["presents"].get<u64>());
    REQUIRE(view.recorded == 3);

    // A stop and a start between two samples: the count is the new recording's.
    static_cast<void>(record("stop"));
    static_cast<void>(record("start"));
    present();
    view = Core::Nx2Debug::ReadGamePad(system);
    REQUIRE(view.recorded == 1);
    REQUIRE_FALSE(view.record_full);

    // Past its step limit the recorder stops counting, and so does the mark.
    for (int i = 0; i < 6100; ++i) {
        present();
    }
    view = Core::Nx2Debug::ReadGamePad(system);
    REQUIRE(view.record_full);
    REQUIRE(view.recorded == record("status")["presents"].get<u64>());
    const u64 full_at = view.recorded;
    present();
    REQUIRE(Core::Nx2Debug::ReadGamePad(system).recorded == full_at);
}

TEST_CASE("The trainer overlay shows WAIT before a scheduled clip and REPLAY only during it",
          "[core][nx2_debug_server]") {
    using namespace Core::HID;
    ScopedSystemTestContext context;
    Core::System system;
    auto& player1 = *system.HIDCore().GetEmulatedController(NpadIdType::Player1);
    player1.SetNpadStyleIndex(NpadStyleIndex::Fullkey);
    player1.Connect();
    const u64 old_count = Core::g_nx2_present_count.load();
    SCOPE_EXIT {
        player1.Disconnect();
        Core::g_nx2_present_count = old_count;
    };
    const u64 start = Core::g_nx2_present_count.load() + 5;
    std::thread call{[&] {
        static_cast<void>(Core::Nx2Debug::CallTool(
            system, "input_sequence",
            {{"steps", {{{"buttons", {"A"}}, {"frames", 3}}}}, {"start_present", start}}));
    }};
    SCOPE_EXIT {
        // Ends the call if a check failed early.
        if (call.joinable()) {
            Core::g_nx2_present_count = start + 10;
            call.join();
        }
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (Core::Nx2Debug::ReadGamePad(system).replay_wait == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    auto view = Core::Nx2Debug::ReadGamePad(system);
    REQUIRE(view.replay_wait == 5);
    REQUIRE_FALSE(view.replaying);
    REQUIRE((view.buttons & NpadButton::A) == NpadButton::None);

    Core::g_nx2_present_count = start - 1;
    REQUIRE(Core::Nx2Debug::ReadGamePad(system).replay_wait == 1);
    Core::g_nx2_present_count = start;
    view = Core::Nx2Debug::ReadGamePad(system);
    REQUIRE(view.replaying);
    REQUIRE(view.replay_wait == 0);
    REQUIRE((view.buttons & NpadButton::A) == NpadButton::A);
    Core::g_nx2_present_count = start + 2;
    REQUIRE(Core::Nx2Debug::ReadGamePad(system).replaying);
    // After the last step: neither mark.
    Core::g_nx2_present_count = start + 3;
    call.join();
    view = Core::Nx2Debug::ReadGamePad(system);
    REQUIRE_FALSE(view.replaying);
    REQUIRE(view.replay_wait == 0);
}
