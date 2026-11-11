// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <mutex>
#include <numeric>
#include <thread>
#include <tuple>
#include <unordered_map>

#include "common/fs/path_util.h"
#include "common/scope_exit.h"
#include "common/thread.h"
#include "common/logging.h"
#include "common/nx2_kit_stall.h"
#include "common/nx2_sampler.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "common/stb.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/debugger/nx2_bug_bundle.h"
#include "core/debugger/nx2_debug_server.h"
#include "core/debugger/nx2_debug_tools.h"
#include "core/debugger/nx2_trainer_tools.h"
#include "core/file_sys/oot_time_control.h"
#include "core/frontend/framebuffer_layout.h"
#include "core/hle/kernel/k_process.h"
#include "core/memory.h"
#include "hid_core/hid_core.h"
#include "hid_core/resource_manager.h"
#include "video_core/gpu.h"
#include "video_core/gpu_trace/recorder.h"
#include "video_core/nx2_debug_control.h"
#include "video_core/renderer_base.h"

namespace Core::Nx2Debug {

using nlohmann::json;
using namespace std::chrono_literals;

/// Composites one luma_watch arms at most (60 s at 60 presents/s).
constexpr u32 MAX_LUMA_WATCH = 3600;
/// Lines the log ring keeps for log_tail.
constexpr size_t RING_LINES = 20000;
/// Bytes one read_memory or read_gpu_memory call returns at most.
constexpr u64 MAX_READ = 64 * 1024;
/// Presented frames one input or input_sequence call holds at most (10 minutes at 60 fps).
constexpr u64 MAX_INPUT_FRAMES = 36000;
constexpr u64 MAX_INPUT_MS = 600'000;
/// Steps per input_sequence call and per input_record recording. A recorded step with both
/// sticks and two buttons is about 110 bytes, so 6000 steps fit the 1 MB request limit
/// (nx2_debug_server.cpp): 100 s of a stick that moves at every present at 60 presents/s.
constexpr size_t MAX_INPUT_STEPS = 6000;
/// Presents one measured input holds at most: the present-time ring, less 256 presents of margin
/// for presents that land while the measurement is read.
constexpr u64 MAX_MEASURED_PRESENTS = std::tuple_size_v<decltype(g_nx2_present_times_ns)> - 256;
/// The pre-roll ring of record_anchor: 5 minutes at 60 presents/s, and half the steps of a call,
/// so a pre-roll and its clip can still fit one input_sequence call.
constexpr u64 MAX_PRE_ROLL_PRESENTS = 18000;
constexpr size_t MAX_PRE_ROLL_STEPS = 3000;
/// Screenshots one input_sequence capture_frames call saves at most (about 1.5 MB each).
constexpr size_t MAX_CAPTURE_FRAMES = 32;
/// scan_memory's kept hits: 16 bytes each, so 32 MB at most.
constexpr size_t MAX_SCAN_HITS = 2'000'000;
/// Hits one scan_memory list call returns at most.
constexpr size_t MAX_SCAN_LIST = 1000;
/// Addresses one input_sequence watch reads at each present.
constexpr size_t MAX_WATCH = 8;

constexpr std::array<std::pair<std::string_view, HID::NpadButton>, 16> INPUT_BUTTONS{{
    {"A", HID::NpadButton::A},         {"B", HID::NpadButton::B},
    {"X", HID::NpadButton::X},         {"Y", HID::NpadButton::Y},
    {"L", HID::NpadButton::L},         {"R", HID::NpadButton::R},
    {"ZL", HID::NpadButton::ZL},       {"ZR", HID::NpadButton::ZR},
    {"Plus", HID::NpadButton::Plus},   {"Minus", HID::NpadButton::Minus},
    {"Up", HID::NpadButton::Up},       {"Down", HID::NpadButton::Down},
    {"Left", HID::NpadButton::Left},   {"Right", HID::NpadButton::Right},
    {"StickL", HID::NpadButton::StickL}, {"StickR", HID::NpadButton::StickR},
}};

struct ToolState {
    explicit ToolState(System& system_, std::string title_, u64 program_id_,
                       std::filesystem::path dump_directory_)
        : system{system_}, title{std::move(title_)}, program_id{program_id_},
          dump_directory{std::move(dump_directory_)} {}

    System& system;
    std::string title;
    u64 program_id;
    std::filesystem::path dump_directory;
    std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};
    /// Names this session's captures apart from earlier ones in the same dump directory.
    s64 session{std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count()};
    std::string log_filter{Settings::values.log_filter.GetValue()};
    u64 captures{};
    u64 screenshots{};
    u64 clips{};
    u64 samples{};
    u64 traces{};
    u64 gpu_reads{};
    std::atomic<bool> stopping{};
};

namespace {

std::string Hex(u64 value) {
    return fmt::format("{:016x}", value);
}

json HexList(const std::vector<u64>& values) {
    json list = json::array();
    for (const u64 value : values) {
        list.push_back(Hex(value));
    }
    return list;
}

template <typename T>
T Param(const json& params, const char* name, T fallback, T low, T high) {
    const T value = params.value(name, fallback);
    return (std::clamp)(value, low, high);
}

std::string HexAddress(u64 value) {
    return fmt::format("{:#x}", value);
}

/// An address as a JSON number or a hex string (0x optional).
u64 AddressParam(const json& params, const char* name) {
    const json& value = params.at(name);
    if (value.is_number_unsigned()) {
        return value.get<u64>();
    }
    if (!value.is_string()) {
        throw std::runtime_error{fmt::format("{} must be a hex string or a non-negative number", name)};
    }
    const std::string text = value.get<std::string>();
    const std::string_view view{text};
    const std::string_view digits =
        view.starts_with("0x") || view.starts_with("0X") ? view.substr(2) : view;
    u64 address = 0;
    const auto [end, error] =
        std::from_chars(digits.data(), digits.data() + digits.size(), address, 16);
    if (digits.empty() || error != std::errc{} || end != digits.data() + digits.size()) {
        throw std::runtime_error{fmt::format("bad {} \"{}\": want hex, e.g. 0x8012345000", name, text)};
    }
    return address;
}

/// The size of a read: 1 to MAX_READ bytes, not wrapping past the end of the address space.
u64 ReadSize(const json& params, u64 address) {
    const json& value = params.at("size");
    if (!value.is_number_integer() || value.get<s64>() < 1 || value.get<u64>() > MAX_READ) {
        throw std::runtime_error{fmt::format("size must be 1 to {} bytes", MAX_READ)};
    }
    const u64 size = value.get<u64>();
    if (address + size < address) {
        throw std::runtime_error{"address + size wraps around"};
    }
    return size;
}

Kernel::KProcess& Application(ToolState& state) {
    Kernel::KProcess* const process = state.system.ApplicationProcess();
    if (process == nullptr) {
        throw std::runtime_error{"no application process is running"};
    }
    return *process;
}

/// A host pointer to the byte at address, or null when its page is not mapped. GetPointerSilent
/// reads the page entry as guest cores do, with no commit, log or GPU download; it masks the top
/// 16 bits, so addresses there would alias and are unmapped instead.
const u8* GuestByte(Memory::Memory& memory, u64 address) {
    return (address >> 48) == 0 ? memory.GetPointerSilent(address) : nullptr;
}

bool IsVulkan() {
    return Settings::values.renderer_backend.GetValue() == Settings::RendererBackend::Vulkan;
}

/// Waits for a future in short slices so a stopping server does not wait out the timeout.
template <typename T>
bool WaitReady(const std::atomic<bool>& stop, std::future<T>& future,
               std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (future.wait_for(50ms) != std::future_status::ready) {
        if (stop.load(std::memory_order_relaxed) || std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
    }
    return true;
}

void AppendPng(void* context, void* data, int size) {
    auto& png = *static_cast<std::vector<u8>*>(context);
    const u8* const bytes = static_cast<const u8*>(data);
    png.insert(png.end(), bytes, bytes + size);
}

/// A renderer screenshot (BGRA, rows bottom up when invert_y) as a PNG. Throws std::runtime_error.
std::vector<u8> ScreenshotPng(const std::vector<u8>& pixels, const Layout::FramebufferLayout& layout,
                              bool invert_y) {
    std::vector<u8> rgb(size_t{layout.width} * layout.height * 3);
    for (u32 y = 0; y < layout.height; ++y) {
        const u8* source = pixels.data() + size_t{invert_y ? layout.height - 1 - y : y} * layout.width * 4;
        u8* target = rgb.data() + size_t{y} * layout.width * 3;
        for (u32 x = 0; x < layout.width; ++x, source += 4, target += 3) {
            target[0] = source[2];
            target[1] = source[1];
            target[2] = source[0];
        }
    }
    std::vector<u8> png;
    if (!stbi_write_png_to_func(AppendPng, &png, static_cast<int>(layout.width),
                                static_cast<int>(layout.height), 3, rgb.data(),
                                static_cast<int>(layout.width * 3))) {
        throw std::runtime_error{"PNG encoding failed"};
    }
    return png;
}

json Status(ToolState& state, const json& params) {
    const auto window = std::chrono::milliseconds{Param<int>(params, "fps_window_ms", 500, 0, 5000)};
    const u64 before = g_nx2_present_count.load(std::memory_order_relaxed);
    const auto begin = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - begin < window &&
           !state.stopping.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(20ms);
    }
    const u64 after = g_nx2_present_count.load(std::memory_order_relaxed);
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    return {
        {"title", state.title},
        {"program_id", fmt::format("{:016X}", state.program_id)},
        {"build", std::string{Common::g_build_fullname}},
        {"scm_rev", std::string{Common::g_scm_rev}},
        {"switch2", Settings::CurrentProgramIsSwitch2()},
        {"renderer", IsVulkan() ? "vulkan" : "other"},
        {"powered_on", state.system.IsPoweredOn()},
        {"paused", state.system.IsPaused()},
        {"presents", after},
        {"fps", seconds > 0.0 ? static_cast<double>(after - before) / seconds : 0.0},
        {"uptime_s", std::chrono::duration<double>(std::chrono::steady_clock::now() - state.start)
                         .count()},
        {"dump_dir", state.dump_directory.generic_string()},
        {"log_filter", state.log_filter},
        {"rejected", HexList(VideoCore::Nx2Debug::RejectedHashes())},
    };
}

json FrameTimes(ToolState& state, const json& params) {
    const double seconds = params.value("seconds", 5.0);
    const double refresh_hz = params.value("refresh_hz", 60.0);
    if (!std::isfinite(seconds) || seconds <= 0.0 || seconds > 60.0 ||
        !std::isfinite(refresh_hz) || refresh_hz <= 0.0) {
        throw std::runtime_error{"seconds must be > 0 and <= 60; refresh_hz must be > 0"};
    }
    const u64 before = g_nx2_present_count.load(std::memory_order_relaxed);
    const auto begin = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count() <
               seconds &&
           !state.stopping.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(20ms);
    }
    const auto end = std::chrono::steady_clock::now();
    const u64 after = g_nx2_present_count.load(std::memory_order_relaxed);
    const u64 begin_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(begin.time_since_epoch()).count();
    const u64 end_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(end.time_since_epoch()).count();
    return ReadFrameTimes(before, after, begin_ns, end_ns, refresh_hz);
}

} // Anonymous namespace

static json IntervalStats(std::vector<double> intervals, double refresh_hz, bool truncated,
                          bool full_precision, size_t recent_limit = 120) {
    json recent = json::array();
    for (size_t i = !full_precision && intervals.size() > recent_limit ? intervals.size() - recent_limit : 0;
         i < intervals.size(); ++i) {
        recent.push_back(full_precision ? intervals[i] : std::round(intervals[i] * 10.0) / 10.0);
    }
    std::ranges::sort(intervals);
    const size_t frames = intervals.size();
    const auto percentile = [&](double fraction) {
        if (frames == 0) {
            return 0.0;
        }
        const double index = fraction * static_cast<double>(frames - 1);
        const size_t low = static_cast<size_t>(index);
        const size_t high = (std::min)(low + 1, frames - 1);
        return intervals[low] +
               (intervals[high] - intervals[low]) * (index - static_cast<double>(low));
    };
    const double mean = frames == 0 ? 0.0 : std::accumulate(intervals.begin(), intervals.end(), 0.0) /
                                              static_cast<double>(frames);
    const double median = percentile(0.5);
    std::array<size_t, 4> buckets{};
    size_t stutters = 0;
    for (const double interval : intervals) {
        const double periods = interval * refresh_hz / 1000.0;
        ++buckets[periods < 1.5 ? 0 : periods < 2.5 ? 1 : periods < 3.5 ? 2 : 3];
        stutters += interval > 2.0 * median;
    }
    const auto share = [&](size_t bucket) {
        return frames == 0 ? 0.0 : static_cast<double>(buckets[bucket]) / static_cast<double>(frames);
    };
    return {{"frames", frames},
            {"mean_ms", mean},
            {"fps", mean > 0.0 ? 1000.0 / mean : 0.0},
            {"p50_ms", median},
            {"p90_ms", percentile(0.9)},
            {"p99_ms", percentile(0.99)},
            {"max_ms", frames == 0 ? 0.0 : intervals.back()},
            {"min_ms", frames == 0 ? 0.0 : intervals.front()},
            {"vsync_buckets", {{"1", share(0)}, {"2", share(1)}, {"3", share(2)}, {"4+", share(3)}}},
            {"stutters", stutters},
            {"longest_ms", json(std::vector<double>(intervals.rbegin(),
                                                    intervals.rbegin() +
                                                        static_cast<std::ptrdiff_t>(
                                                            (std::min<size_t>)(frames, 10))))},
            {"intervals_ms", std::move(recent)},
            {"truncated", truncated}};
}

json ReadFrameTimes(u64 before, u64 after, u64 begin_ns, u64 end_ns, double refresh_hz,
                    size_t recent_limit) {
    const u64 capacity = g_nx2_present_times_ns.size();
    bool truncated = after < before || after - before > capacity;
    const u64 first = (std::max)(before + 1, after > capacity ? after - capacity + 1 : u64{1});
    std::vector<double> intervals;
    u64 previous = 0;
    for (u64 frame = after; frame >= first; --frame) {
        const u64 timestamp =
            g_nx2_present_times_ns[frame % capacity].load(std::memory_order_relaxed);
        // The counter advances before its store; the slot can also be overwritten during a read.
        if (timestamp < begin_ns || timestamp > end_ns) {
            truncated |= timestamp > end_ns;
            previous = 0;
            continue;
        }
        if (previous != 0 && timestamp > previous) {
            intervals.clear();
        } else if (previous != 0) {
            intervals.push_back(static_cast<double>(previous - timestamp) / 1'000'000.0);
        }
        previous = timestamp;
    }

    std::ranges::reverse(intervals);
    return IntervalStats(std::move(intervals), refresh_hz, truncated, false, recent_limit);
}

