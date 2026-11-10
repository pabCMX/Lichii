// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <functional>
#include <map>
#include <set>
#include <span>
#include <utility>
#include <string_view>
#include <vector>

#include "core/debugger/nx2_debug_server.h"

namespace Core {
class System;
}

/// Trainer methods of the debug server (docs/nx2/trainer-kit.md): pause, resume, step_frames,
/// time_control and switches.
namespace Core::Nx2Debug {

/// Pauses (run false) or resumes (run true) emulation the way the frontend's own pause button
/// does, so its menus stay in step. It may return before the change; the methods then wait for
/// System::IsPaused.
using RunControl = std::function<void(bool run)>;

/// The frontend registers its run control at startup and clears it ({}) before it is destroyed.
/// Thread-safe: a clear waits for a call in progress.
void SetRunControl(RunControl control);

/// One live switch: its method name, its environment variable and its class in the
/// docs/nx2/debug-mcp.md table.
struct LiveSwitchInfo {
    std::string_view name;
    std::string_view env;
    std::string_view kind;
};

/// The switches the "switches" method can flip, in the order it lists them.
[[nodiscard]] std::span<const LiveSwitchInfo> LiveSwitches();

/// Requests a frame capture (the next frame the GPU finishes) and returns its end: end(false)
/// waits and answers as capture_frame does, end(true) withdraws the request. step_frames
/// {capture} calls it while the game is paused.
using CaptureStart =
    std::function<std::function<nlohmann::json(bool cancel)>(const nlohmann::json& params)>;

/// Adds the trainer methods to the server's table. stopping says that the session ends, so a
/// waiting method returns early.
void AddTrainerHandlers(Handlers& handlers, System& system, std::function<bool()> stopping,
                        CaptureStart capture = {});

/// The run state the methods read: System::IsPoweredOn and IsPaused, or a test's own.
struct RunState {
    std::function<bool()> powered_on;
    std::function<bool()> paused;
};

/// As above with a given run state (for tests).
void AddTrainerHandlers(Handlers& handlers, System& system, RunState run,
                        std::function<bool()> stopping, CaptureStart capture = {});

/// The host side of the time_control step trace (docs/nx2/trainer-kit.md, "Step trace"). At each
/// present the present hook gives Add the guest trace area (OotTimeControl::TraceSize bytes:
/// magic, count, then the last 8 calls); Add keeps the calls made since the previous present,
/// stamped with this present. Not thread-safe: the trainer tools hold a lock around it.
class StepTrace {
public:
    static constexpr size_t KeptRecords = 256; ///< records Summary lists, the first ones
    static constexpr size_t MaxDistinct = 32;  ///< distinct values listed per field

    /// Starts a trace of `presents` presents. count is the guest count at the arm: the call in
    /// progress then may be half written, so the first call is skipped.
    void Arm(u32 count, u64 presents);
    /// One present's copy of the trace area. Returns false once the trace has its presents (the
    /// caller then stops the guest side).
    bool Add(u64 present, std::span<const u8> area);
    void Stop() {
        armed = false;
    }
    [[nodiscard]] bool Armed() const {
        return armed;
    }
    /// {armed, presents, wanted, calls, dropped, first_present, last_present, calls_per_present,
    /// s1, s0, own_step, step}, each value field {min, max, distinct, more_distinct, nan}; with
    /// records, the first KeptRecords as [present, s1, s0, own_step, step].
    [[nodiscard]] nlohmann::json Summary(bool records) const;

private:
    struct Field {
        float min{};
        float max{};
        std::set<float> distinct;
        bool more{};
        u64 nan{}; ///< NaN values: counted here, not in min, max or distinct
        bool seen{};
        void Add(float value);
    };
    bool armed{};
    u32 next{};
    u64 wanted{};
    u64 presents{};
    u64 calls{};
    u64 dropped{};
    u64 first_present{};
    u64 last_present{};
    std::map<u32, u64> per_present; ///< calls in one present -> presents
    std::array<Field, 4> fields{};  ///< s1, s0, own step, step returned
    std::vector<std::pair<u64, std::array<float, 4>>> kept; ///< present, the four fields
};

} // namespace Core::Nx2Debug
