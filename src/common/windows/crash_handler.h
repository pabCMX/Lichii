// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <optional>

namespace Common::Windows {

/// On an unhandled exception, abort() or a CRT invalid-parameter failure, logs one Critical "Host
/// crash" line (code, faulting address and stack as module+offset, thread), flushes the log and
/// writes <dump_directory>/eden_crash.dmp. Call after Common::Log::Initialize.
void InstallCrashHandler(const std::filesystem::path& dump_directory);

/// Best effort: writes <dump_directory>/eden_<reason>.dmp now with the stacks of every thread
/// except the calling one. Used before the frontend ends the process over a stuck thread, so the
/// dump shows where that thread waits. A helper process (this executable, see RunDumpHelper) writes
/// the dump and suspends the other threads of this process while it does. Memory that cannot be
/// read is left out of the dump. The 15 s limit bounds only the wait for the helper, not the start
/// of the helper before it. Logs one line before the helper starts and, in most cases, one with the
/// result. Returns false, with no result line, when those threads may still be suspended: the wait
/// timed out or failed, the helper's exit code cannot be read, or it is not one of the helper's
/// own codes (a crash). The caller must then end the process at once without logging or
/// allocating. Before InstallCrashHandler it only logs that the dump is skipped.
[[nodiscard]] bool WriteDumpNow(const char* reason);

/// Helper mode for WriteDumpNow. If the command line is
/// `<exe> --write-minidump <pid> <thread id> <file>`, writes a minidump of process <pid> without
/// thread <thread id> to <file> and returns the exit code (0 on success). Otherwise returns
/// nothing. The frontend calls it first in main, before the log, the config or Qt start.
std::optional<int> RunDumpHelper();

} // namespace Common::Windows
