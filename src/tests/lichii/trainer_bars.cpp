// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "lichii/render/trainer_bars.h"

using namespace TrainerHud;
using Entry = Core::FrameTimeHistory::Entry;
using Clock = Core::FrameTimeHistory::Clock;

TEST_CASE("Bar colours: green to 17.5 ms, orange to 34.0 ms, red above", "[trainer_hud]") {
    REQUIRE(ColourOf(8.3) == BarColour::Green);
    REQUIRE(ColourOf(16.9) == BarColour::Green); // 60 FPS pacing noise
    REQUIRE(ColourOf(17.5) == BarColour::Green);
    REQUIRE(ColourOf(17.6) == BarColour::Orange);
    REQUIRE(ColourOf(33.9) == BarColour::Orange);
    REQUIRE(ColourOf(34.0) == BarColour::Orange);
    REQUIRE(ColourOf(34.1) == BarColour::Red);
    REQUIRE(ColourOf(120.0) == BarColour::Red);
}

TEST_CASE("Bars use a fixed 50 ms scale and clip longer frames", "[trainer_hud]") {
    REQUIRE(MeasureBar(25.0, 100).height == 50);
    REQUIRE_FALSE(MeasureBar(25.0, 100).clipped);
    REQUIRE(MeasureBar(50.0, 100).height == 100);
    REQUIRE_FALSE(MeasureBar(50.0, 100).clipped);
    REQUIRE(MeasureBar(120.0, 100).height == 100);
    REQUIRE(MeasureBar(120.0, 100).clipped);
    REQUIRE(MeasureBar(0.1, 100).height == 1); // every frame shows
    // A frame exactly on a target line has its top pixel on the line's row.
    REQUIRE(LineRow(LINE_60_MS, 100) == 100 - MeasureBar(LINE_60_MS, 100).height);
    REQUIRE(LineRow(LINE_30_MS, 150) == 150 - MeasureBar(LINE_30_MS, 150).height);
}

TEST_CASE("Bar pitch is whole device pixels with a gap", "[trainer_hud]") {
    REQUIRE(BarPixelsFor(1.0).pitch == 4);
    REQUIRE(BarPixelsFor(1.0).bar == 3);
    REQUIRE(BarPixelsFor(1.5).pitch == 6);
    REQUIRE(BarPixelsFor(1.5).bar == 4); // the gap rounds to 2
    REQUIRE(BarPixelsFor(2.0).pitch == 8);
    REQUIRE(BarPixelsFor(2.0).bar == 6);
}

TEST_CASE("Frame bars keep the last 120 frames, newest at the right", "[trainer_hud]") {
    FrameBars bars;
    REQUIRE(bars.Size() == 0);
    REQUIRE(bars.Stats().count == 0);

    bars.Push(10.0f);
    bars.Push(30.0f);
    REQUIRE(bars.Size() == 2);
    // Not full: the newest bar is at the right edge, the empty slots at the left.
    REQUIRE(bars.Position(1) == BAR_COUNT - 1);
    REQUIRE(bars.Position(0) == BAR_COUNT - 2);
    REQUIRE_FALSE(bars.Used(2));
    const BarStats two = bars.Stats();
    REQUIRE(two.mean_ms == Catch::Approx(20.0));
    REQUIRE(two.worst_ms == Catch::Approx(30.0));

    for (int i = 0; i < 130; ++i) {
        bars.Push(float(i));
    }
    REQUIRE(bars.Size() == BAR_COUNT);
    REQUIRE(bars.Pushed() == 132);
    // Frame 131 (value 129) is in slot 131 % 120 = 11, at the right edge.
    REQUIRE(bars[11] == 129.0f);
    REQUIRE(bars.Position(11) == BAR_COUNT - 1);
    REQUIRE(bars.Position(bars.FirstSlot()) == 0);
    REQUIRE(bars[bars.FirstSlot()] == 10.0f); // the oldest shown: values 10 to 129
    REQUIRE(bars.Stats().worst_ms == Catch::Approx(129.0));
    REQUIRE(bars.Stats().mean_ms == Catch::Approx(69.5));

    bars.Clear();
    REQUIRE(bars.Size() == 0);
}

TEST_CASE("New frames are the entries after the last drawn one", "[trainer_hud]") {
    using namespace std::chrono_literals;
    const Clock::time_point start{};
    std::vector<Entry> entries;
    for (int i = 1; i <= 5; ++i) {
        entries.push_back({start + i * 16ms, 16.0f + float(i)});
    }
    // First tick: everything.
    REQUIRE(NewFrames(entries, Clock::time_point{}).size() == 5);
    // Two presents arrived since the frame that ended at 48 ms: both, in order.
    const auto fresh = NewFrames(entries, start + 48ms);
    REQUIRE(fresh.size() == 2);
    REQUIRE(fresh[0].ms == 20.0f);
    REQUIRE(fresh[1].ms == 21.0f);
    REQUIRE(NewFrames(entries, start + 80ms).empty());

    // More than the panel holds: only the newest BAR_COUNT.
    std::vector<Entry> many;
    for (int i = 1; i <= 300; ++i) {
        many.push_back({start + i * 1ms, float(i)});
    }
    const auto capped = NewFrames(many, Clock::time_point{});
    REQUIRE(capped.size() == BAR_COUNT);
    REQUIRE(capped.front().ms == float(300 - BAR_COUNT + 1));
}

TEST_CASE("The bars panel is centred at the bottom, above the input panel if they collide",
          "[trainer_hud]") {
    const Box image{.x = 100, .y = 50, .width = 1280, .height = 720};
    const Box input{.x = 116, .y = 50 + 720 - 16 - 170, .width = 344, .height = 170};

    // Alone: centred, 16 px above the bottom.
    const Box alone = PlaceBars(image, 514, 132, 16, std::nullopt, 8);
    REQUIRE(alone.x == 100 + (1280 - 514) / 2);
    REQUIRE(alone.y == 50 + 720 - 16 - 132);

    // With the input panel on a wide image: side by side, same bottom margin.
    const Box wide = PlaceBars(image, 514, 132, 16, input, 8);
    REQUIRE(wide.x == alone.x);
    REQUIRE(wide.y == alone.y);
    REQUIRE(wide.x >= input.x + input.width + 8);

    // Narrow image: the centred place would cover the input panel, so it goes above it.
    const Box narrow_image{.x = 0, .y = 0, .width = 800, .height = 450};
    const Box narrow_input{.x = 12, .y = 450 - 12 - 128, .width = 258, .height = 128};
    const Box above = PlaceBars(narrow_image, 514, 132, 12, narrow_input, 6);
    REQUIRE(above.x == (800 - 514) / 2);
    REQUIRE(above.y + above.height + 6 == narrow_input.y);

    // Narrower than the panel: it keeps the left margin.
    const Box tiny =
        PlaceBars({.x = 0, .y = 0, .width = 400, .height = 300}, 514, 132, 12, std::nullopt, 6);
    REQUIRE(tiny.x == 12);
}