json ReadInputFrameTimes(u64 first, u64 total, u64 begin_ns, u64 end_ns) {
    if (first == 0 || total == 0 || total > MAX_MEASURED_PRESENTS || first + total < first) {
        throw std::runtime_error{"invalid measured input range"};
    }
    const u64 last = first + total - 1;
    const u64 capacity = g_nx2_present_times_ns.size();
    const u64 before = g_nx2_present_count.load(std::memory_order_seq_cst);
    std::vector<double> intervals;
    u64 previous = 0;
    bool complete = before > last && before - first < capacity;
    for (u64 frame = first; complete && frame <= last; ++frame) {
        const u64 timestamp = g_nx2_present_times_ns[frame % capacity].load(std::memory_order_seq_cst);
        // A pending store retains zero or a timestamp from before input; neither is accepted.
        if (timestamp == 0 || timestamp < begin_ns || timestamp > end_ns ||
            (previous && timestamp <= previous)) {
            complete = false;
            break;
        }
        if (previous) {
            intervals.push_back(static_cast<double>(timestamp - previous) / 1'000'000.0);
        }
        previous = timestamp;
    }
    const u64 after = g_nx2_present_count.load(std::memory_order_seq_cst);
    complete &= after >= before && after - first < capacity;
    if (!complete) {
        intervals.clear();
    }
    auto result = IntervalStats(std::move(intervals), 60.0, !complete, true);
    result.update({{"supported", "held-input-v1"}, {"first_present", first},
                   {"last_present", last}, {"expected_intervals", total - 1},
                   {"complete", complete}, {"covered_presents", complete ? total : 0},
                   {"snapshot_before", before}, {"snapshot_after", after}});
    return result;
}


Png CaptureScreenshot(System& system, u32 height, std::chrono::seconds timeout,
                      const std::atomic<bool>& stop) {
    auto& renderer = system.GPU().Renderer();
    if (renderer.IsScreenshotPending()) {
        throw std::runtime_error{"a screenshot is already pending: no frame was presented since"};
    }
    const auto layout = Layout::DefaultFrameLayout(height * 16 / 9, height);
    const auto pixels = std::make_shared<std::vector<u8>>(size_t{layout.width} * layout.height * 4);
    const auto done = std::make_shared<std::promise<bool>>();
    auto future = done->get_future();
    renderer.RequestScreenshot(
        pixels->data(), [pixels, done](bool invert_y) { done->set_value(invert_y); }, layout);
    if (!WaitReady(stop, future, timeout)) {
        throw std::runtime_error{fmt::format(
            "no frame was presented within {} s (paused or loading?)", timeout.count())};
    }
    const bool invert_y = future.get();
    // Stored on the GPU thread before the callback was started. The debug server is serial, and
    // a bug bundle's screenshot is refused while this one is pending, so read it now.
    const u64 present = renderer.ScreenshotPresent();
    return {.bytes = ScreenshotPng(*pixels, layout, invert_y),
            .width = layout.width,
            .height = layout.height,
            .present = present};
}

namespace {

json Screenshot(ToolState& state, const json& params) {
    if (state.system.IsPaused()) {
        // A request now would stay pending until the next present and block later screenshots.
        throw std::runtime_error{"paused: a screenshot needs a present; use step_frames or resume"};
    }
    const Png shot = CaptureScreenshot(
        state.system, Param<u32>(params, "height", 720, 144, 2160),
        std::chrono::seconds{Param<int>(params, "timeout_s", 5, 1, 30)}, state.stopping);
    const std::vector<u8>& png = shot.bytes;
    json result = {{"width", shot.width}, {"height", shot.height}, {"present", shot.present}};
    if (params.value("save", false)) {
        std::filesystem::create_directories(state.dump_directory);
        const auto path =
            state.dump_directory / fmt::format("screenshot-{}-{:04}.png", state.session, ++state.screenshots);
        std::ofstream file{path, std::ios::binary};
        file.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
        if (!file) {
            throw std::runtime_error{fmt::format("cannot write {}", path.generic_string())};
        }
        result["path"] = path.generic_string();
    }
    if (params.value("inline", true)) {
        result["png_base64"] = Base64(png);
    }
    return result;
}

json LogTail(ToolState&, const json& params) {
    const u64 after = params.value("after", u64{0});
    const size_t limit = Param<size_t>(params, "limit", 200, 1, 5000);
    const std::string contains = params.value("contains", std::string{});
    const std::vector<Common::Log::RingLine> lines = Common::Log::ReadRing(after);
    std::vector<const Common::Log::RingLine*> matched;
    for (const auto& line : lines) {
        if (contains.empty() || line.text.find(contains) != std::string::npos) {
            matched.push_back(&line);
        }
    }
    // From the start the newest lines; after a sequence number the oldest ones, to page forward.
    const bool more = matched.size() > limit;
    if (more) {
        if (after == 0) {
            matched.erase(matched.begin(), matched.end() - static_cast<std::ptrdiff_t>(limit));
        } else {
            matched.resize(limit);
        }
    }
    json text = json::array();
    for (const auto* line : matched) {
        text.push_back(line->text);
    }
    const u64 next = more && after != 0 ? matched.back()->sequence
                     : lines.empty()    ? after
                                        : lines.back().sequence;
    return {{"lines", std::move(text)}, {"next", next}, {"more", more}};
}

json LogFilter(ToolState& state, const json& params) {
    const std::string filter = params.at("filter").get<std::string>();
    Common::Log::Filter parsed;
    parsed.ParseFilterString(filter);
    Common::Log::SetGlobalFilter(parsed);
    json result = {{"previous", state.log_filter}, {"filter", filter}};
    state.log_filter = filter;
    return result;
}

json FinishCapture(u64 id, const std::filesystem::path& directory, bool sampled, bool buffers, int timeout,
                   size_t keep);

/// Requests a capture of the next frame the GPU finishes and returns the wait for it, which
/// answers as capture_frame does. step_frames {capture} requests it while the game is paused.
std::function<json(bool cancel)> StartCapture(ToolState& state, const json& params) {
    if (!IsVulkan()) {
        throw std::runtime_error{"frame capture needs the Vulkan renderer"};
    }
    const u64 id = ++state.captures;
    const auto directory = state.dump_directory / fmt::format("capture-{}-{:03}", state.session, id);
    std::filesystem::create_directories(directory);
    const bool sampled = params.value("sampled", false);
    const bool buffers = params.value("buffers", false);
    const int timeout = Param<int>(params, "timeout_s", 120, 1, 600);
    const size_t keep = Param<size_t>(params, "manifest_lines", 200, 0, 5000);
    VideoCore::Nx2Debug::RequestCapture(
        {.id = id, .directory = directory, .sampled = sampled, .buffers = buffers});
    return [id, directory, sampled, buffers, timeout, keep](bool cancel) -> json {
        if (cancel) {
            VideoCore::Nx2Debug::CancelCapture(id);
            return {};
        }
        return FinishCapture(id, directory, sampled, buffers, timeout, keep);
    };
}

json CaptureFrame(ToolState& state, const json& params) {
    if (state.system.IsPaused()) {
        throw std::runtime_error{"paused: a capture needs a present; use step_frames {frames: 1, "
                                 "capture: true} for the next frame, or resume"};
    }
    return StartCapture(state, params)(false);
}

json FinishCapture(u64 id, const std::filesystem::path& directory, bool sampled, bool buffers, int timeout,
                   size_t keep) {
    const auto finished = VideoCore::Nx2Debug::WaitCapture(id, std::chrono::seconds{timeout});
    if (!finished) {
        VideoCore::Nx2Debug::CancelCapture(id);
        throw std::runtime_error{fmt::format(
            "the capture did not finish within {} s (paused or loading?); if it started, its "
            "files are still being written to {}",
            timeout, directory.generic_string())};
    }
    if (!finished->error.empty()) {
        throw std::runtime_error{finished->error};
    }
    json entries = json::array();
    json kinds = json::object();
    json frame;
    json summary;
    size_t total = 0;
    std::ifstream manifest{directory / "manifest.jsonl"};
    for (std::string line; std::getline(manifest, line);) {
        json entry = json::parse(line, nullptr, false);
        if (entry.is_discarded()) {
            continue;
        }
        const std::string kind = entry.value("kind", std::string{});
        if (kind == "frame") {
            frame = std::move(entry);
        } else if (kind == "summary") {
            summary = std::move(entry);
        } else {
            kinds[kind] = kinds.value(kind, 0) + 1;
            if (total++ < keep) {
                entries.push_back(std::move(entry));
            }
        }
    }
    return {
        {"directory", directory.generic_string()},
        {"sampled", sampled},
        {"buffers", buffers},
        {"frame", std::move(frame)},
        {"summary", std::move(summary)},
        {"kinds", std::move(kinds)},
        {"entries_total", total},
        {"entries", std::move(entries)},
        {"index_command",
         fmt::format("python3 -I tools/nx2/frame_dump_index.py \"{}\"", directory.generic_string())},
    };
}

json CaptureBundle(ToolState& state, const json& params) {
    BugBundle bundle{
        state.system,
        {.trigger = "debug_server",
         .directory = state.dump_directory / "bug-reports",
         .size_limit = Param<u64>(params, "size_limit_mib", BugBundle::DEFAULT_SIZE_LIMIT >> 20, 1,
                                  100)
                       << 20,
         .capture_timeout = std::chrono::seconds{Param<int>(params, "timeout_s", 90, 5, 600)}}};
    bundle.Capture(state.stopping);
    const BugBundle::Result result = bundle.Write(params.value("description", std::string{}));
    return {
        {"path", result.path.generic_string()},
        {"bytes", result.bytes},
        {"dropped", result.dropped},
        {"frame_error", result.frame_error},
        {"manifest", result.manifest},
        {"index_command", fmt::format("python3 -I tools/nx2/bundle_index.py \"{}\"",
                                      result.path.generic_string())},
    };
}

json RejectShaders(ToolState&, const json& params) {
    if (!IsVulkan()) {
        throw std::runtime_error{"runtime rejection needs the Vulkan renderer"};
    }
    std::vector<u64> hashes;
    for (const json& hash : params.value("hashes", json::array())) {
        hashes.push_back(ParseShaderHash(hash.get<std::string>()));
    }
    const bool reject = params.value("reject", true);
    const u64 generation = VideoCore::Nx2Debug::SetRejected(hashes, reject);
    const int timeout = Param<int>(params, "timeout_s", 5, 0, 60);
    const bool applied = hashes.empty() ||
                         VideoCore::Nx2Debug::WaitRejectApplied(generation, std::chrono::seconds{timeout});
    json result = {{"rejected", HexList(VideoCore::Nx2Debug::RejectedHashes())}, {"applied", applied}};
    if (!applied) {
        result["note"] = "no frame was presented yet; the change applies at the next frame end";
    }
    return result;
}

json TraceStatus(ToolState&, const json&) {
    const auto status = VideoCommon::GpuTrace::GetRecorderStatus();
    return {{"requested", status.requested},       {"recording", status.recording},
            {"presents", status.presents},         {"presents_wanted", status.presents_wanted},
            {"bytes_uncompressed", status.bytes},  {"path", status.path},
            {"last_trace", status.last_trace},     {"last_error", status.last_error}};
}

json TraceRecord(ToolState& state, const json& params) {
    VideoCommon::GpuTrace::RecordOptions options;
    const double seconds = Param<double>(params, "seconds", 3.0, 0.1, 60.0);
    options.presents = Param<u32>(params, "presents", static_cast<u32>(seconds * 60.0), 1, 3600);
    if (params.contains("path")) {
        options.path = params["path"].get<std::string>();
    } else {
        std::filesystem::create_directories(state.dump_directory);
        options.path = state.dump_directory /
                       fmt::format("trace-{}-{:03}.lxtrace", state.session, ++state.traces);
    }
    std::string error;
    if (!VideoCommon::GpuTrace::RequestRecording(options, error)) {
        throw std::runtime_error{error};
    }
    return TraceStatus(state, params);
}

json TraceStop(ToolState& state, const json& params) {
    VideoCommon::GpuTrace::StopRecording();
    return TraceStatus(state, params);
}

json SampleThreads(ToolState& state, const json& params) {
    const std::string threads = params.value("threads", std::string{"GPU,GPUBack"});
    const double seconds = Param<double>(params, "seconds", 10.0, 1.0, 60.0);
    std::filesystem::create_directories(state.dump_directory);
    const auto path =
        state.dump_directory / fmt::format("samples-{}-{:03}.txt", state.session, ++state.samples);
    std::string report = Nx2Sampler::Sample(threads.c_str(), seconds, path.string());
    report.resize((std::min)(report.size(), Param<size_t>(params, "max_chars", 60000, 0, 4000000)));
    return {{"file", path.generic_string()}, {"report", std::move(report)}};
}

// Lifts or restores the speed limit (test scripts boot through the logos and menus unlimited).
json SpeedLimit(ToolState&, const json& params) {
    auto& setting = Settings::values.use_speed_limit;
    const bool previous = setting.GetValue();
    if (params.contains("on")) {
        setting.SetValue(params.at("on").get<bool>());
    }
    return {{"previous", previous}, {"on", setting.GetValue()}};
}

json HostTiming(ToolState& state, const json& params) {
    auto& stats = state.system.CoreTiming().host_wait_stats;
    const bool previous = stats.load();
    if (params.contains("stats")) {
        stats = params.at("stats").get<bool>();
    }
    return {{"previous", previous}, {"stats", stats.load()}};
}

json ReadMemory(ToolState& state, const json& params) {
    const u64 address = AddressParam(params, "address");
    const u64 size = ReadSize(params, address);
    const std::string as = params.value("as", std::string{"hex"});
    static_cast<void>(FormatMemory(std::span<const u8>{}, as));
    std::vector<u8> bytes(size);
    if (const auto unmapped = ReadGuestMemory(Application(state).GetMemory(), address, bytes)) {
        throw std::runtime_error{fmt::format(
            "guest address {:#x} is not mapped (reading {} bytes at {:#x})", *unmapped, size, address)};
    }
    json result = {{"address", HexAddress(address)}, {"size", size}};
    result.update(FormatMemory(bytes, as));
    return result;
}

json SearchMemory(ToolState& state, const json& params) {
    const std::vector<u8> pattern = ParseHexBytes(params.at("pattern").get<std::string>());
    if (pattern.size() > 256) {
        throw std::runtime_error{"the pattern is longer than 256 bytes"};
    }
    Kernel::KProcess& process = Application(state);
    const std::string regions = params.value("regions", std::string{"heap"});
    u64 region_start = 0;
    u64 region_size = 0;
    if (regions == "heap") {
        region_start = GetInteger(process.GetPageTable().GetHeapRegionStart());
        region_size = process.GetPageTable().GetHeapRegionSize();
    } else if (regions == "all") {
        region_start = GetInteger(process.GetPageTable().GetAddressSpaceStart());
        region_size = process.GetPageTable().GetAddressSpaceSize();
    } else {
        throw std::runtime_error{fmt::format("regions \"{}\": want heap or all", regions)};
    }
    const u64 start = params.contains("start") ? (std::max)(region_start, AddressParam(params, "start"))
                                               : region_start;
    const u64 end = params.contains("end")
                        ? (std::min)(region_start + region_size, AddressParam(params, "end"))
                        : region_start + region_size;
    const size_t max_hits = Param<size_t>(params, "max_hits", 64, 1, 256);
    const int budget = Param<int>(params, "budget_ms", 2000, 1, 10000);
    const auto begin = std::chrono::steady_clock::now();
    const GuestSearch search =
        SearchGuestMemory(process.GetMemory(), pattern, start, (std::max)(start, end), max_hits,
                          begin + std::chrono::milliseconds{budget});
    json hits = json::array();
    for (const u64 hit : search.hits) {
        hits.push_back(HexAddress(hit));
    }
    json result = {
        {"hits", std::move(hits)},
        {"complete", search.complete},
        {"start", HexAddress(start)},
        {"end", HexAddress((std::max)(start, end))},
        {"scanned_bytes", search.scanned},
        {"elapsed_ms", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                 begin)
                           .count()},
    };
    if (!search.complete) {
        result["next"] = HexAddress(search.next);
    }
    return result;
}

