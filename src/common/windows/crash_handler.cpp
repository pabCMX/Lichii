// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <optional>
#include <string>

#include <intrin.h>
#include <windows.h>

#include <dbghelp.h>
#include <shellapi.h>

#include "common/logging.h"
#include "common/windows/crash_handler.h"

#define LOG_CRASH(...) ::Common::Log::FmtCrashMessage(__FILE__, __LINE__, __func__, __VA_ARGS__)

namespace Common::Windows {
namespace {

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                          PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION,
                                          PMINIDUMP_CALLBACK_INFORMATION);
using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);

// Everything the handler needs is resolved at install: loading a DLL from a crashed process can
// deadlock on the loader lock.
MiniDumpWriteDumpFn write_dump{};
GetThreadDescriptionFn get_thread_description{};
LPTOP_LEVEL_EXCEPTION_FILTER previous_filter{};
wchar_t dump_path[MAX_PATH]{};
wchar_t dump_directory_path[MAX_PATH]{};
std::atomic<DWORD> reporting_thread{};

struct CodeAddress {
    char module[64];
    unsigned long long offset;
};

CodeAddress Locate(const void* address) {
    CodeAddress result{"?", reinterpret_cast<uintptr_t>(address)};
    HMODULE module{};
    char path[MAX_PATH];
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCSTR>(address), &module) ||
        GetModuleFileNameA(module, path, sizeof(path)) == 0) {
        return result;
    }
    const char* slash = std::strrchr(path, '\\');
    std::snprintf(result.module, sizeof(result.module), "%s", slash ? slash + 1 : path);
    result.offset -= reinterpret_cast<uintptr_t>(module);
    return result;
}

