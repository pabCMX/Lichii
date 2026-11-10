// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "common/common_types.h"

/// Breadcrumbs at the blocking points of the lab harness kit (docs/nx2/debug-mcp.md, "If the kit
/// hangs"). Each thread notes, lock-free, what it waits for or holds and since when, so the debug
/// server's watchdog can name a stall: the thread, the lock, the operation and the holder.
namespace Common::KitStall {

/// False when EDEN_NX2_KIT_HOOKS=0 (read once): the debug server then installs none of the kit's
/// hooks. SetHooksEnabled is for tests.
[[nodiscard]] bool HooksEnabled() noexcept;
void SetHooksEnabled(bool enabled) noexcept;

enum class State : u8 { Waits, Holds };

/// Marks the calling thread as waiting for `lock` in `op` until Held(), then as holding it, until
/// the scope ends. Scopes nest up to 4 deep per thread; deeper ones are not noted. lock and op
/// must be string literals.
class Scope {
public:
    Scope(const char* lock, const char* op) noexcept;
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

    void Held() noexcept;

private:
    int index;
};

/// One mark older than the limit, as the watchdog reports it.
struct Stall {
    std::string text;
    /// Names this mark apart from others, for repeat suppression.
    u64 key;
};

/// The marks that waited or held longer than limit_ns at now_ns (steady-clock ns), each with the
/// holder of the same lock when another thread holds it.
[[nodiscard]] std::vector<Stall> Check(u64 limit_ns, u64 now_ns);

/// Steady-clock now in ns, the clock of the marks.
[[nodiscard]] u64 NowNs() noexcept;

/// Called once at the first stall the watchdog reports, while set: for a frontend that can write
/// a minidump. Nothing sets it on the Base.
extern std::atomic<void (*)()> g_first_stall_hook;

} // namespace Common::KitStall