/// The process's one scan_memory list (as the recorder, it outlives a single call's ToolState).
std::optional<ScanList>& GetScanList() {
    static std::optional<ScanList> list;
    return list;
}
std::mutex scan_mutex;

ScanType ParseScanType(const std::string& name) {
    static constexpr std::array<std::pair<std::string_view, ScanType>, 6> types{{
        {"u8", ScanType::U8}, {"u16", ScanType::U16}, {"u32", ScanType::U32},
        {"i32", ScanType::I32}, {"f32", ScanType::F32}, {"f64", ScanType::F64}}};
    const auto it = std::ranges::find(types, std::string_view{name},
                                      &std::pair<std::string_view, ScanType>::first);
    if (it == types.end()) {
        throw std::runtime_error{fmt::format("type \"{}\": want u8, u16, u32, i32, f32 or f64", name)};
    }
    return it->second;
}

std::string_view ScanTypeName(ScanType type) {
    static constexpr std::array<std::string_view, 6> names{"u8", "u16", "u32", "i32", "f32", "f64"};
    return names[static_cast<size_t>(type)];
}

/// A value of type as JSON: an integer, a float, or "nan", "inf" or "-inf".
json ScanJson(ScanType type, u64 bits) {
    const double value = ScanValue(type, bits);
    if (type != ScanType::F32 && type != ScanType::F64) {
        return static_cast<s64>(value);
    }
    if (std::isnan(value)) {
        return "nan";
    }
    if (std::isinf(value)) {
        return value < 0 ? "-inf" : "inf";
    }
    return value;
}

/// The test of a first scan or refine: {op?, value?, min?, max?}. A value of an integer type must
/// be whole; an f32 value is rounded to f32, so equal matches the stored float exactly.
ScanTest ParseScanTest(const json& params, ScanType type, bool first) {
    const std::string op = params.value("op", std::string{params.contains("value") ? "equal" : "range"});
    const auto number = [&](const char* key) {
        const json& value = params.at(key);
        if (!value.is_number()) {
            throw std::runtime_error{fmt::format("{} must be a number", key)};
        }
        const double number = value.get<double>();
        if (type != ScanType::F32 && type != ScanType::F64 && number != std::floor(number)) {
            throw std::runtime_error{fmt::format("{} must be whole for {}", key, ScanTypeName(type))};
        }
        return type == ScanType::F32 ? static_cast<double>(static_cast<float>(number)) : number;
    };
    if (op == "equal") {
        const double value = number("value");
        return {.op = ScanOp::Equal, .low = value, .high = value};
    }
    if (op == "range") {
        const ScanTest test{.op = ScanOp::Range, .low = number("min"), .high = number("max")};
        if (test.low > test.high) {
            throw std::runtime_error{"min must not exceed max"};
        }
        return test;
    }
    static constexpr std::array<std::pair<std::string_view, ScanOp>, 4> refines{{
        {"changed", ScanOp::Changed}, {"unchanged", ScanOp::Unchanged},
        {"increased", ScanOp::Increased}, {"decreased", ScanOp::Decreased}}};
    const auto it = std::ranges::find(refines, std::string_view{op},
                                      &std::pair<std::string_view, ScanOp>::first);
    if (it == refines.end() || first) {
        throw std::runtime_error{fmt::format(
            "op \"{}\": a first scan wants equal or range; a refine also changed, unchanged, "
            "increased or decreased", op)};
    }
    return {.op = it->second};
}

/// The [start, end) ranges of the game's writable memory: heap (state Normal) and data (the
/// modules' .data and .bss: CodeData and AliasCodeData), as the kernel's memory blocks say.
std::vector<std::pair<u64, u64>> ScanRegions(Kernel::KProcess& process, std::string_view regions) {
    const bool heap = regions == "heap" || regions == "both";
    const bool data = regions == "data" || regions == "both";
    if (!heap && !data) {
        throw std::runtime_error{fmt::format("regions \"{}\": want heap, data or both", regions)};
    }
    auto& table = process.GetPageTable();
    std::vector<std::pair<u64, u64>> ranges;
    const u64 end = GetInteger(table.GetAddressSpaceStart()) + table.GetAddressSpaceSize();
    for (u64 address = GetInteger(table.GetAddressSpaceStart()); address < end;) {
        Kernel::KMemoryInfo info{};
        Kernel::Svc::PageInfo page{};
        if (table.QueryInfo(&info, &page, address).IsError()) {
            break;
        }
        const auto svc = info.GetSvcMemoryInfo();
        const bool writable = True(svc.permission & Kernel::Svc::MemoryPermission::Write);
        const bool wanted = (heap && svc.state == Kernel::Svc::MemoryState::Normal) ||
                            (data && (svc.state == Kernel::Svc::MemoryState::CodeData ||
                                      svc.state == Kernel::Svc::MemoryState::AliasCodeData));
        if (writable && wanted) {
            if (!ranges.empty() && ranges.back().second == svc.base_address) {
                ranges.back().second += svc.size;
            } else {
                ranges.emplace_back(svc.base_address, svc.base_address + svc.size);
            }
        }
        if (svc.base_address + svc.size <= address) {
            break;
        }
        address = svc.base_address + svc.size;
    }
    return ranges;
}

json ScanSummary(const ScanList& list) {
    return {{"type", ScanTypeName(list.type)}, {"count", list.addresses.size()},
            {"truncated", list.truncated}, {"complete", list.complete},
            {"max_hits", MAX_SCAN_HITS}};
}

/// {action: "first", type, value | min+max, regions?, op?}, {action: "refine", op, value?, min?,
/// max?}, {action: "list", limit?, offset?}, {action: "drop"}. A cheat-engine style search: the
/// first scan keeps its hits (at most MAX_SCAN_HITS), each refine keeps those that pass.
json ScanMemory(ToolState& state, const json& params) {
    const std::string action = params.value("action", std::string{});
    if (action != "first" && action != "refine" && action != "list" && action != "drop") {
        throw std::runtime_error{"action must be first, refine, list or drop"};
    }
    std::scoped_lock lock{scan_mutex};
    std::optional<ScanList>& list = GetScanList();
    if (action == "drop") {
        const size_t count = list ? list->addresses.size() : 0;
        list.reset();
        return {{"dropped", count}};
    }
    if (action == "first") {
        const ScanType type = ParseScanType(params.at("type").get<std::string>());
        const ScanTest test = ParseScanTest(params, type, true);
        Kernel::KProcess& process = Application(state);
        const std::string regions = params.value("regions", std::string{"both"});
        const auto ranges = ScanRegions(process, regions);
        const int budget = Param<int>(params, "budget_ms", 30000, 100, 60000);
        const auto begin = std::chrono::steady_clock::now();
        list.reset();
        list = ScanGuestMemory(process.GetMemory(), ranges, type, test, MAX_SCAN_HITS,
                               begin + std::chrono::milliseconds{budget});
        json covered = json::array();
        u64 bytes = 0;
        for (const auto& [start, end] : ranges) {
            covered.push_back({{"start", HexAddress(start)}, {"end", HexAddress(end)}});
            bytes += end - start;
        }
        json result = ScanSummary(*list);
        result.update({{"regions", regions}, {"ranges", std::move(covered)}, {"region_bytes", bytes},
                       {"scanned_bytes", list->scanned},
                       {"elapsed_ms", std::chrono::duration<double, std::milli>(
                                          std::chrono::steady_clock::now() - begin)
                                          .count()}});
        return result;
    }
    if (!list) {
        throw std::runtime_error{"no scan list: start with action first"};
    }
    if (action == "refine") {
        if (!params.contains("op")) {
            throw std::runtime_error{"refine needs op"};
        }
        const ScanTest test = ParseScanTest(params, list->type, false);
        const size_t before = list->addresses.size();
        RefineScan(Application(state).GetMemory(), *list, test);
        json result = ScanSummary(*list);
        result["before"] = before;
        return result;
    }
    // action "list": the kept hits from offset on, with their value now.
    const size_t offset = Param<size_t>(params, "offset", 0, 0, list->addresses.size());
    const size_t limit = Param<size_t>(params, "limit", 100, 1, MAX_SCAN_LIST);
    auto& memory = Application(state).GetMemory();
    const size_t size = ScanSize(list->type);
    json hits = json::array();
    for (size_t i = offset; i < (std::min)(offset + limit, list->addresses.size()); ++i) {
        std::array<u8, 8> now{};
        json entry = {{"address", HexAddress(list->addresses[i])},
                      {"kept", ScanJson(list->type, list->values[i])}};
        if (!ReadGuestMemory(memory, list->addresses[i], std::span{now.data(), size})) {
            u64 bits = 0;
            std::memcpy(&bits, now.data(), size);
            entry["now"] = ScanJson(list->type, bits);
        }
        hits.push_back(std::move(entry));
    }
    json result = ScanSummary(*list);
    result.update({{"offset", offset}, {"hits", std::move(hits)}});
    return result;
}

json ReadGpuMemory(ToolState& state, const json& params) {
    if (!IsVulkan()) {
        throw std::runtime_error{"GPU memory reads need the Vulkan renderer"};
    }
    const u64 address = AddressParam(params, "gpu_address");
    const u64 size = ReadSize(params, address);
    const std::string as = params.value("as", std::string{"hex"});
    static_cast<void>(FormatMemory(std::span<const u8>{}, as));
    const u64 id = ++state.gpu_reads;
    const int timeout = Param<int>(params, "timeout_s", 5, 1, 60);
    VideoCore::Nx2Debug::RequestGpuRead({.id = id, .gpu_address = address, .size = size});
    const auto read = VideoCore::Nx2Debug::WaitGpuRead(id, std::chrono::seconds{timeout});
    if (!read) {
        VideoCore::Nx2Debug::CancelGpuRead(id);
        throw std::runtime_error{
            fmt::format("no frame was presented within {} s (paused or loading?)", timeout)};
    }
    if (!read->error.empty()) {
        throw std::runtime_error{read->error};
    }
    json result = {{"gpu_address", HexAddress(address)},
                   {"size", size},
                   {"address_space", read->address_space}};
    result.update(FormatMemory(read->bytes, as));
    return result;
}

/// The controller input and input_record use: player 1, or the handheld controller when player 1
/// is not connected.
std::pair<HID::EmulatedController*, std::string> InputController(ToolState& state) {
    auto& hid = state.system.HIDCore();
    HID::EmulatedController* const player1 = hid.GetEmulatedController(HID::NpadIdType::Player1);
    if (player1->IsConnected()) {
        return {player1, "player1"};
    }
    HID::EmulatedController* const handheld = hid.GetEmulatedController(HID::NpadIdType::Handheld);
    if (handheld->IsConnected()) {
        return {handheld, "handheld"};
    }
    throw std::runtime_error{"neither player 1 nor the handheld controller is connected"};
}

/// A kit lock with its stall breadcrumb (common/nx2_kit_stall.h): waits, then holds.
class KitLock {
public:
    KitLock(std::mutex& mutex, const char* name, const char* op)
        : stall{name, op}, lock{mutex} {
        stall.Held();
    }

private:
    Common::KitStall::Scope stall;
    std::unique_lock<std::mutex> lock;
};

/// What the kit runs now, for the watchdog: a recording, input calls, a luma watch.
std::atomic<bool> recording_active{};
std::atomic<int> input_calls{};

std::string KitActive() {
    std::string active;
    const auto add = [&active](std::string_view name) {
        active += active.empty() ? "" : " ";
        active += name;
    };
    if (recording_active.load(std::memory_order_relaxed)) {
        add("record");
    }
    if (input_calls.load(std::memory_order_relaxed) != 0) {
        add("injector");
    }
    if (VideoCore::Nx2Debug::LumaWatchArmed()) {
        add("luma");
    }
    return active;
}

/// Names kit stalls in the log while the kit is active (docs/nx2/debug-mcp.md, "If the kit
/// hangs"): every 250 ms it checks the breadcrumbs, and logs one Error line per stall older than
/// EDEN_NX2_KIT_STALL_MS (default 2000), again every 10 s while it lasts.
class Watchdog {
public:
    ~Watchdog() {
        Stop();
    }

    /// Starts the thread unless it runs or Stop runs. Call after marking the kit active.
    void Ensure() {
        std::scoped_lock lock{mutex};
        if (running || stopping) {
            return;
        }
        if (thread.joinable()) {
            thread.join();
        }
        stop = false;
        running = true;
        thread = std::thread{[this] { Loop(); }};
    }

