// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cmath>
#include <utility>
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

TEST_CASE("Bar pitch scales to whole device pixels with a gap", "[trainer_hud]") {
    const auto check = [](double unit, int pitch, int bar) {
        const BarPixels pixels = BarPixelsFor(unit);
        REQUIRE(pixels.pitch == pitch);
        REQUIRE(pixels.bar == bar);
    };
    check(0.75, 3, 2); // the smallest image scale: still a gap
    check(1.0, 4, 3);  // 720p
    check(1.5, 6, 4);  // 1080p, or 720p at a device pixel ratio of 1.5
    check(2.0, 8, 6);  // 1440p
    check(3.0, 12, 9); // 2160p
    check(1.1, 4, 3);  // a gap under half a pixel still keeps 1 px
    // Never under 2 px.
    REQUIRE(BarPixelsFor(0.25).pitch == 2);
}

TEST_CASE("The bars panel scales with the game image", "[trainer_hud]") {
    REQUIRE(PanelScale(450) == 0.75);
    REQUIRE(PanelScale(720) == 1.0);
    REQUIRE(PanelScale(2160) == 3.0);
    REQUIRE(PanelScale(4320) == 4.0);

    const BarsGeometry one = BarsGeometryFor(1.0);
    REQUIRE(one.graph.width == 480);
    REQUIRE(one.graph.height == 100);
    REQUIRE(one.width == 512);
    REQUIRE(one.height == 132);
    const BarsGeometry three = BarsGeometryFor(3.0);
    REQUIRE(three.graph.width == int(BAR_COUNT) * 12);
    REQUIRE(three.graph.height == 300);
    REQUIRE(three.width == 1536);
    REQUIRE(three.height == 396);
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

namespace {

/// count presents at 30 FPS, the first one ending at first_ms.
std::vector<Entry> Presents(int first_ms, int count) {
    std::vector<Entry> entries;
    for (int i = 0; i < count; ++i) {
        entries.push_back({Clock::time_point{} + std::chrono::milliseconds{first_ms + i * 33}, 33.0f});
    }
    return entries;
}

std::vector<Entry> Join(std::vector<Entry> a, const std::vector<Entry>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

} // Anonymous namespace

TEST_CASE("New bars are the entries after the last bar, in order", "[trainer_hud]") {
    const std::vector<Entry> entries = Presents(1000, 5);
    // First tick: everything, no restart.
    const BarsUpdate first = UpdateBars(entries, std::nullopt);
    REQUIRE_FALSE(first.restart);
    REQUIRE(first.fresh.size() == 5);
    // Two presents since the bar that ended at the third entry: both, in order.
    const BarsUpdate two = UpdateBars(entries, entries[2].end);
    REQUIRE_FALSE(two.restart);
    REQUIRE(two.fresh.size() == 2);
    REQUIRE(two.fresh[0].end == entries[3].end);
    REQUIRE(two.fresh[1].end == entries[4].end);
    REQUIRE(UpdateBars(entries, entries[4].end).fresh.empty());
    // More than the panel holds: only the newest BAR_COUNT, still no restart.
    const std::vector<Entry> many = Presents(1000, 300);
    const BarsUpdate capped = UpdateBars(many, many[0].end);
    REQUIRE_FALSE(capped.restart);
    REQUIRE(capped.fresh.size() == BAR_COUNT);
    REQUIRE(capped.fresh.back().end == many.back().end);
}

TEST_CASE("Bars lost while the GUI thread stalls start the panel again, never a silent join",
          "[trainer_hud]") {
    // 120 bars shown, the last one ends at shown.back(). Then the GUI thread stalls 3 s while the
    // game presents at 30 FPS: 90 presents.
    const std::vector<Entry> shown = Presents(0, 120);
    const auto last = shown.back().end;
    const std::vector<Entry> stall = Presents(120 * 33, 90);

    // A 2 s copy holds only the last 60 or so: the last bar is not in it, so it is a gap.
    const std::vector<Entry> window(stall.end() - 60, stall.end());
    const BarsUpdate cut = UpdateBars(window, last);
    REQUIRE(cut.restart);
    REQUIRE(cut.fresh.size() == 60);

    // The whole history still holds the last bar: all 90 go on after it.
    const BarsUpdate whole = UpdateBars(Join(shown, stall), last);
    REQUIRE_FALSE(whole.restart);
    REQUIRE(whole.fresh.size() == 90);
    REQUIRE(whole.fresh.front().end == stall.front().end);

    // The history overflowed (2048 entries, all after the last bar): start again from the newest.
    const BarsUpdate overflow =
        UpdateBars(Presents(120 * 33 + 5000, int(Core::FrameTimeHistory::Capacity)), last);
    REQUIRE(overflow.restart);
    REQUIRE(overflow.fresh.size() == BAR_COUNT);
}

TEST_CASE("A history that started again resets the bars; a second reader does not",
          "[trainer_hud]") {
    const std::vector<Entry> shown = Presents(0, 120);
    const auto last = shown.back().end;
    // Emptied (a new game's history before its first present).
    REQUIRE(UpdateBars({}, last).restart);
    // Its newest entry is older than the last bar.
    REQUIRE(UpdateBars(std::span{shown}.first(50), last).restart);
    // A new game's entries, all after the last bar: the last bar is not among them.
    const BarsUpdate next_game = UpdateBars(Presents(10'000, 30), last);
    REQUIRE(next_game.restart);
    REQUIRE(next_game.fresh.size() == 30);
    // The performance overlay starts reading too: the history is not cleared, so the bars go on.
    const BarsUpdate second_reader = UpdateBars(Join(shown, Presents(120 * 33, 2)), last);
    REQUIRE_FALSE(second_reader.restart);
    REQUIRE(second_reader.fresh.size() == 2);
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

TEST_CASE("Scaled panels sit side by side on 16:9 images and stack on small ones",
          "[trainer_hud]") {
    // As the overlay places them at a device pixel ratio of 1: the input panel from PanelRect.
    const auto place = [](int width, int height) {
        const Box image{.x = 0, .y = 0, .width = width, .height = height};
        const double scale = PanelScale(height);
        const int margin = int(std::lround(16.0 * scale));
        const Box input{.x = margin,
                        .y = height - margin - int(std::lround(170.0 * scale)),
                        .width = int(std::lround(344.0 * scale)),
                        .height = int(std::lround(170.0 * scale))};
        const BarsGeometry geometry = BarsGeometryFor(scale);
        const Box bars = PlaceBars(image, geometry.width, geometry.height, margin, input,
                                   int(std::lround(8.0 * scale)));
        // Never overlapping: beside the input panel or above it.
        REQUIRE((bars.x >= input.x + input.width || bars.y + bars.height <= input.y));
        return std::pair{bars, input};
    };
    for (const auto& [width, height] : {std::pair{1280, 720}, std::pair{1920, 1080},
                                       std::pair{2560, 1440}, std::pair{3840, 2160}}) {
        const auto [bars, input] = place(width, height);
        const double scale = PanelScale(height);
        REQUIRE(bars.x == (width - bars.width) / 2);
        REQUIRE(bars.y + bars.height == height - int(std::lround(16.0 * scale)));
        REQUIRE(bars.width == int(std::lround(512 * scale)));
    }
    const auto [stacked, input] = place(800, 450);
    REQUIRE(stacked.y + stacked.height < input.y);
}