// `fail_fast` names a failure that ends in a fail-fast (abort() or a CRT invalid-parameter
// failure), and is null for an unhandled exception.
void Report(EXCEPTION_POINTERS* pointers, const char* fail_fast) {
    // Called on the crashed thread's stack, which the dispatcher's frames sit on top of: the
    // stack from the faulting frame on is the part after the exception address.
    void* frames[40];
    const USHORT count = RtlCaptureStackBackTrace(1, 40, frames, nullptr);
    const void* fault = pointers->ExceptionRecord->ExceptionAddress;
    const USHORT first =
        fail_fast ? 0 : static_cast<USHORT>(std::find(frames, frames + count, fault) - frames);
    char stack[1024] = "";
    std::size_t used = 0;
    for (USHORT i = first < count ? first : 0; i < count && frames[i]; ++i) {
        const CodeAddress frame = Locate(frames[i]);
        const int written = std::snprintf(stack + used, sizeof(stack) - used, " %s+%#llx",
                                          frame.module, frame.offset);
        if (written < 0 || used + static_cast<std::size_t>(written) >= sizeof(stack)) {
            break;
        }
        used += static_cast<std::size_t>(written);
    }

    char thread_name[64] = "";
    PWSTR description{};
    if (get_thread_description &&
        SUCCEEDED(get_thread_description(GetCurrentThread(), &description))) {
        WideCharToMultiByte(CP_UTF8, 0, description, -1, thread_name, sizeof(thread_name) - 1,
                            nullptr, nullptr);
        LocalFree(description);
    }

    if (!fail_fast) {
        const EXCEPTION_RECORD& record = *pointers->ExceptionRecord;
        const CodeAddress at = Locate(fault);
        LOG_CRASH("Host crash: unhandled exception {:#010x} at {}+{:#x} on thread {} \"{}\", "
                  "info {:#x} {:#x}; stack:{}",
                  record.ExceptionCode, at.module, at.offset, GetCurrentThreadId(), thread_name,
                  record.NumberParameters > 0 ? record.ExceptionInformation[0] : 0,
                  record.NumberParameters > 1 ? record.ExceptionInformation[1] : 0, stack);
    } else {
        LOG_CRASH("Host crash: {} on thread {} \"{}\"; stack:{}", fail_fast, GetCurrentThreadId(),
                  thread_name, stack);
    }

    if (!write_dump) {
        return;
    }
    const HANDLE file = CreateFileW(dump_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    MINIDUMP_EXCEPTION_INFORMATION information{GetCurrentThreadId(), pointers, FALSE};
    const BOOL written = write_dump(GetCurrentProcess(), GetCurrentProcessId(), file,
                                    MiniDumpWithThreadInfo, &information, nullptr, nullptr);
    const DWORD error = written ? 0 : GetLastError();
    CloseHandle(file);
    LOG_CRASH("Host crash: minidump of process {} {} ({:#x})", GetCurrentProcessId(),
              written ? "written next to the log as eden_crash.dmp" : "failed", error);
}

// Reports a fail-fast. The dump gets the context that RtlCaptureContext captures here, as a
// 0xC0000409 exception at `address`, the C runtime's call into the handler. A debugger walks from
// here up through the C runtime frames to the failing caller.
void ReportFailFast(const char* failure, void* address) {
    CONTEXT context{};
    RtlCaptureContext(&context);
    EXCEPTION_RECORD record{};
    record.ExceptionCode = 0xC0000409; // What the fail-fast reports next.
    record.ExceptionAddress = address;
    EXCEPTION_POINTERS pointers{&record, &context};
    Report(&pointers, failure);
}

// Exit codes of the dump helper. Any other code means that it crashed or was killed, maybe while
// threads of the target were still suspended.
constexpr DWORD helper_written = 0;
constexpr DWORD helper_bad_arguments = 101;
constexpr DWORD helper_no_dbghelp = 102;
constexpr DWORD helper_no_process = 103;
constexpr DWORD helper_no_file = 104;
constexpr DWORD helper_dump_failed = 105;

// Parses a decimal process or thread id: digits only, not 0, at most MAXDWORD.
std::optional<DWORD> ParseId(const wchar_t* text) {
    unsigned long long value = 0;
    for (const wchar_t* digit = text; *digit; ++digit) {
        if (*digit < L'0' || *digit > L'9') {
            return std::nullopt;
        }
        value = value * 10 + static_cast<unsigned>(*digit - L'0');
        if (value > MAXDWORD) {
            return std::nullopt;
        }
    }
    if (value == 0) {
        return std::nullopt;
    }
    return static_cast<DWORD>(value);
}

// The helper's MiniDumpWriteDump callback. It leaves out the thread whose id `param` points to, so
// that thread is not suspended. For the other callback types the dump stays as without a callback.
BOOL CALLBACK ExcludeThreadCallback(void* param, MINIDUMP_CALLBACK_INPUT* input,
                                    MINIDUMP_CALLBACK_OUTPUT* output) {
    switch (input->CallbackType) {
    case IncludeThreadCallback:
        return input->IncludeThread.ThreadId != *static_cast<const DWORD*>(param);
    case IncludeModuleCallback:
    case ModuleCallback:
    case ThreadCallback:
    case ThreadExCallback:
        // The write flags are set to their defaults on entry.
        return TRUE;
    case ReadMemoryFailureCallback:
        // Leave the block out instead of failing the whole dump.
        output->Status = S_OK;
        return TRUE;
    case CancelCallback:
        output->CheckCancel = FALSE;
        output->Cancel = FALSE;
        return TRUE;
    case SecondaryFlagsCallback:
        output->SecondaryFlags = 0;
        return TRUE;
    default:
        // FALSE adds or removes no memory region, keeps the default file I/O and starts no kernel
        // dump. The snapshot and virtual memory callbacks are not for a process target.
        return FALSE;
    }
}

// Only the first thread to crash reports. A second one waits a while for it to end the process,
// then falls through to the previous handling, so a report stuck on a lock the second thread
// holds cannot hang the process. A crash inside the report falls through at once.
bool BeginReport() {
    DWORD expected = 0;
    const DWORD self = GetCurrentThreadId();
    if (reporting_thread.compare_exchange_strong(expected, self)) {
        return true;
    }
    if (expected != self) {
        Sleep(10000);
    }
    return false;
}

LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* pointers) {
    if (BeginReport()) {
        Report(pointers, nullptr);
    }
    return previous_filter ? previous_filter(pointers) : EXCEPTION_CONTINUE_SEARCH;
}

// std::terminate (an uncaught C++ exception, also on a noexcept thread entry) and failed fatal
// asserts end in abort(), which the C runtime turns into a fail-fast that skips the filter above.
void OnAbort(int) {
    if (BeginReport()) {
        ReportFailFast("abort()", _ReturnAddress());
    }
}