    void Stop() {
        std::thread done;
        {
            std::scoped_lock lock{mutex};
            stopping = true;
            stop = true;
            done = std::move(thread);
        }
        if (done.joinable()) {
            done.join();
        }
        std::scoped_lock lock{mutex};
        stopping = false;
    }

private:
    void Loop() {
        Common::SetCurrentThreadName("KitWatchdog");
        static const u64 limit_ns = [] {
            const char* const text = std::getenv("EDEN_NX2_KIT_STALL_MS");
            const long ms = text != nullptr ? std::strtol(text, nullptr, 10) : 0;
            return static_cast<u64>(ms > 0 ? ms : 2000) * 1'000'000;
        }();
        std::unordered_map<u64, u64> reported;
        while (!stop) {
            std::this_thread::sleep_for(250ms);
            const u64 now = Common::KitStall::NowNs();
            const auto stalls = Common::KitStall::Check(limit_ns, now);
            const std::string active = KitActive();
            for (const auto& stall : stalls) {
                auto [it, fresh] = reported.try_emplace(stall.key, now);
                if (!fresh && now - it->second < 10'000'000'000ULL) {
                    continue;
                }
                it->second = now;
                LOG_ERROR(Debug, "KIT STALL: {}; present count {}; active: {}", stall.text,
                          g_nx2_present_count.load(std::memory_order_relaxed),
                          active.empty() ? "none" : active);
                if (!first_reported) {
                    first_reported = true;
                    if (const auto hook = Common::KitStall::g_first_stall_hook.load()) {
                        hook();
                    }
                }
            }
            // Exit only when nothing runs and no breadcrumb is open, so a stall that is still
            // young when its call ends is still reported.
            if (stalls.empty() && active.empty() && !Common::KitStall::AnyOpen()) {
                std::scoped_lock lock{mutex};
                if (KitActive().empty()) {
                    running = false;
                    return;
                }
            }
        }
        std::scoped_lock lock{mutex};
        running = false;
    }

    std::mutex mutex;
    std::thread thread;
    bool running{};
    bool stopping{};
    bool first_reported{};
    std::atomic<bool> stop{};
};

Watchdog& GetWatchdog() {
    static Watchdog watchdog;
    return watchdog;
}

/// Refuses a kit feature when EDEN_NX2_KIT_HOOKS=0.
void RequireKitHooks(std::string_view feature) {
    if (!Common::KitStall::HooksEnabled()) {
        throw std::runtime_error{fmt::format(
            "{} needs the kit's hooks, which EDEN_NX2_KIT_HOOKS=0 turned off", feature)};
    }
}

void PresentReply(u64 present);

/// True on the present thread while PresentReply runs its own npad update, so the recorder keeps
/// only that update's reads as the entry the game reads next.
thread_local bool in_forced_npad_update = false;

/// g_nx2_present_reply_hook's users (a frame-step replay, a recording in mode "any"): the hook and
/// the npad update stamps are on only while one runs.
std::mutex reply_hook_mutex;
int reply_hook_users = 0;

/// The trainer's step trace at each present's reply (SetPresentReplyTrace), or null.
std::atomic<void (*)(u64)> reply_trace{};

/// Under reply_hook_mutex: the reply hook is on while a replay, a recording or the trace uses it.
void UpdateReplyHook() {
    const bool on = reply_hook_users > 0 || reply_trace.load(std::memory_order_relaxed) != nullptr;
    g_nx2_present_reply_hook.store(on ? PresentReply : nullptr, std::memory_order_release);
}

void UseReplyHook() {
    std::scoped_lock lock{reply_hook_mutex};
    if (reply_hook_users++ == 0) {
        ::Service::HID::g_npad_update_stamps.store(true, std::memory_order_relaxed);
    }
    UpdateReplyHook();
}

void DropReplyHook() {
    std::scoped_lock lock{reply_hook_mutex};
    --reply_hook_users;
    UpdateReplyHook();
    if (reply_hook_users == 0) {
        ::Service::HID::g_npad_update_stamps.store(false, std::memory_order_relaxed);
    }
}

/// The frame-step replay running now, for the reply hook: the presents it covers, and the offsets
/// from its start whose newest npad entry at the reply was written before the present was counted
/// (the game then reads the previous step once more).
struct ReplaySync {
    std::mutex mutex;
    bool active{};
    /// npad_sync: one npad update at each covered present's reply, so the newest entry carries
    /// that present's step when the game resumes.
    bool sync{true};
    u64 start{};
    u64 end{};
    std::vector<u64> stale;
};

ReplaySync& GetReplaySync() {
    static ReplaySync sync;
    return sync;
}

/// input_sequence watch while it runs: active is the one check the reply hook makes otherwise.
struct PresentWatch {
    std::atomic<bool> active{};
    std::mutex mutex;
    std::optional<ValueWatch> watch;
};

PresentWatch& GetPresentWatch() {
    static PresentWatch watch;
    return watch;
}

/// Reply hook part of a watch: reads the watched values at the present's reply.
void WatchPresent(u64 present) {
    PresentWatch& watch = GetPresentWatch();
    KitLock lock{watch.mutex, "watch mutex", __func__};
    if (watch.watch) {
        watch.watch->Sample(present);
    }
}

/// watch: [{address, as}] for the frame-step call over presents [start, end).
std::vector<std::pair<u64, ScanType>> ParseWatch(const json& list) {
    if (!list.is_array() || list.empty() || list.size() > MAX_WATCH) {
        throw std::runtime_error{fmt::format("watch must be an array of 1 to {} {{address, as}}", MAX_WATCH)};
    }
    std::vector<std::pair<u64, ScanType>> addresses;
    for (const json& entry : list) {
        if (!entry.is_object() || !entry.contains("address") || !entry.contains("as")) {
            throw std::runtime_error{"a watch entry is {address, as}"};
        }
        const ScanType type = ParseScanType(entry.at("as").get<std::string>());
        const u64 address = AddressParam(entry, "address");
        if (address % ScanSize(type) != 0) {
            throw std::runtime_error{fmt::format("watch address {:#x} is not aligned to its {} bytes",
                                                 address, ScanSize(type))};
        }
        addresses.emplace_back(address, type);
    }
    return addresses;
}

/// Reply hook part of a replay: notes present when the last npad update started before its count.
/// True when the replay wants the npad updated at this reply.
bool NoteReplayReply(u64 present) {
    ReplaySync& sync = GetReplaySync();
    KitLock lock{sync.mutex, "replay sync mutex", __func__};
    if (!sync.active || present < sync.start || present >= sync.end) {
        return false;
    }
    const u64 counted_ns =
        g_nx2_present_times_ns[present % g_nx2_present_times_ns.size()].load(std::memory_order_acquire);
    if (counted_ns != 0 &&
        ::Service::HID::g_npad_update_ns.load(std::memory_order_acquire) < counted_ns) {
        sync.stale.push_back(present - sync.start);
    }
    return sync.sync;
}

/// A list of distinct clip frame numbers (0 to clip_frames - 1), at most max of them, sorted.
std::vector<u64> ClipFrames(const json& list, u64 clip_frames, size_t max, std::string_view name) {
    if (!list.is_array() || list.empty() || list.size() > max) {
        throw std::runtime_error{fmt::format("{} must be an array of 1 to {} clip frames", name, max)};
    }
    std::vector<u64> frames;
    for (const json& frame : list) {
        if (!frame.is_number_integer() || frame.get<s64>() < 0 || frame.get<u64>() >= clip_frames) {
            throw std::runtime_error{fmt::format("{}: {} is not a clip frame (0 to {})", name,
                                                 frame.dump(), clip_frames - 1)};
        }
        frames.push_back(frame.get<u64>());
    }
    std::ranges::sort(frames);
    if (std::ranges::adjacent_find(frames) != frames.end()) {
        throw std::runtime_error{fmt::format("{} names a frame twice", name)};
    }
    return frames;
}

/// input_sequence capture_frames: the input wait loop requests a 1280x720 screenshot of each
/// named clip frame, so nothing runs on the present path. A request goes in once the composite
/// of the present before the target has begun; a shot can still land later (adjacent frames, or
/// a composite that had not reached its check), so each entry names the present it shows. The
/// renderer's callback thread encodes and writes the file.
class ClipCaptures {
public:
    ClipCaptures(ToolState& state, std::vector<u64> frames, u64 clip_present)
        : renderer{state.system.GPU().Renderer()},
          directory{state.dump_directory / fmt::format("clip-{}-{:03}", state.session, ++state.clips)} {
        std::filesystem::create_directories(directory);
        for (const u64 frame : frames) {
            shots.push_back({.frame = frame, .target = clip_present + frame, .present = {},
                             .written = {}});
        }
    }

    /// From the wait loop: notes the present of a finished shot, then requests the next one.
    void Poll() {
        if (pending) {
            if (renderer.IsScreenshotPending()) {
                return;
            }
            shots[*pending].present = renderer.ScreenshotPresent();
            pending.reset();
        }
        if (next == shots.size() || renderer.IsScreenshotPending() ||
            g_nx2_composite_present.load(std::memory_order_acquire) + 1 < shots[next].target) {
            return;
        }
        Shot& shot = shots[next];
        const auto layout = Layout::DefaultFrameLayout(1280, 720);
        const auto pixels = std::make_shared<std::vector<u8>>(size_t{layout.width} * layout.height * 4);
        const auto done = std::make_shared<std::promise<std::string>>();
        shot.written = done->get_future();
        const auto path = directory / fmt::format("frame-{:05}.png", shot.frame);
        renderer.RequestScreenshot(
            pixels->data(),
            [pixels, done, layout, path](bool invert_y) {
                try {
                    const std::vector<u8> png = ScreenshotPng(*pixels, layout, invert_y);
                    std::ofstream file{path, std::ios::binary};
                    file.write(reinterpret_cast<const char*>(png.data()),
                               static_cast<std::streamsize>(png.size()));
                    done->set_value(file ? path.generic_string() : "");
                } catch (const std::exception&) {
                    done->set_value("");
                }
            },
            layout);
        pending = next++;
    }

    /// Requests and waits for the rest, up to timeout, and returns
    /// [{frame, target_present, present, path}]; a shot that did not finish has an error instead.
    json Finish(const ToolState& state, std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while ((pending || next < shots.size()) && std::chrono::steady_clock::now() < deadline &&
               !state.stopping.load(std::memory_order_relaxed)) {
            Poll();
            std::this_thread::sleep_for(1ms);
        }
        json list = json::array();
        for (Shot& shot : shots) {
            json entry = {{"frame", shot.frame}, {"target_present", shot.target}};
            const bool written =
                shot.written.valid() && shot.present &&
                shot.written.wait_until(deadline) == std::future_status::ready;
            const std::string path = written ? shot.written.get() : "";
            if (!path.empty()) {
                entry["present"] = *shot.present;
                entry["path"] = path;
            } else {
                entry["error"] = !shot.written.valid() ? "not requested: no composite reached it in time"
                                 : written             ? "the file could not be written"
                                                       : "the screenshot did not finish in time";
            }
            list.push_back(std::move(entry));
        }
        std::ofstream{directory / "captures.json"} << list.dump(1) << '\n';
        return list;
    }

    [[nodiscard]] std::string Directory() const {
        return directory.generic_string();
    }

private:
    struct Shot {
        u64 frame;
        u64 target;
        std::optional<u64> present;
        std::future<std::string> written;
    };
    VideoCore::RendererBase& renderer;
    std::filesystem::path directory;
    std::vector<Shot> shots;
    size_t next{};
    std::optional<size_t> pending;
};

