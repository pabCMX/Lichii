// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "lichii/render/trainer_input.h"

using TrainerHud::ReadStick;

TEST_CASE("Stick angles are compass degrees: 0 up, clockwise", "[trainer_hud]") {
    constexpr s32 MAX = 32767;
    REQUIRE(ReadStick(0, MAX).angle == 0);
    REQUIRE(ReadStick(MAX, 0).angle == 90);
    REQUIRE(ReadStick(0, -MAX).angle == 180);
    REQUIRE(ReadStick(-MAX, 0).angle == 270);
    REQUIRE(ReadStick(20000, 20000).angle == 45);
    REQUIRE(ReadStick(-20000, 20000).angle == 315);
    // Just left of up rounds to 0, never to 360.
    REQUIRE(ReadStick(-10, MAX).angle == 0);
    REQUIRE(ReadStick(0, 0).angle == -1);
    REQUIRE(ReadStick(100, 0).angle == -1); // under 0.005: centred
}

TEST_CASE("Stick magnitude is the length over a full deflection", "[trainer_hud]") {
    using Catch::Approx;
    REQUIRE(ReadStick(0, 0).magnitude == 0.0);
    REQUIRE(ReadStick(0, -32767).magnitude == Approx(1.0));
    REQUIRE(ReadStick(16384, 0).magnitude == Approx(0.5).margin(0.0001));
    REQUIRE(ReadStick(-23170, 23170).magnitude == Approx(1.0).margin(0.0001));
    // A square corner passes the rim.
    REQUIRE(ReadStick(32767, 32767).magnitude == Approx(1.4142).margin(0.0001));
}

TEST_CASE("The stick trail keeps the newest points and drops old ones", "[trainer_hud]") {
    TrainerHud::StickTrail trail;
    for (s64 i = 0; i < 70; ++i) {
        trail.Push({.x = s32(i), .y = 0, .ms = i * 10});
    }
    REQUIRE(trail.Size() == TrainerHud::StickTrail::CAPACITY);
    REQUIRE(trail[0].x == 6); // 0 to 5 were overwritten
    REQUIRE(trail[trail.Size() - 1].x == 69);

    trail.Expire(690, 500); // keeps 190 ms and later
    REQUIRE(trail.Size() == 51);
    REQUIRE(trail[0].x == 19);

    trail.Expire(10'000, 500);
    REQUIRE(trail.Size() == 0);
    trail.Push({.x = 1, .y = 2, .ms = 10'000});
    REQUIRE(trail.Size() == 1);
    REQUIRE(trail[0].y == 2);
    trail.Clear();
    REQUIRE(trail.Size() == 0);
}