// A CRT function given an invalid parameter (a bad file descriptor or a null format string, for
// example) also fail-fasts and skips the filter.
void OnInvalidParameter(const wchar_t* expression, const wchar_t* function, const wchar_t* file,
                        unsigned int line, std::uintptr_t reserved) {
    if (BeginReport()) {
        ReportFailFast("CRT invalid-parameter failure", _ReturnAddress());
    }
    // What the C runtime does without a handler.
    _invoke_watson(expression, function, file, line, reserved);
}

} // Anonymous namespace

void InstallCrashHandler(const std::filesystem::path& dump_directory) {
    const std::wstring path = (dump_directory / "eden_crash.dmp").wstring();
    if (path.size() < MAX_PATH) {
        std::wmemcpy(dump_path, path.c_str(), path.size() + 1);
        const std::wstring directory = dump_directory.wstring();
        std::wmemcpy(dump_directory_path, directory.c_str(), directory.size() + 1);
        if (const HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll")) {
            write_dump = reinterpret_cast<MiniDumpWriteDumpFn>(
                reinterpret_cast<void*>(GetProcAddress(dbghelp, "MiniDumpWriteDump")));
        }
    }
    get_thread_description = reinterpret_cast<GetThreadDescriptionFn>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"KernelBase.dll"), "GetThreadDescription")));
    previous_filter = SetUnhandledExceptionFilter(OnUnhandledException);
    std::signal(SIGABRT, OnAbort);
    _set_invalid_parameter_handler(OnInvalidParameter);
}

bool WriteDumpNow(const char* reason) {
    if (dump_directory_path[0] == L'\0') {
        LOG_ERROR(Common, "Minidump for {} skipped: the crash handler is not installed", reason);
        return true;
    }
    char name[64];
    std::snprintf(name, sizeof(name), "eden_%s.dmp", reason);
    // The helper is this large executable, which loads the Qt DLLs before main runs, and a dump of
    // this process takes a few seconds. 15 s covers that and still bounds the wait.
    constexpr DWORD timeout_ms = 15000;
    // The job ends the helper when this process ends (the exit watchdog, for one), also while the
    // helper is still suspended. Without it the helper could outlive this process, so no dump.
    const HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job ||
        !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        const DWORD error = GetLastError();
        if (job) {
            CloseHandle(job);
        }
        LOG_ERROR(Common, "Minidump for {} skipped: cannot set up a job for the helper ({:#x})",
                  reason, error);
        return true;
    }
    PROCESS_INFORMATION child{};
    {
        // The helper starts suspended and runs only after these strings are freed.
        wchar_t exe[MAX_PATH];
        const DWORD exe_size = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        if (exe_size == 0 || exe_size == MAX_PATH) {
            LOG_ERROR(Common, "Minidump for {} failed: no executable path ({:#x})", reason,
                      GetLastError());
            CloseHandle(job);
            return true;
        }
        const std::wstring path = (std::filesystem::path{dump_directory_path} / name).wstring();
        // Both paths end in a file name, so no backslash comes before a closing quote.
        std::wstring command = L"\"" + std::wstring{exe} + L"\" --write-minidump " +
                               std::to_wstring(GetCurrentProcessId()) + L" " +
                               std::to_wstring(GetCurrentThreadId()) + L" \"" + path + L"\"";
        LOG_ERROR(Common,
                  "Writing a minidump for {} as {} next to the log through a helper process. If "
                  "the helper crashes or does not end within {} ms, the process ends at once "
                  "with no more log lines",
                  reason, name, timeout_ms);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_FORCEOFFFEEDBACK;
        if (!CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup,
                            &child)) {
            LOG_ERROR(Common, "Minidump for {} failed: cannot start the helper ({:#x})", reason,
                      GetLastError());
            CloseHandle(job);
            return true;
        }
        if (!AssignProcessToJobObject(job, child.hProcess)) {
            // The helper never ran, so nothing is suspended.
            const DWORD error = GetLastError();
            TerminateProcess(child.hProcess, 1);
            CloseHandle(child.hThread);
            CloseHandle(child.hProcess);
            CloseHandle(job);
            LOG_ERROR(Common, "Minidump for {} failed: cannot put the helper in its job ({:#x})",
                      reason, error);
            return true;
        }
    }
    // From here on the helper can suspend the threads of this process, one of them maybe with the
    // heap lock held. The paths that return false do not allocate, free or log.
    if (ResumeThread(child.hThread) == static_cast<DWORD>(-1)) {
        // The helper never ran, so nothing is suspended.
        const DWORD error = GetLastError();
        TerminateProcess(child.hProcess, 1);
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
        CloseHandle(job);
        LOG_ERROR(Common, "Minidump for {} failed: cannot resume the helper ({:#x})", reason,
                  error);
        return true;
    }
    CloseHandle(child.hThread);
    const DWORD wait = WaitForSingleObject(child.hProcess, timeout_ms);
    if (wait != WAIT_OBJECT_0) {
        // Timed out or the wait failed: threads of this process can still be suspended.
        TerminateProcess(child.hProcess, 1);
        CloseHandle(child.hProcess);
        CloseHandle(job);
        return false;
    }
    DWORD exit_code{};
    const BOOL has_exit_code = GetExitCodeProcess(child.hProcess, &exit_code);
    CloseHandle(child.hProcess);
    CloseHandle(job);
    if (!has_exit_code) {
        // The helper ended, but without its exit code nothing tells that DbgHelp resumed the
        // threads.
        return false;
    }
    const char* failure = nullptr;
    switch (exit_code) {
    case helper_written:
        LOG_ERROR(Common, "Minidump of process {} for {} written next to the log as {}",
                  GetCurrentProcessId(), reason, name);
        return true;
    case helper_bad_arguments:
        failure = "bad helper arguments";
        break;
    case helper_no_dbghelp:
        failure = "cannot load dbghelp.dll";
        break;
    case helper_no_process:
        failure = "cannot open this process";
        break;
    case helper_no_file:
        failure = "cannot create the file";
        break;
    case helper_dump_failed:
        failure = "MiniDumpWriteDump failed";
        break;
    default:
        // The helper crashed or was killed, maybe while threads of this process were suspended.
        return false;
    }
    LOG_ERROR(Common, "Minidump for {} failed: {}", reason, failure);
    return true;
}