/// Holds the steps on player 1 (or the handheld controller when player 1 is not connected) from
/// the next present on, and returns once they are done. The injector stops by itself after the
/// last step; leaving this function by any path removes it. The first clip_offset frames are a
/// pre-roll: measure leaves them out.
json RunInput(ToolState& state, const std::vector<InputStep>& steps, const json& params,
              const ClientGone& client_gone, u64 clip_offset = 0) {
    // Timed steps follow host time, for games that advance by elapsed time rather than per present.
    const bool timed = steps.front().ms != 0;
    u64 total = 0;
    for (const InputStep& step : steps) {
        if ((step.ms != 0) != timed) {
            throw std::runtime_error{"steps mix frames and ms; use one unit per call"};
        }
        total += timed ? step.ms : step.frames;
    }
    if (total > (timed ? MAX_INPUT_MS : MAX_INPUT_FRAMES)) {
        throw std::runtime_error{fmt::format("{} {} in total; at most {} per call", total,
                                             timed ? "ms" : "frames",
                                             timed ? MAX_INPUT_MS : MAX_INPUT_FRAMES)};
    }
    const bool measure = params.value("measure", false);
    if (measure && !timed && total - clip_offset > MAX_MEASURED_PRESENTS) {
        throw std::runtime_error{
            fmt::format("measured input is limited to {} presents", MAX_MEASURED_PRESENTS)};
    }
    // start_present: step 0 at exactly that present, for a clip synced to a present the client
    // found. The injector answers by the present count, so installing it early is exact.
    std::optional<u64> start_present;
    if (params.contains("start_present")) {
        const json& value = params.at("start_present");
        if (timed) {
            throw std::runtime_error{"start_present needs frame steps, not ms"};
        }
        if (!value.is_number_integer() || value.get<s64>() < 1) {
            throw std::runtime_error{"start_present must be a present count of 1 or more"};
        }
        start_present = value.get<u64>();
        RequireKitHooks("start_present");
    }
    if (params.contains("npad_sync") && !params.at("npad_sync").is_boolean()) {
        throw std::runtime_error{"npad_sync must be true or false"};
    }
    const bool kit_hooks = Common::KitStall::HooksEnabled();
    // Default on with the kit's hooks; asking for it without them fails.
    const bool npad_sync = params.value("npad_sync", kit_hooks);
    if (npad_sync) {
        RequireKitHooks("npad_sync");
    }
    std::vector<std::pair<u64, ScanType>> watched;
    if (params.contains("watch")) {
        if (timed) {
            throw std::runtime_error{"watch needs frame steps, not ms"};
        }
        RequireKitHooks("watch");
        watched = ParseWatch(params.at("watch"));
    }
    std::vector<u64> capture_frames;
    if (params.contains("capture_frames")) {
        if (timed) {
            throw std::runtime_error{"capture_frames needs frame steps, not ms"};
        }
        capture_frames = ClipFrames(params.at("capture_frames"), total - clip_offset,
                                    MAX_CAPTURE_FRAMES, "capture_frames");
        if (state.system.IsPaused()) {
            throw std::runtime_error{"paused: capture_frames needs presents; resume first"};
        }
    }
    const auto picked = InputController(state);
    HID::EmulatedController* const controller = picked.first;
    const std::string& name = picked.second;
    const u64 counted = g_nx2_present_count.load(std::memory_order_relaxed);
    if (start_present && *start_present <= counted) {
        throw std::runtime_error{fmt::format(
            "start_present {} has passed: the game is at present {}", *start_present, counted)};
    }
    if (start_present && *start_present - counted > MAX_INPUT_FRAMES) {
        throw std::runtime_error{fmt::format("start_present {} is more than {} presents ahead of {}",
                                             *start_present, MAX_INPUT_FRAMES, counted)};
    }
    const u64 start = start_present.value_or(counted + 1);
    const u64 lead = start - counted - 1;
    const u64 total_s = timed ? total / 1000 : (lead + total) / 20;
    const int timeout = Param<int>(params, "timeout_s", static_cast<int>(total_s + 10), 1, 900);
    if (timed && std::chrono::seconds{timeout} <= std::chrono::milliseconds{total}) {
        throw std::runtime_error{
            fmt::format("timeout_s {} does not exceed the {} ms of input", timeout, total)};
    }
    const auto begin = std::chrono::steady_clock::now();
    input_calls.fetch_add(1, std::memory_order_relaxed);
    GetWatchdog().Ensure();
    SCOPE_EXIT {
        input_calls.fetch_sub(1, std::memory_order_relaxed);
    };
    if (!watched.empty()) {
        PresentWatch& watch = GetPresentWatch();
        KitLock lock{watch.mutex, "watch mutex", __func__};
        watch.watch.emplace(Application(state).GetMemory(), std::move(watched), start, start + total);
        watch.active.store(true, std::memory_order_release);
    }
    SCOPE_EXIT {
        PresentWatch& watch = GetPresentWatch();
        if (watch.active.load(std::memory_order_relaxed)) {
            watch.active.store(false, std::memory_order_release);
            KitLock lock{watch.mutex, "watch mutex", __func__};
            watch.watch.reset();
        }
    };
    if (!timed && kit_hooks) {
        ReplaySync& sync = GetReplaySync();
        KitLock lock{sync.mutex, "replay sync mutex", __func__};
        sync.active = true;
        sync.sync = npad_sync;
        sync.start = start;
        sync.end = start + total;
        sync.stale.clear();
        UseReplyHook();
    }
    SCOPE_EXIT {
        if (!timed && kit_hooks) {
            ReplaySync& sync = GetReplaySync();
            {
                KitLock lock{sync.mutex, "replay sync mutex", __func__};
                sync.active = false;
            }
            DropReplyHook();
        }
    };
    const auto script = std::make_shared<const std::vector<InputStep>>(steps);
    if (timed) {
        controller->SetInputInjector([script, begin] {
            const auto elapsed = std::chrono::steady_clock::now() - begin;
            return InputAtTime(
                *script, static_cast<u64>(
                             std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()));
        });
    } else {
        controller->SetInputInjector([script, start] {
            return InputAt(*script, start, g_nx2_present_count.load(std::memory_order_relaxed));
        });
    }
    SCOPE_EXIT {
        controller->SetInputInjector({});
    };
    // The first present whose whole count had the injector: start, unless the start present
    // was counted while it was installed.
    const u64 installed_at = g_nx2_present_count.load(std::memory_order_relaxed);
    const u64 landed = installed_at < start ? start : installed_at + 1;
    if ((measure || start_present) && !timed && landed != start) {
        throw std::runtime_error{"present advanced while installing input; input released"};
    }
    const auto deadline = begin + std::chrono::seconds{timeout};
    const auto timed_end = begin + std::chrono::milliseconds{timed ? total : 0};
    std::optional<ClipCaptures> captures;
    if (!capture_frames.empty()) {
        captures.emplace(state, std::move(capture_frames), start + clip_offset);
    }
    u64 now = start - 1;
    for (;;) {
        if (captures) {
            captures->Poll();
        }
        now = g_nx2_present_count.load(std::memory_order_relaxed);
        if (timed ? std::chrono::steady_clock::now() >= timed_end : now >= start + total) {
            break;
        }
        if (state.stopping.load(std::memory_order_relaxed)) {
            throw std::runtime_error{"the game is shutting down; input released"};
        }
        if (client_gone()) {
            LOG_INFO(Debug, "Debug server client left during input; input released");
            throw std::runtime_error{"the client disconnected; input released"};
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error{fmt::format(
                "{} of {} {} within {} s (paused or loading?); input released",
                timed ? static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::steady_clock::now() - begin)
                                             .count())
                      : (now >= start ? now - start : 0),
                total, timed ? "ms passed" : "frames were presented", timeout)};
        }
        std::this_thread::sleep_for(timed || captures ? 1ms : 2ms);
    }
    // Timed: the presents that began inside the window, except the last, whose timestamp may
    // still be pending. The count was read after the window ended, so drop presents stamped later.
    const auto ns = [](auto time) -> u64 {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
    };
    const u64 capacity = g_nx2_present_times_ns.size();
    u64 presents = timed ? (now >= start + 1 ? now - start : 0) : total;
    while (timed && presents != 0 && presents <= capacity &&
           g_nx2_present_times_ns[(start + presents - 1) % capacity].load() > ns(timed_end)) {
        --presents;
    }
    json result = {
        {"controller", name},
        {"steps", steps.size()},
        {"frames", presents},
        {"first_present", start},
        {"elapsed_s",
         std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count()},
    };
    if (captures) {
        result["capture_dir"] = captures->Directory();
        result["captures"] = captures->Finish(state, 5s);
    }
    if (PresentWatch& watch = GetPresentWatch(); watch.active.load(std::memory_order_relaxed)) {
        KitLock lock{watch.mutex, "watch mutex", __func__};
        result["watch"] = watch.watch->Json();
    }
    if (timed) {
        result["ms"] = total;
        result["clock"] = "host_time";
    } else if (measure || start_present) {
        result["landed_present"] = landed;
        result["npad_sync"] = npad_sync;
        if (kit_hooks) {
            ReplaySync& sync = GetReplaySync();
            KitLock lock{sync.mutex, "replay sync mutex", __func__};
            result["stale_presents"] = sync.stale;
        }
    }
    if (measure) {
        if (timed && (presents < 2 || presents > MAX_MEASURED_PRESENTS)) {
            json empty = IntervalStats({}, 60.0, true, true);
            empty.update({{"supported", "held-input-v1"}, {"first_present", start},
                          {"last_present", start + presents - 1}, {"expected_intervals", 0},
                          {"complete", false}, {"covered_presents", 0}});
            result["measurement"] = std::move(empty);
        } else {
            result["measurement"] =
                ReadInputFrameTimes(start + clip_offset, presents - clip_offset, ns(begin),
                                    ns(std::chrono::steady_clock::now()));
        }
        json& measurement = result["measurement"];
        if (timed && measurement["complete"].get<bool>()) {
            // Intervals only span the counted presents; a stall before the first or after the
            // last one is not in them, so report the uncovered ends of the window.
            const u64 first_ns = g_nx2_present_times_ns[start % capacity].load();
            const u64 last_ns = g_nx2_present_times_ns[(start + presents - 1) % capacity].load();
            measurement["window_ms"] = total;
            measurement["head_ms"] = static_cast<double>(first_ns - ns(begin)) / 1'000'000.0;
            measurement["tail_ms"] =
                static_cast<double>(ns(timed_end) - (std::min)(last_ns, ns(timed_end))) /
                1'000'000.0;
        }
    }
    return result;
}

json Input(ToolState& state, const json& params, const ClientGone& client_gone) {
    json step = params;
    step.erase("timeout_s");
    step.erase("measure");
    step.erase("start_present");
    step.erase("npad_sync");
    return RunInput(state, {ParseInputStep(step)}, params, client_gone);
}

/// {steps, pre_roll?, ...}: pre_roll (an input_record stop reply's pre_roll steps) plays first,
/// so the clip starts pre_roll's frames after step 0's present, as in the live run, with no gap
/// between them. Both count against the step and frame limits of one call; measure covers the
/// clip only.
json InputSequence(ToolState& state, const json& params, const ClientGone& client_gone) {
    const json& list = params.at("steps");
    if (!list.is_array() || list.empty() || list.size() > MAX_INPUT_STEPS) {
        throw std::runtime_error{fmt::format("steps must be an array of 1 to {}", MAX_INPUT_STEPS)};
    }
    const json pre_roll = params.value("pre_roll", json::array());
    if (!pre_roll.is_array() || pre_roll.size() + list.size() > MAX_INPUT_STEPS) {
        throw std::runtime_error{
            fmt::format("pre_roll must be an array; with steps at most {} steps", MAX_INPUT_STEPS)};
    }
    std::vector<InputStep> steps;
    u64 pre_roll_frames = 0;
    const auto parse = [&steps](const json& from, std::string_view name) {
        for (size_t index = 0; index < from.size(); ++index) {
            try {
                steps.push_back(ParseInputStep(from[index]));
            } catch (const std::exception& error) {
                throw std::runtime_error{fmt::format("{} {}: {}", name, index, error.what())};
            }
        }
    };
    parse(pre_roll, "pre_roll step");
    for (const InputStep& step : steps) {
        if (step.ms != 0) {
            throw std::runtime_error{"pre_roll needs frame steps, not ms"};
        }
        pre_roll_frames += step.frames;
    }
    parse(list, "step");
    json result = RunInput(state, steps, params, client_gone, pre_roll_frames);
    if (!pre_roll.empty()) {
        result["pre_roll_frames"] = pre_roll_frames;
        result["clip_present"] = result["first_present"].get<u64>() + pre_roll_frames;
    }
    return result;
}

/// The process's one input_record recording. RecordPresent reads it on the present thread; the
/// input_record handler and Service shutdown change it. All of them hold mutex.
struct Recorder {
    std::mutex mutex;
    /// Player 1 and the handheld controller. Their bindings and connections are separate, and
    /// the game reads every connected npad, so the owner's pad can drive either.
    std::array<HID::EmulatedController*, 2> sources{};
    std::string controller_name;
    std::optional<InputRecording> recording;
    /// Mode "any" (default): a count's step holds every button the npad updater gave the game at
    /// any time during that count. Mode "sample": the input at the start of the count only.
    bool any{true};
    /// Mode "any": the count still running, closed into the recordings at the next present.
    /// input has the sticks of the count's start; reply_input those of the present's reply.
    struct OpenCount {
        u64 present;
        HID::InjectedInput input;
        std::optional<HID::InjectedInput> reply_input;
        std::chrono::steady_clock::time_point counted;
        /// npad_sync: the npad update at this reply runs now; synced is what it wrote.
        bool sync_pending{};
        std::optional<HID::InjectedInput> synced;
    };
    std::optional<OpenCount> open;
    /// Mode "any": the same presents with the sticks of the count's start, the phase before
    /// 2026-11-10, for one A/B (temporary).
    std::optional<InputRecording> at_count;
    /// Mode "any": the time from each present's count to its reply, which the game spends blocked
    /// in the present call.
    u64 blocked_sum_us{};
    u64 blocked_max_us{};
    u64 blocked_presents{};
    bool uses_reply_hook{};
    /// Mode "any": one npad update at each present's reply, as replays do, so the reply stick
    /// is exactly the newest npad entry the game reads next.
    bool npad_sync{};
    /// record_anchor: the pre-roll ring and the anchor present it was marked with. The ring
    /// records as mode "any" with npad_sync, and goes on through recordings.
    std::optional<PreRollRing> pre_roll;
    std::optional<u64> anchor;
    /// The pre-roll of the recording running now: cut from the ring once the ring reached the
    /// recording's start, before newer presents can push it out.
    struct PreRollCut {
        std::optional<u64> anchor;
        u64 first_present;
        std::vector<InputStep> steps;
    };
    std::optional<PreRollCut> cut;
};

Recorder& GetRecorder() {
    static Recorder recorder;
    return recorder;
}

std::array<HID::EmulatedController*, 2> RecordedControllers(ToolState& state) {
    auto& hid = state.system.HIDCore();
    return {hid.GetEmulatedController(HID::NpadIdType::Player1),
            hid.GetEmulatedController(HID::NpadIdType::Handheld)};
}

/// Adds what controller gives the game now to input: the two getters the npad updater reads,
/// limited to the buttons an input step can hold. Buttons are ORed; each stick keeps the larger
/// deflection.
void AddGameInput(HID::InjectedInput& input, HID::NpadButton held, const HID::AnalogSticks& sticks);

void AddGameInput(HID::InjectedInput& input, const HID::EmulatedController& controller) {
    AddGameInput(input, controller.GetNpadButtons().raw, controller.GetSticks());
}

/// Adds the buttons and sticks of one controller read to input.
void AddGameInput(HID::InjectedInput& input, HID::NpadButton held, const HID::AnalogSticks& sticks) {
    for (const auto& [name, button] : INPUT_BUTTONS) {
        input.buttons |= held & button;
    }
    const auto keep_larger = [](std::optional<HID::AnalogStickState>& kept,
                                const HID::AnalogStickState& stick) {
        const auto size = [](const HID::AnalogStickState& value) {
            return s64{value.x} * value.x + s64{value.y} * value.y;
        };
        if (!kept || size(stick) > size(*kept)) {
            kept = stick;
        }
    };
    keep_larger(input.left, sticks.left);
    keep_larger(input.right, sticks.right);
}

/// What the game is given now from the connected recorded controllers.
HID::InjectedInput GameInput(std::span<HID::EmulatedController* const> controllers) {
    HID::InjectedInput input{.buttons = {}, .left = HID::AnalogStickState{},
                             .right = HID::AnalogStickState{}};
    for (const HID::EmulatedController* const controller : controllers) {
        if (controller->IsConnected()) {
            AddGameInput(input, *controller);
        }
    }
    return input;
}

/// For status: each recorded controller's connection, npad style and whether it gives input now.
json ControllerStates(std::span<HID::EmulatedController* const> controllers) {
    json list = json::array();
    for (const HID::EmulatedController* const controller : controllers) {
        HID::InjectedInput input{};
        AddGameInput(input, *controller);
        const bool active = input.buttons != HID::NpadButton::None || input.left->x != 0 ||
                            input.left->y != 0 || input.right->x != 0 || input.right->y != 0;
        list.push_back(
            {{"npad", controller->GetNpadIdType() == HID::NpadIdType::Handheld ? "handheld"
                                                                                : "player1"},
             {"connected", controller->IsConnected()},
             {"style", static_cast<int>(controller->GetNpadStyleIndex())},
             {"input", active}});
    }
    return list;
}

void ClearRecordHooks(Recorder& recorder) {
    g_nx2_present_hook.store(nullptr, std::memory_order_release);
    if (recorder.uses_reply_hook) {
        recorder.uses_reply_hook = false;
        DropReplyHook();
    }
    HID::g_npad_read_hook.store(nullptr, std::memory_order_release);
}

void RecordPresent(u64 present);
void RecordNpadRead(const HID::EmulatedController& controller, HID::NpadButton held,
                    const HID::AnalogSticks& sticks);

/// Installs the hooks of a recording or of the pre-roll ring; the reply hook only once.
void InstallRecordHooks(Recorder& recorder) {
    g_nx2_present_hook.store(RecordPresent, std::memory_order_release);
    if (recorder.any) {
        HID::g_npad_read_hook.store(RecordNpadRead, std::memory_order_release);
        if (!recorder.uses_reply_hook) {
            recorder.uses_reply_hook = true;
            UseReplyHook();
        }
    }
}

