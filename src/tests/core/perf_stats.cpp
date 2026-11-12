// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/perf_stats.h"

namespace {
using namespace std::chrono_literals;
using Core::FrameTimeHistory;
}

TEST_CASE("Frame time summary has min, median and max", "[core][perf_stats]") {
    REQUIRE_FALSE(Core::Summarize({}));

    const std::array<float, 5> odd{35.5f, 52.8f, 33.1f, 34.0f, 36.0f};
    const auto summary = Core::Summarize(odd);
    REQUIRE(summary);
    REQUIRE(summary->min_ms == 33.1f);
    REQUIRE(summary->median_ms == 35.5f);
    REQUIRE(summary->max_ms == 52.8f);

    const std::array<float, 4> even{20.0f, 10.0f, 40.0f, 30.0f};
    REQUIRE(Core::Summarize(even)->median_ms == 25.0f);
}

TEST_CASE("Frame time history keeps the newest window in order", "[core][perf_stats]") {
    FrameTimeHistory history;
    const auto start = FrameTimeHistory::Clock::time_point{};
    REQUIRE(history.Recent(5s).empty());

    // Wraps the ring: only the last Capacity entries survive.
    const std::size_t pushed = FrameTimeHistory::Capacity + 10;
    for (std::size_t i = 0; i < pushed; ++i) {
        history.Push(start + i * 1ms, static_cast<float>(i));
    }
    const auto all = history.Recent(1h);
    REQUIRE(all.size() == FrameTimeHistory::Capacity);
    REQUIRE(all.front().ms == 10.0f);
    REQUIRE(all.back().ms == static_cast<float>(pushed - 1));

    // The window is measured back from the newest entry, not from now.
    const auto recent = history.Recent(100ms);
    REQUIRE(recent.size() == 100);
    REQUIRE(recent.front().ms == static_cast<float>(pushed - 100));

    std::vector<float> ms;
    for (const auto& entry : recent) {
        ms.push_back(entry.ms);
    }
    const auto summary = Core::Summarize(ms);
    REQUIRE(summary->min_ms == static_cast<float>(pushed - 100));
    REQUIRE(summary->max_ms == static_cast<float>(pushed - 1));
    REQUIRE(summary->median_ms == static_cast<float>(pushed - 50) - 0.5f);

    history.Clear();
    REQUIRE(history.Recent(1h).empty());
}

TEST_CASE("Frame time history records while any reader wants it", "[core][perf_stats]") {
    using Reader = Core::PerfStats::FrameTimeReader;
    // On the heap: it holds an hour of frame times (1.7 MB).
    const auto stats_owner = std::make_unique<Core::PerfStats>(0);
    Core::PerfStats& stats = *stats_owner;
    const auto frame = [&] {
        stats.BeginSystemFrame();
        stats.EndSystemFrame();
    };
    frame();
    REQUIRE(stats.GetRecentFrameTimes(1h).empty());

    stats.SetFrameTimeHistoryEnabled(true, Reader::TrainerBars);
    frame();
    // The performance overlay sets "off" at each stats update while it is hidden.
    stats.SetFrameTimeHistoryEnabled(false);
    frame();
    REQUIRE(stats.GetRecentFrameTimes(1h).size() == 2);

    // A second reader keeps the entries; the last one to stop stops the recording.
    stats.SetFrameTimeHistoryEnabled(true);
    frame();
    REQUIRE(stats.GetRecentFrameTimes(1h).size() == 3);
    stats.SetFrameTimeHistoryEnabled(false, Reader::TrainerBars);
    frame();
    REQUIRE(stats.GetRecentFrameTimes(1h).size() == 4);
    stats.SetFrameTimeHistoryEnabled(false);
    frame();
    REQUIRE(stats.GetRecentFrameTimes(1h).size() == 4);

    // Starting again with no reader clears the old entries.
    stats.SetFrameTimeHistoryEnabled(true, Reader::TrainerBars);
    REQUIRE(stats.GetRecentFrameTimes(1h).empty());
}
