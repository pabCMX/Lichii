// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>

#include "common/common_types.h"
#include "core/perf_stats.h"

/// The parts of the trainer overlay's frame-time bars panel that need no Qt, so the unit tests use
/// them. The panel shows one bar per present, all of the same width, the newest at the right.
/// Height and colour give the frame time on a fixed scale.
namespace TrainerHud {

/// The colour limits in ms. Each has a small margin over 1000/60 and 1000/30, so normal pacing
/// noise at 60 or 30 FPS keeps its colour.
constexpr double GREEN_MAX_MS = 17.5;  ///< up to this: fast enough for 60 FPS
constexpr double ORANGE_MAX_MS = 34.0; ///< up to this: slower than 60 FPS, fast enough for 30
/// The top of the fixed vertical scale. A longer frame is drawn at full height, marked clipped.
constexpr double SCALE_TOP_MS = 50.0;
/// The two target lines, drawn over the bars.
constexpr double LINE_60_MS = 1000.0 / 60.0;
constexpr double LINE_30_MS = 1000.0 / 30.0;

/// Bars in the panel: 2 s at 60 FPS.
constexpr std::size_t BAR_COUNT = 120;
/// The bar pitch and the gap between bars, in device-independent pixels: a 3 px bar, a 1 px gap.
constexpr double BAR_PITCH = 4.0;
constexpr double BAR_GAP = 1.0;

enum class BarColour { Green, Orange, Red };

constexpr BarColour ColourOf(double ms) {
    return ms <= GREEN_MAX_MS ? BarColour::Green
                              : (ms <= ORANGE_MAX_MS ? BarColour::Orange : BarColour::Red);
}

struct BarSize {
    int height;   ///< in pixels, at least 1
    bool clipped; ///< the frame is longer than SCALE_TOP_MS
};

/// A bar's height for a graph graph_height pixels high.
inline BarSize MeasureBar(double ms, int graph_height) {
    const bool clipped = ms > SCALE_TOP_MS;
    const double shown = std::clamp(ms, 0.0, SCALE_TOP_MS);
    const int height = int(std::lround(shown / SCALE_TOP_MS * graph_height));
    return {.height = std::clamp(height, 1, graph_height), .clipped = clipped};
}

/// The row of the line at ms, counted from the top of a graph graph_height pixels high. A bar of
/// exactly ms has its top pixel on this row.
inline int LineRow(double ms, int graph_height) {
    return graph_height - int(std::lround(ms / SCALE_TOP_MS * graph_height));
}

struct BarPixels {
    int pitch; ///< device pixels from one bar to the next
    int bar;   ///< device pixels of bar; the rest of the pitch is the gap
};

/// BAR_PITCH and BAR_GAP in whole device pixels at unit device pixels per layout unit (the game
/// image's scale times the device pixel ratio). The pitch is never under 2 px; the gap is at least
/// 1 px while the bar is 3 px or wider, so single frames stay apart.
inline BarPixels BarPixelsFor(double unit) {
    const int pitch = std::max(2, int(std::lround(BAR_PITCH * unit)));
    int gap = int(std::lround(BAR_GAP * unit));
    if (pitch - gap >= 3) {
        gap = std::max(gap, 1);
    }
    return {.pitch = pitch, .bar = pitch - gap};
}

/// A rectangle in pixels.
struct Box {
    int x;
    int y;
    int width;
    int height;
};

/// Layout units to pixels for a game image image_height pixels high: one unit is a pixel at 720p.
/// Both panels use it, so they grow together with the game image.
inline double PanelScale(int image_height) {
    return std::clamp(image_height / 720.0, 0.75, 4.0);
}

/// The bars panel in layout units.
constexpr double BARS_PADDING = 6.0;
constexpr double BARS_TEXT_HEIGHT = 16.0;
constexpr double BARS_TEXT_GAP = 4.0;
constexpr double BARS_GRAPH_HEIGHT = 100.0;
/// Right of the graph, for the "60" and "30" labels.
constexpr double BARS_LABEL_WIDTH = 20.0;

/// The bars panel in whole device pixels: graph is the bars' rectangle inside the panel.
struct BarsGeometry {
    BarPixels pixels;
    Box graph;
    int width;
    int height;
};

/// The bars panel at unit device pixels per layout unit. The graph is BAR_COUNT pitches wide.
inline BarsGeometry BarsGeometryFor(double unit) {
    const auto pixels_of = [unit](double units) { return int(std::lround(units * unit)); };
    const BarPixels pixels = BarPixelsFor(unit);
    const int padding = pixels_of(BARS_PADDING);
    const Box graph{.x = padding,
                    .y = padding + pixels_of(BARS_TEXT_HEIGHT + BARS_TEXT_GAP),
                    .width = int(BAR_COUNT) * pixels.pitch,
                    .height = pixels_of(BARS_GRAPH_HEIGHT)};
    return {.pixels = pixels,
            .graph = graph,
            .width = graph.x + graph.width + pixels_of(BARS_LABEL_WIDTH) + padding,
            .height = graph.y + graph.height + padding};
}

struct BarStats {
    std::size_t count;
    double mean_ms;
    double worst_ms;
};

/// The frame times the bars show, one slot per bar. Frame n (counted from the first push) is in
/// slot n % BAR_COUNT, so the overlay keeps an image with one column of bars per slot and on a new
/// frame redraws only that slot.
class FrameBars {
public:
    void Push(float ms) {
        frame_ms[pushed % BAR_COUNT] = ms;
        ++pushed;
    }

