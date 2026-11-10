// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <span>
#include <string_view>

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

/// Adds the trainer methods to the server's table. stopping says that the session ends, so a
/// waiting method returns early.
void AddTrainerHandlers(Handlers& handlers, System& system, std::function<bool()> stopping);

/// The run state the methods read: System::IsPoweredOn and IsPaused, or a test's own.
struct RunState {
    std::function<bool()> powered_on;
    std::function<bool()> paused;
};

/// As above with a given run state (for tests).
void AddTrainerHandlers(Handlers& handlers, System& system, RunState run,
                        std::function<bool()> stopping);

} // namespace Core::Nx2Debug
