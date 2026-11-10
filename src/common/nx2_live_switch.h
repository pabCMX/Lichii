// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstdlib>

namespace Common::Nx2Live {

/// An on/off EDEN_NX2_* switch that the debug server's "switches" method can flip while the game
/// runs (docs/nx2/trainer-kit.md). The environment gives the start value. Reading it is one
/// relaxed load: a plain load on x86 and ARM, no more than the function-local
/// `static const bool` it replaces, which also tests its guard on every call.
class Switch {
public:
    explicit Switch(const char* env_) : env{env_}, on{std::getenv(env_) != nullptr} {}

    bool operator()() const {
        return on.load(std::memory_order_relaxed);
    }

    void Set(bool value) {
        on.store(value, std::memory_order_relaxed);
    }

    /// The value from the environment again (nx2_switches.txt loads after static initialization).
    void ReadEnv() {
        Set(std::getenv(env) != nullptr);
    }

    const char* const env;

private:
    std::atomic<bool> on;
};

inline Switch g_ipc_trace{"EDEN_NX2_IPC_TRACE"};
inline Switch g_ipc_summary{"EDEN_NX2_IPC_SUMMARY"};
inline Switch g_vdec_trace{"EDEN_NX2_VDEC_TRACE"};
inline Switch g_render_trace{"EDEN_NX2_RENDER_TRACE"};

/// Called by VideoCore::CreateGPU at each boot, once nx2_switches.txt is applied. It also drops
/// live changes from the last boot.
inline void ReadEnv() {
    for (Switch* const entry : {&g_ipc_trace, &g_ipc_summary, &g_vdec_trace, &g_render_trace}) {
        entry->ReadEnv();
    }
}

} // namespace Common::Nx2Live