    void Clear() {
        pushed = 0;
    }

    [[nodiscard]] std::size_t Size() const {
        return std::size_t(std::min<u64>(pushed, BAR_COUNT));
    }

    [[nodiscard]] u64 Pushed() const {
        return pushed;
    }

    /// The slot drawn at the left edge. Until the panel is full, the slots from here to the end
    /// are empty, so the bars still end at the right edge.
    [[nodiscard]] std::size_t FirstSlot() const {
        return std::size_t(pushed % BAR_COUNT);
    }

    /// The bar index from the left edge (0 to BAR_COUNT - 1) where a slot is drawn.
    [[nodiscard]] std::size_t Position(std::size_t slot) const {
        return (slot + BAR_COUNT - FirstSlot()) % BAR_COUNT;
    }

    [[nodiscard]] bool Used(std::size_t slot) const {
        return pushed >= BAR_COUNT || slot < pushed;
    }

    [[nodiscard]] float operator[](std::size_t slot) const {
        return frame_ms[slot];
    }

    /// Over the bars shown.
    [[nodiscard]] BarStats Stats() const {
        const std::size_t count = Size();
        double sum = 0;
        double worst = 0;
        for (std::size_t i = 0; i < count; ++i) {
            sum += frame_ms[i];
            worst = std::max(worst, double(frame_ms[i]));
        }
        return {.count = count, .mean_ms = count != 0 ? sum / double(count) : 0, .worst_ms = worst};
    }

private:
    std::array<float, BAR_COUNT> frame_ms{};
    u64 pushed{};
};

/// The entries of a history copy (oldest first) that ended after last_end: the frames to add as
/// bars this tick. At most the BAR_COUNT newest, as older ones would leave the panel at once.
inline std::span<const Core::FrameTimeHistory::Entry> NewFrames(
    std::span<const Core::FrameTimeHistory::Entry> entries,
    Core::FrameTimeHistory::Clock::time_point last_end) {
    std::size_t first = entries.size();
    while (first != 0 && entries[first - 1].end > last_end && entries.size() - first < BAR_COUNT) {
        --first;
    }
    return entries.subspan(first);
}

/// Where the bars panel goes over the game image (all in pixels): centred along the bottom edge,
/// margin pixels above it like the input panel, and never left of the margin. When the input
/// panel shows (input) and the two would be closer than gap side by side, the bars panel goes
/// gap pixels above the input panel instead, so they never overlap.
inline Box PlaceBars(const Box& image, int width, int height, int margin,
                     const std::optional<Box>& input, int gap) {
    Box bars{.x = std::max(image.x + (image.width - width) / 2, image.x + margin),
             .y = image.y + image.height - margin - height,
             .width = width,
             .height = height};
    if (input && bars.x < input->x + input->width + gap) {
        bars.y = std::min(bars.y, input->y - gap - height);
    }
    return bars;
}

} // namespace TrainerHud