std::optional<int> RunDumpHelper() {
    int argc = 0;
    LPWSTR* const argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        // The mode is unknown. A command line with the switch text ends here, so a helper request
        // never starts the frontend. Any other command line starts the frontend as usual.
        if (std::wcsstr(GetCommandLineW(), L"--write-minidump")) {
            return static_cast<int>(helper_bad_arguments);
        }
        return std::nullopt;
    }
    if (argc < 2 || std::wcscmp(argv[1], L"--write-minidump") != 0) {
        LocalFree(argv);
        return std::nullopt;
    }
    const auto write = [&]() -> DWORD {
        if (argc != 5) {
            return helper_bad_arguments;
        }
        const std::optional<DWORD> pid = ParseId(argv[2]);
        const std::optional<DWORD> thread = ParseId(argv[3]);
        if (!pid || !thread) {
            return helper_bad_arguments;
        }
        DWORD excluded_thread = *thread;
        const HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
        const auto write_dump_fn =
            dbghelp ? reinterpret_cast<MiniDumpWriteDumpFn>(
                          reinterpret_cast<void*>(GetProcAddress(dbghelp, "MiniDumpWriteDump")))
                    : nullptr;
        if (!write_dump_fn) {
            return helper_no_dbghelp;
        }
        const HANDLE target = OpenProcess(
            PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE, FALSE, *pid);
        if (!target) {
            return helper_no_process;
        }
        const HANDLE file = CreateFileW(argv[4], GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            CloseHandle(target);
            return helper_no_file;
        }
        // Every thread except the caller's, which waits for this helper. MiniDumpWithThreadInfo,
        // as for the crash dump: larger types failed on the emulator's address space.
        MINIDUMP_CALLBACK_INFORMATION callback{ExcludeThreadCallback, &excluded_thread};
        const BOOL written =
            write_dump_fn(target, *pid, file, MiniDumpWithThreadInfo, nullptr, nullptr, &callback);
        CloseHandle(file);
        CloseHandle(target);
        return written ? helper_written : helper_dump_failed;
    };
    const DWORD code = write();
    LocalFree(argv);
    return static_cast<int>(code);
}

} // namespace Common::Windows