/// Takes the running recording's pre-roll from the ring once the ring holds every present
/// before the recording's start, or at once with now (the recording stops).
void CutPreRoll(Recorder& recorder, bool now) {
    if (!recorder.recording || !recorder.pre_roll || recorder.cut) {
        return;
    }
    const u64 start = recorder.recording->StartPresent();
    if (!now && recorder.pre_roll->NextPresent() < start) {
        return;
    }
    recorder.cut = Recorder::PreRollCut{.anchor = recorder.anchor,
                                        .first_present = recorder.pre_roll->FirstPresent(),
                                        .steps = recorder.pre_roll->StepsBefore(start)};
}

/// Mode "any": moves the open count into the pre-roll ring and the recordings: its buttons with
/// the sticks of the present's reply (or of its start, without a reply), and with the sticks of
/// its start. False once a recording is truncated.
bool CloseCount(Recorder& recorder) {
    if (!recorder.open) {
        return true;
    }
    const Recorder::OpenCount open = *recorder.open;
    recorder.open.reset();
    HID::InjectedInput at_reply = open.reply_input.value_or(open.input);
    at_reply.buttons = open.input.buttons;
    if (recorder.pre_roll) {
        recorder.pre_roll->Add(open.present, at_reply);
        CutPreRoll(recorder, false);
    }
    if (!recorder.recording) {
        return true;
    }
    const bool kept = recorder.recording->Add(open.present, at_reply);
    return recorder.at_count->Add(open.present, open.input) && kept;
}

/// g_nx2_present_hook while input_record records. Runs right after the present count becomes
/// present, so it reads the input the injector and the owner's pad give at the start of that
/// count. Mode "any" then keeps the count open for the npad reads during it.
void RecordPresent(u64 present) {
    const auto counted = std::chrono::steady_clock::now();
    Recorder& recorder = GetRecorder();
    KitLock lock{recorder.mutex, "recorder mutex", __func__};
    const bool recording = recorder.recording && !recorder.recording->Truncated() &&
                           present >= recorder.recording->StartPresent();
    if ((!recording && !recorder.pre_roll) ||
        (recorder.open && present <= recorder.open->present)) {
        return;
    }
    const HID::InjectedInput input = GameInput(recorder.sources);
    const bool kept =
        recorder.any ? CloseCount(recorder) : recorder.recording->Add(present, input);
    if (!kept && !recorder.pre_roll) {
        ClearRecordHooks(recorder);
        return;
    }
    if (recorder.any) {
        recorder.open = Recorder::OpenCount{.present = present,
                                            .input = input,
                                            .reply_input = std::nullopt,
                                            .counted = counted,
                                            .sync_pending = false,
                                            .synced = std::nullopt};
    }
}

/// g_nx2_present_reply_hook in mode "any": takes the open count's sticks at the present's reply,
/// after which the game reads its next input, and the time it was blocked.
void RecordPresentReply(u64 present) {
    Recorder& recorder = GetRecorder();
    KitLock lock{recorder.mutex, "recorder mutex", __func__};
    if (!recorder.open || recorder.open->present != present) {
        return;
    }
    const u64 blocked_us = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                std::chrono::steady_clock::now() -
                                                recorder.open->counted)
                                                .count());
    recorder.blocked_sum_us += blocked_us;
    recorder.blocked_max_us = (std::max)(recorder.blocked_max_us, blocked_us);
    ++recorder.blocked_presents;
    // With npad_sync, exactly the entry the update wrote; otherwise the getters now.
    HID::InjectedInput input =
        recorder.open->synced ? *recorder.open->synced : GameInput(recorder.sources);
    recorder.open->sync_pending = false;
    recorder.open->synced.reset();
    recorder.open->input.buttons |= input.buttons;
    recorder.open->reply_input = input;
}

/// HID::g_npad_read_hook in mode "any": ORs the buttons the npad updater reads for the game into
/// the open count. A read that may belong to another count (the count moved around it) is left
/// out, so scripted input of the next count never leaks into this one.
void RecordNpadRead(const HID::EmulatedController& controller, HID::NpadButton held,
                    const HID::AnalogSticks& sticks) {
    Recorder& recorder = GetRecorder();
    KitLock lock{recorder.mutex, "recorder mutex", __func__};
    if (!recorder.open || std::ranges::find(recorder.sources, &controller) == recorder.sources.end()) {
        return;
    }
    const u64 before = g_nx2_present_count.load(std::memory_order_acquire);
    if (before != recorder.open->present) {
        return;
    }
    if (recorder.open->sync_pending && in_forced_npad_update) {
        // The update at this present's reply: keep the values it wrote for the game.
        if (!recorder.open->synced) {
            recorder.open->synced = HID::InjectedInput{};
        }
        AddGameInput(*recorder.open->synced, held, sticks);
    }
    HID::InjectedInput input{};
    AddGameInput(input, controller);
    if (g_nx2_present_count.load(std::memory_order_acquire) == before) {
        recorder.open->input.buttons |= input.buttons;
    }
}

/// Mode "any" with npad_sync: whether the open count wants the npad updated at its reply.
bool RecordingWantsSync(u64 present) {
    Recorder& recorder = GetRecorder();
    KitLock lock{recorder.mutex, "recorder mutex", __func__};
    if (!recorder.npad_sync || !recorder.open || recorder.open->present != present) {
        return false;
    }
    recorder.open->sync_pending = true;
    recorder.open->synced.reset();
    return true;
}

/// g_nx2_present_reply_hook: the stale check of a replay, then the npad update a replay or a
/// recording asks for (outside the recorder lock: the update takes the npad lock and then calls
/// the npad read hook, which keeps what it wrote), then the recording's reply sample.
void PresentReply(u64 present) {
    const bool replay_sync = NoteReplayReply(present);
    const bool record_sync = RecordingWantsSync(present);
    if (replay_sync || record_sync) {
        in_forced_npad_update = true;
        static_cast<void>(::Service::HID::UpdateNpadNow());
        in_forced_npad_update = false;
    }
    RecordPresentReply(present);
    if (GetPresentWatch().active.load(std::memory_order_relaxed)) {
        WatchPresent(present);
    }
    if (const auto trace = reply_trace.load(std::memory_order_acquire)) {
        trace(present);
    }
}

/// Ends any recording without a result, so neither the present path nor the npad updater reads
/// the controllers for it any more.
void StopRecording() {
    Recorder& recorder = GetRecorder();
    KitLock lock{recorder.mutex, "recorder mutex", __func__};
    ClearRecordHooks(recorder);
    recording_active.store(false, std::memory_order_relaxed);
    recorder.recording.reset();
    recorder.at_count.reset();
    recorder.open.reset();
    recorder.sources = {};
    recorder.pre_roll.reset();
    recorder.anchor.reset();
    recorder.cut.reset();
}

/// The pre-roll ring for status and record_anchor replies. lost_presents: the presents after the
/// anchor that the ring does not hold (it was armed late, or the lead is longer than the ring).
json PreRollStatus(const Recorder& recorder) {
    if (!recorder.pre_roll) {
        return {{"armed", false}};
    }
    const u64 first = recorder.pre_roll->FirstPresent();
    return {{"armed", true},
            {"anchor_present", recorder.anchor ? json(*recorder.anchor) : json(nullptr)},
            {"first_present", first},
            {"presents", recorder.pre_roll->NextPresent() - first},
            {"lost_presents", recorder.anchor ? json(first - (*recorder.anchor + 1)) : json(nullptr)},
            {"max_presents", MAX_PRE_ROLL_PRESENTS},
            {"max_steps", MAX_PRE_ROLL_STEPS}};
}

/// The stop reply's pre_roll: the steps from first_present to the recording's start, and how
/// many of those presents had input.
json PreRollJson(const Recorder::PreRollCut& cut) {
    json steps = json::array();
    u64 presents = 0;
    u64 input_presents = 0;
    for (const InputStep& step : cut.steps) {
        steps.push_back(InputStepJson(step));
        presents += step.frames;
        input_presents += IsNeutral(step.input) ? 0 : step.frames;
    }
    return {{"anchor_present", cut.anchor ? json(*cut.anchor) : json(nullptr)},
            {"first_present", cut.first_present},
            {"presents", presents},
            {"lost_presents", cut.anchor ? json(cut.first_present - (*cut.anchor + 1)) : json(nullptr)},
            {"input_presents", input_presents},
            {"steps", std::move(steps)}};
}

/// {} (re)starts the pre-roll ring at the next present, with no anchor yet; {present: N} marks
/// the anchor (the ring then keeps presents after N; it starts first when it is off); {off: true}
/// stops it. Refused while a recording runs, whose pre-roll is the ring up to its start.
json RecordAnchor(ToolState& state, const json& params) {
    for (const auto& [key, value] : params.items()) {
        if (key != "present" && key != "off") {
            throw std::runtime_error{fmt::format("unknown key \"{}\": want present or off", key)};
        }
    }
    RequireKitHooks("record_anchor");
    const bool off = params.value("off", false);
    std::optional<u64> anchor;
    if (params.contains("present")) {
        const json& value = params.at("present");
        if (!value.is_number_integer() || value.get<s64>() < 0) {
            throw std::runtime_error{"present must be a present count of 0 or more"};
        }
        anchor = value.get<u64>();
    }
    if (off && anchor) {
        throw std::runtime_error{"give present or off, not both"};
    }
    const auto sources = RecordedControllers(state);
    Recorder& recorder = GetRecorder();
    KitLock lock{recorder.mutex, "recorder mutex", __func__};
    if (recorder.recording) {
        throw std::runtime_error{fmt::format("a recording runs since present {}; stop it first",
                                             recorder.recording->StartPresent())};
    }
    if (off) {
        if (recorder.pre_roll) {
            ClearRecordHooks(recorder);
            recording_active.store(false, std::memory_order_relaxed);
            recorder.pre_roll.reset();
            recorder.anchor.reset();
            recorder.open.reset();
            recorder.sources = {};
        }
        return PreRollStatus(recorder);
    }
    const u64 now = g_nx2_present_count.load(std::memory_order_relaxed);
    if (!anchor || !recorder.pre_roll) {
        recorder.pre_roll.emplace(now + 1, MAX_PRE_ROLL_PRESENTS, MAX_PRE_ROLL_STEPS);
        recorder.anchor.reset();
        if (!recorder.uses_reply_hook) {
            recorder.sources = sources;
            recorder.any = true;
            recorder.npad_sync = true;
            recorder.open.reset();
            InstallRecordHooks(recorder);
            recording_active.store(true, std::memory_order_relaxed);
            GetWatchdog().Ensure();
        }
    }
    if (anchor) {
        recorder.anchor = anchor;
        recorder.pre_roll->DropBefore(*anchor + 1);
    }
    return PreRollStatus(recorder);
}

/// {action: "start" | "status" | "stop", mode?: "any" | "sample"}. Records the input the game
/// is given at each present from the next one on; stop returns it as input_sequence steps.
json InputRecord(ToolState& state, const json& params) {
    for (const auto& [key, value] : params.items()) {
        if (key != "action" && key != "mode" && key != "npad_sync") {
            throw std::runtime_error{
                fmt::format("unknown key \"{}\": want action, mode or npad_sync", key)};
        }
    }
    const json action_value = params.value("action", json{});
    const std::string action = action_value.is_string() ? action_value.get<std::string>() : "";
    Recorder& recorder = GetRecorder();
    if (action == "start") {
        RequireKitHooks("input_record");
        const std::string mode = params.value("mode", std::string{"any"});
        if (mode != "any" && mode != "sample") {
            throw std::runtime_error{"mode must be any or sample"};
        }
        if (params.contains("npad_sync") && !params.at("npad_sync").is_boolean()) {
            throw std::runtime_error{"npad_sync must be true or false"};
        }
        const bool npad_sync = params.value("npad_sync", true);
        const auto picked = InputController(state);
        KitLock lock{recorder.mutex, "recorder mutex", __func__};
        if (recorder.recording) {
            throw std::runtime_error{fmt::format(
                "already recording since present {}; stop it first",
                recorder.recording->StartPresent())};
        }
        if (recorder.pre_roll && mode != "any") {
            throw std::runtime_error{
                "the pre-roll ring (record_anchor) records in mode any; turn it off first"};
        }
        recorder.sources = RecordedControllers(state);
        recorder.controller_name = picked.second;
        recorder.any = mode == "any";
        recorder.npad_sync = recorder.any && npad_sync;
        // The ring's open count goes on: it is the last present of the pre-roll.
        if (!recorder.pre_roll) {
            recorder.open.reset();
        }
        recorder.blocked_sum_us = recorder.blocked_max_us = recorder.blocked_presents = 0;
        // The hooks go in first. A present counted meanwhile waits for the lock, then is
        // recorded or ignored by the start present read below, so none is lost.
        InstallRecordHooks(recorder);
        recorder.cut.reset();
        recorder.recording.emplace(g_nx2_present_count.load(std::memory_order_relaxed) + 1,
                                   MAX_INPUT_FRAMES, MAX_INPUT_STEPS);
        recording_active.store(true, std::memory_order_relaxed);
        GetWatchdog().Ensure();
        if (recorder.any) {
            recorder.at_count.emplace(recorder.recording->StartPresent(), MAX_INPUT_FRAMES,
                                      MAX_INPUT_STEPS);
        } else {
            recorder.at_count.reset();
        }
        return {{"recording", true},
                {"start_present", recorder.recording->StartPresent()},
                {"controller", picked.second},
                {"mode", mode},
                {"npad_sync", recorder.npad_sync}};
    }
    if (action == "status") {
        json controllers = ControllerStates(RecordedControllers(state));
        KitLock lock{recorder.mutex, "recorder mutex", __func__};
        json result;
        if (!recorder.recording) {
            result = {{"recording", false}, {"controllers", std::move(controllers)}};
        } else {
            const bool open_in_recording =
                recorder.open && recorder.open->present >= recorder.recording->StartPresent();
            result = {{"recording", true},
                      {"start_present", recorder.recording->StartPresent()},
                      {"presents", recorder.recording->Presents() + (open_in_recording ? 1 : 0)},
                      {"steps", recorder.recording->Steps().size()},
                      {"controller", recorder.controller_name},
                      {"mode", recorder.any ? "any" : "sample"},
                      {"truncated", recorder.recording->Truncated() ||
                                        (recorder.at_count && recorder.at_count->Truncated())},
                      {"controllers", std::move(controllers)}};
        }
        if (recorder.pre_roll) {
            result["pre_roll"] = PreRollStatus(recorder);
        }
        return result;
    }
    if (action == "stop") {
        std::optional<InputRecording> recording;
        std::optional<InputRecording> at_count;
        std::string name;
        bool any{};
        bool npad_sync{};
        u64 blocked_sum_us{}, blocked_max_us{}, blocked_presents{};
        std::optional<Recorder::PreRollCut> cut;
        {
            KitLock lock{recorder.mutex, "recorder mutex", __func__};
            if (!recorder.recording) {
                throw std::runtime_error{"not recording"};
            }
            if (!recorder.pre_roll) {
                ClearRecordHooks(recorder);
                recording_active.store(false, std::memory_order_relaxed);
            }
            // The count running now ends here: its step holds what the game got so far.
            static_cast<void>(CloseCount(recorder));
            CutPreRoll(recorder, true);
            cut = std::move(recorder.cut);
            recorder.cut.reset();
            any = recorder.any;
            npad_sync = recorder.npad_sync;
            recording = std::move(recorder.recording);
            recorder.recording.reset();
            at_count = std::move(recorder.at_count);
            recorder.at_count.reset();
            blocked_sum_us = recorder.blocked_sum_us;
            blocked_max_us = recorder.blocked_max_us;
            blocked_presents = recorder.blocked_presents;
            if (recorder.pre_roll) {
                // The ring goes on as before the recording.
                recorder.npad_sync = true;
            } else {
                recorder.sources = {};
            }
            name = std::move(recorder.controller_name);
        }
        const auto steps_json = [](const InputRecording& from) {
            json steps = json::array();
            for (const InputStep& step : from.Steps()) {
                steps.push_back(InputStepJson(step));
            }
            return steps;
        };
        json result = {{"steps", steps_json(*recording)},
                       {"presents", recording->Presents()},
                       {"start_present", recording->StartPresent()},
                       {"end_present", recording->StartPresent() + recording->Presents() - 1},
                       {"controller", std::move(name)},
                       {"mode", any ? "any" : "sample"},
                       {"npad_sync", npad_sync},
                       {"truncated", recording->Truncated() || (at_count && at_count->Truncated())}};
        if (at_count) {
            result["steps_at_count"] = steps_json(*at_count);
            result["blocked_us"] = {
                {"presents", blocked_presents},
                {"mean", blocked_presents ? blocked_sum_us / blocked_presents : 0},
                {"max", blocked_max_us}};
        }
        if (cut) {
            result["pre_roll"] = PreRollJson(*cut);
        }
        return result;
    }
    throw std::runtime_error{"action must be start, status or stop"};
}

