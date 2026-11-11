// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

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
