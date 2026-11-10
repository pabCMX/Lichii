// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "common/common_types.h"
#include "hid_core/frontend/emulated_controller.h"

namespace Core {
class System;
}

namespace Core::Memory {
class Memory;
}

namespace Core::Nx2Debug {

class Server;
struct Endpoint;
struct ToolState;

/// The debug server of one boot (docs/nx2/debug-mcp.md): started when a game has loaded, stopped
/// first when it shuts down, so the tools never see a half-built or half-destroyed system.
class Service {
public:
    explicit Service(System& system, std::string title, u64 program_id, const Endpoint& endpoint,
                     std::string dump_directory);
    ~Service();

    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

private:
    std::shared_ptr<ToolState> state;
    std::unique_ptr<Server> server;
};

/// A reject_shaders hash: exactly 16 hex digits, optionally after 0x. Throws std::runtime_error.
[[nodiscard]] u64 ParseShaderHash(std::string_view text);

/// search_memory's pattern: hex digit pairs, optionally after 0x, spaces allowed. Throws
/// std::runtime_error.
[[nodiscard]] std::vector<u8> ParseHexBytes(std::string_view text);

/// read_memory's formats: {"hex": "..."} or {"u32"|"u64"|"f32": [...]}; u64 values are hex strings
/// and non-finite floats are "nan", "inf" or "-inf". Throws std::runtime_error when the format is
/// unknown or the size is not a multiple of its element.
[[nodiscard]] nlohmann::json FormatMemory(std::span<const u8> bytes, std::string_view as);

/// Copies guest memory from a thread that is not a guest CPU core (docs/nx2/debug-mcp.md, "Guest
/// and GPU memory"). Returns the first address that is not mapped; out is then partly written.
[[nodiscard]] std::optional<u64> ReadGuestMemory(Memory::Memory& memory, u64 address,
                                                 std::span<u8> out);

struct GuestSearch {
    std::vector<u64> hits;
    /// Where to resume: the first address no match was looked for at; end when complete.
    u64 next;
    bool complete;
    /// Mapped bytes read.
    u64 scanned;
};

/// Finds the pattern in the mapped pages of [start, end), as ReadGuestMemory reads them. Stops at
/// max_hits or the deadline. Matches may span pages, but not unmapped gaps.
[[nodiscard]] GuestSearch SearchGuestMemory(Memory::Memory& memory, std::span<const u8> pattern,
                                            u64 start, u64 end, size_t max_hits,
                                            std::chrono::steady_clock::time_point deadline);

/// One step of input or input_sequence: what to hold, for how many presented frames, or for how
/// many milliseconds of host time (ms != 0, frames == 0).
struct InputStep {
    HID::InjectedInput input;
    u64 frames;
    u64 ms{};
};

/// {buttons?, left_stick?, right_stick?, frames | ms}. Sticks are [x, y] in -1..1. Throws
/// std::runtime_error on unknown keys, buttons or out-of-range values.
[[nodiscard]] InputStep ParseInputStep(const nlohmann::json& step);

/// A frame-counted step as JSON that ParseInputStep reads back to the same input: buttons when
/// any are held, and both sticks when the step sets them.
[[nodiscard]] nlohmann::json InputStepJson(const InputStep& step);

/// An input_record recording: one input per present from start_present on, run-length encoded
/// into frame-counted steps. It keeps at most max_presents presents and max_steps steps; past
/// either it is truncated and keeps what it has.
class InputRecording {
public:
    InputRecording(u64 start_present, u64 max_presents, size_t max_steps);

    /// Records the input the game is given at present `present`. A present before the next
    /// expected one is ignored; skipped presents get this input too. False once truncated.
    bool Add(u64 present, const HID::InjectedInput& input);

    [[nodiscard]] const std::vector<InputStep>& Steps() const {
        return steps;
    }
    [[nodiscard]] u64 StartPresent() const {
        return start_present;
    }
    /// Presents recorded: end_present = StartPresent() + Presents() - 1.
    [[nodiscard]] u64 Presents() const {
        return next_present - start_present;
    }
    [[nodiscard]] bool Truncated() const {
        return truncated;
    }

private:
    std::vector<InputStep> steps;
    u64 start_present;
    u64 next_present;
    u64 max_presents;
    size_t max_steps;
    bool truncated{};
};

/// The input of the timed step active elapsed_us after the start; nothing from the end on.
[[nodiscard]] std::optional<HID::InjectedInput> InputAtTime(std::span<const InputStep> steps,
                                                           u64 elapsed_us);

/// The input of the step active at present count now, for steps starting at present count start;
/// nothing before start and from the end of the last step on.
[[nodiscard]] std::optional<HID::InjectedInput> InputAt(std::span<const InputStep> steps, u64 start,
                                                       u64 now);
/// Guest-present intervals within [begin_ns, end_ns], using only retained ring timestamps;
/// intervals_ms lists the last recent_limit of them.
[[nodiscard]] nlohmann::json ReadFrameTimes(u64 before, u64 after, u64 begin_ns, u64 end_ns,
                                          double refresh_hz, size_t recent_limit = 120);

/// A PNG and its size.
struct Png {
    std::vector<u8> bytes;
    u32 width;
    u32 height;
    /// The guest present the frame shows (RendererBase::ScreenshotPresent).
    u64 present{};
};

/// The next presented frame as a PNG, height pixels high at 16:9 (the debug server's screenshot).
/// Throws std::runtime_error when no frame is presented within timeout (paused or loading), when
/// stop is set first, or when another screenshot request is pending.
[[nodiscard]] Png CaptureScreenshot(System& system, u32 height, std::chrono::seconds timeout,
                                    const std::atomic<bool>& stop);

/// Full precision intervals between held presents only; incomplete snapshots fail closed.
[[nodiscard]] nlohmann::json ReadInputFrameTimes(u64 first, u64 total, u64 begin_ns, u64 end_ns);

/// Runs one debug-server method on system as the service would, with a client that never leaves.
/// For tests: the service itself dispatches through its own handlers.
[[nodiscard]] nlohmann::json CallTool(System& system, std::string_view method,
                                      const nlohmann::json& params);

/// The frontend's input recording hotkey: runs input_record with action "start", "status" or
/// "stop" and returns its reply. Throws std::runtime_error as the method does.
[[nodiscard]] nlohmann::json RecordInput(System& system, std::string_view action);

/// Whether a debug server runs now. The frontend's input recording hotkey is off otherwise.
[[nodiscard]] bool IsServiceRunning();

/// The service when EDEN_NX2_DEBUG_SERVER is set and it can listen, otherwise null.
[[nodiscard]] std::unique_ptr<Service> MaybeStartService(System& system, std::string title,
                                                         u64 program_id);

} // namespace Core::Nx2Debug