/// {presents?, cancel?, wait_s?}. presents arms the next composites for a (present, mean luma)
/// sample each, through the screenshot path: a GPU-thread wait per composite while armed, so only
/// outside measured input. Without presents it reads the samples, waiting up to wait_s for the
/// watch to finish; cancel disarms first.
json LumaWatch(ToolState& state, const json& params, const ClientGone& client_gone) {
    for (const auto& [key, value] : params.items()) {
        if (key != "presents" && key != "cancel" && key != "wait_s") {
            throw std::runtime_error{
                fmt::format("unknown key \"{}\": want presents, cancel or wait_s", key)};
        }
    }
    RequireKitHooks("luma_watch");
    if (params.contains("presents")) {
        const json& presents = params.at("presents");
        if (!presents.is_number_integer() || presents.get<s64>() < 1 ||
            presents.get<u64>() > MAX_LUMA_WATCH) {
            throw std::runtime_error{fmt::format("presents must be 1 to {}", MAX_LUMA_WATCH)};
        }
        VideoCore::Nx2Debug::ArmLumaWatch(presents.get<u32>());
        GetWatchdog().Ensure();
        return {{"armed", presents.get<u32>()},
                {"present", g_nx2_present_count.load(std::memory_order_relaxed)}};
    }
    if (params.value("cancel", false)) {
        VideoCore::Nx2Debug::ArmLumaWatch(0);
        return {{"armed", 0}};
    }
    const int wait_s = Param<int>(params, "wait_s", 0, 0, 120);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{wait_s};
    while (VideoCore::Nx2Debug::ReadLumaWatch().second != 0 &&
           std::chrono::steady_clock::now() < deadline && !state.stopping.load() && !client_gone()) {
        std::this_thread::sleep_for(5ms);
    }
    auto [samples, remaining] = VideoCore::Nx2Debug::ReadLumaWatch();
    json list = json::array();
    for (const auto& sample : samples) {
        list.push_back(json::array(
            {sample.present, std::round(static_cast<double>(sample.luma) * 100.0) / 100.0}));
    }
    return {{"samples", std::move(list)}, {"remaining", remaining}, {"done", remaining == 0}};
}

/// What this build's debug server supports beyond origin, so a client can tell an older exe,
/// which ignores unknown parameters, apart. Raise kit when the kit's contract changes.
/// The trainer methods (docs/nx2/trainer-kit.md) are listed too; time_trace (time_control
/// {trace}) uses the present hook, so EDEN_NX2_KIT_HOOKS=0 leaves it out. time_control_block is
/// the guest address of Ocarina's time control block, only while it is installed (a boot with
/// EDEN_NX2_OOT_TIME_CONTROL=1), so a tool can read the block before any time_control call.
json Features(ToolState&, const json&) {
    json result;
    if (!Common::KitStall::HooksEnabled()) {
        result = {{"features",
                   {"input_ms", "measure", "pause", "step_frames", "time_control", "switches"}},
                  {"kit", 2},
                  {"kit_hooks", false}};
    } else {
        result = {{"features",
                   {"input_ms", "measure", "input_record", "record_mode", "start_present",
                    "landed_present", "screenshot_present", "luma_watch", "npad_sync", "pause",
                    "step_frames", "time_control", "time_trace", "switches", "record_anchor",
                    "pre_roll", "capture_frames", "scan_memory", "watch"}},
                  {"kit", 2},
                  {"kit_hooks", true}};
    }
    if (const u64 block = FileSys::OotTimeControl::BlockAddress(); block != 0) {
        result["time_control_block"] = fmt::format("{:#x}", block);
    }
    return result;
}

Handlers MakeHandlers(const std::shared_ptr<ToolState>& state) {
    const auto handler = [&state](json (*tool)(ToolState&, const json&)) -> Handler {
        return [state, tool](const json& params, const ClientGone&) { return tool(*state, params); };
    };
    const auto waiting = [&state](json (*tool)(ToolState&, const json&, const ClientGone&)) -> Handler {
        return [state, tool](const json& params, const ClientGone& client_gone) {
            return tool(*state, params, client_gone);
        };
    };
    Handlers handlers{
        {"status", handler(Status)},
        {"features", handler(Features)},
        {"frame_times", handler(FrameTimes)},
        {"screenshot", handler(Screenshot)},
        {"log_tail", handler(LogTail)},
        {"log_filter", handler(LogFilter)},
        {"capture_frame", handler(CaptureFrame)},
        {"capture_bundle", handler(CaptureBundle)},
        {"reject_shaders", handler(RejectShaders)},
        {"sample", handler(SampleThreads)},
        {"trace_record", handler(TraceRecord)},
        {"trace_stop", handler(TraceStop)},
        {"trace_status", handler(TraceStatus)},
        {"speed_limit", handler(SpeedLimit)},
        {"host_timing", handler(HostTiming)},
        {"read_memory", handler(ReadMemory)},
        {"search_memory", handler(SearchMemory)},
        {"scan_memory", handler(ScanMemory)},
        {"read_gpu_memory", handler(ReadGpuMemory)},
        {"input", waiting(Input)},
        {"input_sequence", waiting(InputSequence)},
        {"input_record", handler(InputRecord)},
        {"record_anchor", handler(RecordAnchor)},
        {"luma_watch", waiting(LumaWatch)},
    };
    AddTrainerHandlers(handlers, state->system,
                       [state] { return state->stopping.load(std::memory_order_relaxed); },
                       [state](const json& params) { return StartCapture(*state, params); });
    return handlers;
}

} // Anonymous namespace

json CallTool(System& system, std::string_view method, const json& params) {
    const auto state = std::make_shared<ToolState>(system, "", 0, std::filesystem::path{});
    const Handlers handlers = MakeHandlers(state);
    return handlers.at(std::string{method})(params, [] { return false; });
}

json RecordInput(System& system, std::string_view action) {
    return CallTool(system, "input_record", {{"action", std::string{action}}});
}

/// Debug services alive; the Service constructor and destructor count them.
static std::atomic<u32> running_services{};

bool IsServiceRunning() {
    return running_services.load(std::memory_order_relaxed) != 0;
}

u64 ParseShaderHash(std::string_view text) {
    const std::string_view digits =
        text.starts_with("0x") || text.starts_with("0X") ? text.substr(2) : text;
    u64 value = 0;
    const auto [end, error] =
        std::from_chars(digits.data(), digits.data() + digits.size(), value, 16);
    if (digits.size() != 16 || error != std::errc{} || end != digits.data() + digits.size()) {
        throw std::runtime_error{fmt::format(
            "bad hash \"{}\": want all 16 hex digits of a native program key (shader dump file "
            "name) or shader hash (frame dump \"shaders\"); a shorter one would match nothing",
            text)};
    }
    return value;
}

std::vector<u8> ParseHexBytes(std::string_view text) {
    std::string digits;
    const std::string_view body =
        text.starts_with("0x") || text.starts_with("0X") ? text.substr(2) : text;
    for (const char c : body) {
        if (c != ' ') {
            digits.push_back(c);
        }
    }
    std::vector<u8> bytes(digits.size() / 2);
    for (size_t i = 0; i < bytes.size(); ++i) {
        const auto [end, error] =
            std::from_chars(digits.data() + i * 2, digits.data() + i * 2 + 2, bytes[i], 16);
        if (error != std::errc{} || end != digits.data() + i * 2 + 2) {
            bytes.clear();
            break;
        }
    }
    if (bytes.empty() || digits.size() % 2 != 0) {
        throw std::runtime_error{fmt::format(
            "bad pattern \"{}\": want hex byte pairs in memory order (little endian: 1.0f is "
            "0000803f)",
            text)};
    }
    return bytes;
}

json FormatMemory(std::span<const u8> bytes, std::string_view as) {
    if (as == "hex") {
        std::string hex;
        hex.reserve(bytes.size() * 2);
        for (const u8 byte : bytes) {
            fmt::format_to(std::back_inserter(hex), "{:02x}", byte);
        }
        return {{"hex", std::move(hex)}};
    }
    const size_t element = as == "u32" || as == "f32" ? 4 : as == "u64" ? 8 : 0;
    if (element == 0) {
        throw std::runtime_error{fmt::format("as \"{}\": want hex, u32, u64 or f32", as)};
    }
    if (bytes.size() % element != 0) {
        throw std::runtime_error{fmt::format("size must be a multiple of {} for {}", element, as)};
    }
    json values = json::array();
    for (size_t offset = 0; offset < bytes.size(); offset += element) {
        if (element == 8) {
            u64 value;
            std::memcpy(&value, bytes.data() + offset, sizeof(value));
            values.push_back(HexAddress(value));
            continue;
        }
        u32 value;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        if (as == "u32") {
            values.push_back(value);
            continue;
        }
        const float number = std::bit_cast<float>(value);
        if (std::isnan(number)) {
            values.push_back("nan");
        } else if (std::isinf(number)) {
            values.push_back(number < 0 ? "-inf" : "inf");
        } else {
            values.push_back(number);
        }
    }
    return {{std::string{as}, std::move(values)}};
}

void SetPresentReplyTrace(void (*trace)(u64)) {
    std::scoped_lock lock{reply_hook_mutex};
    reply_trace.store(trace, std::memory_order_release);
    UpdateReplyHook();
}

std::optional<u64> ReadGuestMemory(Memory::Memory& memory, u64 address, std::span<u8> out) {
    for (size_t offset = 0; offset < out.size();) {
        const u64 current = address + offset;
        const size_t length = (std::min)(static_cast<size_t>(Memory::LICHII_PAGESIZE -
                                                             (current & Memory::LICHII_PAGEMASK)),
                                         out.size() - offset);
        const u8* const source = GuestByte(memory, current);
        if (source == nullptr) {
            return current;
        }
        std::memcpy(out.data() + offset, source, length);
        offset += length;
    }
    return std::nullopt;
}

GuestSearch SearchGuestMemory(Memory::Memory& memory, std::span<const u8> pattern, u64 start,
                              u64 end, size_t max_hits,
                              std::chrono::steady_clock::time_point deadline) {
    GuestSearch search{.hits = {}, .next = end, .complete = true, .scanned = 0};
    // The bytes of a run of mapped pages that a match starting there could still need.
    std::vector<u8> window;
    window.reserve(Memory::LICHII_PAGESIZE + pattern.size());
    for (u64 address = start; address < end;) {
        if (std::chrono::steady_clock::now() >= deadline) {
            search.next = address - window.size();
            search.complete = false;
            return search;
        }
        const u64 page_end = (std::min)((address | Memory::LICHII_PAGEMASK) + 1, end);
        const u8* const source = GuestByte(memory, address);
        if (source == nullptr) {
            window.clear();
            address = page_end;
            continue;
        }
        const u64 window_start = address - window.size();
        const size_t length = page_end - address;
        window.insert(window.end(), source, source + length);
        search.scanned += length;
        for (size_t offset = 0; offset + pattern.size() <= window.size(); ++offset) {
            const void* const found = std::memchr(window.data() + offset, pattern[0],
                                                  window.size() - pattern.size() + 1 - offset);
            if (found == nullptr) {
                break;
            }
            offset = static_cast<size_t>(static_cast<const u8*>(found) - window.data());
            if (std::memcmp(window.data() + offset, pattern.data(), pattern.size()) != 0) {
                continue;
            }
            search.hits.push_back(window_start + offset);
            if (search.hits.size() >= max_hits) {
                search.next = window_start + offset + 1;
                search.complete = false;
                return search;
            }
        }
        window.erase(window.begin(),
                     window.end() - static_cast<std::ptrdiff_t>(
                                        (std::min)(pattern.size() - 1, window.size())));
        address = page_end;
    }
    return search;
}

size_t ScanSize(ScanType type) {
    static constexpr std::array<size_t, 6> sizes{1, 2, 4, 4, 4, 8};
    return sizes[static_cast<size_t>(type)];
}

double ScanValue(ScanType type, u64 bits) {
    switch (type) {
    case ScanType::U8:
        return static_cast<double>(static_cast<u8>(bits));
    case ScanType::U16:
        return static_cast<double>(static_cast<u16>(bits));
    case ScanType::U32:
        return static_cast<double>(static_cast<u32>(bits));
    case ScanType::I32:
        return static_cast<double>(static_cast<s32>(static_cast<u32>(bits)));
    case ScanType::F32:
        return static_cast<double>(std::bit_cast<float>(static_cast<u32>(bits)));
    case ScanType::F64:
        return std::bit_cast<double>(bits);
    }
    return 0.0;
}

namespace {

/// Whether a value passes test; kept is the value at the last scan (refines only).
bool PassesScan(ScanType type, ScanTest test, u64 bits, u64 kept) {
    switch (test.op) {
    case ScanOp::Changed:
        return bits != kept;
    case ScanOp::Unchanged:
        return bits == kept;
    default:
        break;
    }
    const double value = ScanValue(type, bits);
    switch (test.op) {
    case ScanOp::Equal:
    case ScanOp::Range:
        return value >= test.low && value <= test.high;
    case ScanOp::Increased:
        return value > ScanValue(type, kept);
    case ScanOp::Decreased:
        return value < ScanValue(type, kept);
    default:
        return false;
    }
}

} // Anonymous namespace

ScanList ScanGuestMemory(Memory::Memory& memory, std::span<const std::pair<u64, u64>> ranges,
                         ScanType type, ScanTest test, size_t max_hits,
                         std::chrono::steady_clock::time_point deadline) {
    ScanList list{.type = type, .addresses = {}, .values = {}};
    const size_t size = ScanSize(type);
    for (const auto& [range_start, range_end] : ranges) {
        // Aligned to the size, so a value never spans two pages.
        for (u64 page = range_start & ~u64{Memory::LICHII_PAGEMASK}; page < range_end;
             page += Memory::LICHII_PAGESIZE) {
            if (std::chrono::steady_clock::now() >= deadline) {
                list.complete = false;
                return list;
            }
            const u8* const source = GuestByte(memory, page);
            if (source == nullptr) {
                continue;
            }
            const u64 first = (std::max)(page, (range_start + size - 1) & ~u64{size - 1});
            const u64 last = (std::min)(page + Memory::LICHII_PAGESIZE, range_end);
            for (u64 address = first; address + size <= last; address += size) {
                u64 bits = 0;
                std::memcpy(&bits, source + (address - page), size);
                if (!PassesScan(type, test, bits, 0)) {
                    continue;
                }
                if (list.addresses.size() == max_hits) {
                    list.truncated = true;
                    return list;
                }
                list.addresses.push_back(address);
                list.values.push_back(bits);
            }
            list.scanned += last - (std::max)(page, range_start);
        }
    }
    return list;
}

void RefineScan(Memory::Memory& memory, ScanList& list, ScanTest test) {
    const size_t size = ScanSize(list.type);
    size_t kept = 0;
    for (size_t i = 0; i < list.addresses.size(); ++i) {
        const u8* const source = GuestByte(memory, list.addresses[i]);
        if (source == nullptr) {
            continue;
        }
        u64 bits = 0;
        std::memcpy(&bits, source, size);
        if (PassesScan(list.type, test, bits, list.values[i])) {
            list.addresses[kept] = list.addresses[i];
            list.values[kept] = bits;
            ++kept;
        }
    }
    list.addresses.resize(kept);
    list.values.resize(kept);
}

ValueWatch::ValueWatch(Memory::Memory& memory_, std::vector<std::pair<u64, ScanType>> addresses_,
                       u64 start_, u64 end_)
    : memory{&memory_}, addresses{std::move(addresses_)}, start{start_}, end{end_} {
    presents.reserve(end - start);
    values.reserve((end - start) * addresses.size());
}

void ValueWatch::Sample(u64 present) {
    if (present < start || present >= end || (!presents.empty() && present <= presents.back())) {
        return;
    }
    presents.push_back(present);
    for (const auto& [address, type] : addresses) {
        const u8* const source = GuestByte(*memory, address);
        u64 bits = 0;
        if (source != nullptr) {
            std::memcpy(&bits, source, ScanSize(type));
        }
        values.push_back(source != nullptr ? std::optional{bits} : std::nullopt);
    }
}

json ValueWatch::Json() const {
    json list = json::array();
    for (const auto& [address, type] : addresses) {
        list.push_back({{"address", HexAddress(address)}, {"as", ScanTypeName(type)}});
    }
    json rows = json::array();
    for (size_t row = 0; row < presents.size(); ++row) {
        json line = json::array({presents[row]});
        for (size_t column = 0; column < addresses.size(); ++column) {
            const auto& value = values[row * addresses.size() + column];
            line.push_back(value ? ScanJson(addresses[column].second, *value) : json(nullptr));
        }
        rows.push_back(std::move(line));
    }
    return {{"addresses", std::move(list)}, {"read_at", "reply"}, {"presents", end - start},
            {"sampled", presents.size()}, {"rows", std::move(rows)}};
}

InputStep ParseInputStep(const json& step) {
    if (!step.is_object()) {
        throw std::runtime_error{"a step is an object"};
    }
    for (const auto& [key, value] : step.items()) {
        if (key != "buttons" && key != "left_stick" && key != "right_stick" && key != "frames" &&
            key != "ms") {
            throw std::runtime_error{fmt::format(
                "unknown key \"{}\": want buttons, left_stick, right_stick, frames or ms", key)};
        }
    }
    InputStep result{.input = {}, .frames = 0};
    if (step.contains("frames") == step.contains("ms")) {
        throw std::runtime_error{"a step needs exactly one of frames and ms"};
    }
    if (step.contains("ms")) {
        const json& ms = step.at("ms");
        if (!ms.is_number_integer()) {
            throw std::runtime_error{fmt::format("ms must be a whole number, 1 to {} (got {})", MAX_INPUT_MS, ms.dump())};
        }
        if (ms.get<s64>() < 1 || ms.get<u64>() > MAX_INPUT_MS) {
            throw std::runtime_error{fmt::format("ms must be 1 to {}", MAX_INPUT_MS)};
        }
        result.ms = ms.get<u64>();
    } else {
        const json& frames = step.at("frames");
        if (!frames.is_number_integer()) {
            throw std::runtime_error{fmt::format("frames must be a whole number, 1 to {} (got {})",
                                                 MAX_INPUT_FRAMES, frames.dump())};
        }
        if (frames.get<s64>() < 1 || frames.get<u64>() > MAX_INPUT_FRAMES) {
            throw std::runtime_error{fmt::format("frames must be 1 to {}", MAX_INPUT_FRAMES)};
        }
        result.frames = frames.get<u64>();
    }
    for (const json& button : step.value("buttons", json::array())) {
        const std::string text = button.is_string() ? button.get<std::string>() : button.dump();
        const auto it = std::ranges::find(INPUT_BUTTONS, std::string_view{text},
                                          &std::pair<std::string_view, HID::NpadButton>::first);
        if (it == INPUT_BUTTONS.end()) {
            std::string names;
            for (const auto& [known, _] : INPUT_BUTTONS) {
                names += names.empty() ? "" : ", ";
                names += known;
            }
            throw std::runtime_error{fmt::format("unknown button {}: want one of {}", text, names)};
        }
        result.input.buttons |= it->second;
    }
    const auto stick = [&step](const char* key) -> std::optional<HID::AnalogStickState> {
        if (!step.contains(key)) {
            return std::nullopt;
        }
        const json& value = step.at(key);
        const auto axis = [&](size_t index) {
            if (!value.is_array() || value.size() != 2 || !value[index].is_number() ||
                std::abs(value[index].get<double>()) > 1.0) {
                throw std::runtime_error{fmt::format("{} must be [x, y] with both in -1..1", key)};
            }
            return static_cast<s32>(std::lround(value[index].get<double>() * HID::HID_JOYSTICK_MAX));
        };
        return HID::AnalogStickState{.x = axis(0), .y = axis(1)};
    };
    result.input.left = stick("left_stick");
    result.input.right = stick("right_stick");
    return result;
}

std::optional<HID::InjectedInput> InputAt(std::span<const InputStep> steps, u64 start, u64 now) {
    if (now < start) {
        return std::nullopt;
    }
    u64 end = start;
    for (const InputStep& step : steps) {
        end += step.frames;
        if (now < end) {
            return step.input;
        }
    }
    return std::nullopt;
}

json InputStepJson(const InputStep& step) {
    json result{{"frames", step.frames}};
    json buttons = json::array();
    for (const auto& [name, button] : INPUT_BUTTONS) {
        if ((step.input.buttons & button) != HID::NpadButton::None) {
            buttons.push_back(name);
        }
    }
    if (!buttons.empty()) {
        result["buttons"] = std::move(buttons);
    }
    // Five decimals keep the JSON short and are exact: 0.5e-5 * HID_JOYSTICK_MAX is 0.16, so
    // ParseInputStep rounds back to the same value.
    const auto axis = [](s32 value) {
        const double scaled = static_cast<double>((std::clamp)(value, -HID::HID_JOYSTICK_MAX,
                                                               HID::HID_JOYSTICK_MAX)) /
                              HID::HID_JOYSTICK_MAX;
        return std::round(scaled * 100'000.0) / 100'000.0;
    };
    const auto stick = [&](const char* key, const std::optional<HID::AnalogStickState>& value) {
        if (value) {
            result[key] = json::array({axis(value->x), axis(value->y)});
        }
    };
    stick("left_stick", step.input.left);
    stick("right_stick", step.input.right);
    return result;
}

InputRecording::InputRecording(u64 start_present_, u64 max_presents_, size_t max_steps_)
    : start_present{start_present_}, next_present{start_present_}, max_presents{max_presents_},
      max_steps{max_steps_} {}

bool InputRecording::Add(u64 present, const HID::InjectedInput& input) {
    if (truncated || present < next_present) {
        return !truncated;
    }
    const auto same_stick = [](const std::optional<HID::AnalogStickState>& a,
                               const std::optional<HID::AnalogStickState>& b) {
        return a.has_value() == b.has_value() && (!a || (a->x == b->x && a->y == b->y));
    };
    const u64 wanted = present - next_present + 1;
    const u64 frames = (std::min)(wanted, max_presents - Presents());
    const bool same = !steps.empty() && steps.back().input.buttons == input.buttons &&
                      same_stick(steps.back().input.left, input.left) &&
                      same_stick(steps.back().input.right, input.right);
    if (frames != 0 && same) {
        steps.back().frames += frames;
    } else if (frames != 0 && steps.size() < max_steps) {
        steps.push_back({.input = input, .frames = frames});
    } else {
        truncated = true;
        return false;
    }
    next_present += frames;
    truncated = frames < wanted;
    return !truncated;
}

namespace {

bool SameInput(const HID::InjectedInput& a, const HID::InjectedInput& b) {
    const auto same_stick = [](const std::optional<HID::AnalogStickState>& x,
                               const std::optional<HID::AnalogStickState>& y) {
        return x.has_value() == y.has_value() && (!x || (x->x == y->x && x->y == y->y));
    };
    return a.buttons == b.buttons && same_stick(a.left, b.left) && same_stick(a.right, b.right);
}

} // Anonymous namespace

bool IsNeutral(const HID::InjectedInput& input) {
    const auto centred = [](const std::optional<HID::AnalogStickState>& stick) {
        return !stick || (stick->x == 0 && stick->y == 0);
    };
    return input.buttons == HID::NpadButton::None && centred(input.left) && centred(input.right);
}

PreRollRing::PreRollRing(u64 first_present_, u64 max_presents_, size_t max_steps_)
    : first_present{first_present_}, next_present{first_present_}, max_presents{max_presents_},
      max_steps{max_steps_} {}

void PreRollRing::Add(u64 present, const HID::InjectedInput& input) {
    if (present < next_present) {
        return;
    }
    const u64 frames = present - next_present + 1;
    if (!steps.empty() && SameInput(steps.back().input, input)) {
        steps.back().frames += frames;
    } else {
        steps.push_back({.input = input, .frames = frames});
    }
    next_present += frames;
    if (next_present - first_present > max_presents) {
        DropBefore(next_present - max_presents);
    }
    while (steps.size() > max_steps) {
        first_present += steps.front().frames;
        steps.pop_front();
    }
}

void PreRollRing::DropBefore(u64 present) {
    while (!steps.empty() && first_present < present) {
        InputStep& front = steps.front();
        if (first_present + front.frames <= present) {
            first_present += front.frames;
            steps.pop_front();
        } else {
            front.frames -= present - first_present;
            first_present = present;
        }
    }
    if (steps.empty()) {
        first_present = next_present = (std::max)(next_present, present);
    }
}

std::vector<InputStep> PreRollRing::StepsBefore(u64 end) const {
    std::vector<InputStep> result;
    u64 at = first_present;
    for (const InputStep& step : steps) {
        if (at >= end) {
            break;
        }
        result.push_back(step);
        result.back().frames = (std::min)(step.frames, end - at);
        at += step.frames;
    }
    return result;
}

std::optional<HID::InjectedInput> InputAtTime(std::span<const InputStep> steps, u64 elapsed_us) {
    u64 end = 0;
    for (const InputStep& step : steps) {
        end += step.ms * 1000;
        if (elapsed_us < end) {
            return step.input;
        }
    }
    return std::nullopt;
}

Service::Service(System& system, std::string title, u64 program_id, const Endpoint& endpoint,
                 std::string dump_directory)
    : state{std::make_shared<ToolState>(system, std::move(title), program_id,
                                        std::move(dump_directory))} {
    Common::Log::SetRingCapacity(RING_LINES);
    server = std::make_unique<Server>(endpoint, MakeHandlers(state));
    running_services.fetch_add(1, std::memory_order_relaxed);
    LOG_INFO(Debug, "Debug server listening on {}:{}, dump dir {}", endpoint.host, server->Port(),
             state->dump_directory.generic_string());
    const json features = Features(*state, json::object());
    LOG_INFO(Debug, "Debug server kit {}, kit hooks {}, features {}", features["kit"].dump(),
             features["kit_hooks"].get<bool>() ? "on" : "off (EDEN_NX2_KIT_HOOKS=0)",
             features["features"].dump());
}

Service::~Service() {
    state->stopping.store(true, std::memory_order_relaxed);
    running_services.fetch_sub(1, std::memory_order_relaxed);
    // After the server, whose one thread runs every handler, so no input_record start follows.
    server.reset();
    StopRecording();
    StopStepTrace();
    {
        std::scoped_lock lock{scan_mutex};
        GetScanList().reset();
    }
    GetWatchdog().Stop();
}

std::unique_ptr<Service> MaybeStartService(System& system, std::string title, u64 program_id) {
    const char* const text = std::getenv("EDEN_NX2_DEBUG_SERVER");
    if (text == nullptr || *text == '\0') {
        return nullptr;
    }
    std::string error;
    const auto endpoint = ParseEndpoint(text, error);
    if (!endpoint) {
        LOG_ERROR(Debug, "EDEN_NX2_DEBUG_SERVER={}: {}; the debug server is off", text, error);
        return nullptr;
    }
    const char* const directory = std::getenv("EDEN_NX2_DEBUG_DIR");
    std::string dump_directory = directory != nullptr && *directory != '\0'
        ? std::string{directory}
        : (Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) / "debug-server").generic_string();
    try {
        return std::make_unique<Service>(system, std::move(title), program_id, *endpoint,
                                         std::move(dump_directory));
    } catch (const std::exception& failure) {
        LOG_ERROR(Debug, "Debug server cannot listen on {}:{}: {}", endpoint->host, endpoint->port,
                  failure.what());
        return nullptr;
    }
}

} // namespace Core::Nx2Debug
