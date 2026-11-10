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

#include "common/fs/path_util.h"
#include "common/scope_exit.h"
#include "common/logging.h"
#include "common/nx2_sampler.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "common/stb.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/debugger/nx2_bug_bundle.h"
#include "core/debugger/nx2_debug_server.h"
#include "core/debugger/nx2_debug_tools.h"
#include "core/frontend/framebuffer_layout.h"
#include "core/hle/kernel/k_process.h"
#include "core/memory.h"
#include "hid_core/hid_core.h"
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
    std::vector<u8> rgb(size_t{layout.width} * layout.height * 3);
    for (u32 y = 0; y < layout.height; ++y) {
        const u8* source = pixels->data() + size_t{invert_y ? layout.height - 1 - y : y} * layout.width * 4;
        u8* target = rgb.data() + size_t{y} * layout.width * 3;
        for (u32 x = 0; x < layout.width; ++x, source += 4, target += 3) {
            target[0] = source[2];
            target[1] = source[1];
            target[2] = source[0];
        }
    }
    Png png{.bytes = {}, .width = layout.width, .height = layout.height};
    if (!stbi_write_png_to_func(AppendPng, &png.bytes, static_cast<int>(layout.width),
                                static_cast<int>(layout.height), 3, rgb.data(),
                                static_cast<int>(layout.width * 3))) {
        throw std::runtime_error{"PNG encoding failed"};
    }
    png.present = present;
    return png;
}

namespace {

json Screenshot(ToolState& state, const json& params) {
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

json CaptureFrame(ToolState& state, const json& params) {
    if (!IsVulkan()) {
        throw std::runtime_error{"frame capture needs the Vulkan renderer"};
    }
    const u64 id = ++state.captures;
    const auto directory = state.dump_directory / fmt::format("capture-{}-{:03}", state.session, id);
    std::filesystem::create_directories(directory);
    const bool sampled = params.value("sampled", false);
    const bool buffers = params.value("buffers", false);
    const int timeout = Param<int>(params, "timeout_s", 120, 1, 600);
    VideoCore::Nx2Debug::RequestCapture(
        {.id = id, .directory = directory, .sampled = sampled, .buffers = buffers});
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
    const size_t keep = Param<size_t>(params, "manifest_lines", 200, 0, 5000);
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

/// Holds the steps on player 1 (or the handheld controller when player 1 is not connected) from
/// the next present on, and returns once they are done. The injector stops by itself after the
/// last step; leaving this function by any path removes it.
json RunInput(ToolState& state, const std::vector<InputStep>& steps, const json& params,
              const ClientGone& client_gone) {
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
    if (measure && !timed && total > MAX_MEASURED_PRESENTS) {
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
    u64 now = start - 1;
    for (;;) {
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
        std::this_thread::sleep_for(timed ? 1ms : 2ms);
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
    if (timed) {
        result["ms"] = total;
        result["clock"] = "host_time";
    } else if (measure || start_present) {
        result["landed_present"] = landed;
    }
    if (measure) {
        if (timed && (presents < 2 || presents > MAX_MEASURED_PRESENTS)) {
            json empty = IntervalStats({}, 60.0, true, true);
            empty.update({{"supported", "held-input-v1"}, {"first_present", start},
                          {"last_present", start + presents - 1}, {"expected_intervals", 0},
                          {"complete", false}, {"covered_presents", 0}});
            result["measurement"] = std::move(empty);
        } else {
            result["measurement"] = ReadInputFrameTimes(start, presents, ns(begin),
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
    return RunInput(state, {ParseInputStep(step)}, params, client_gone);
}

json InputSequence(ToolState& state, const json& params, const ClientGone& client_gone) {
    const json& list = params.at("steps");
    if (!list.is_array() || list.empty() || list.size() > MAX_INPUT_STEPS) {
        throw std::runtime_error{fmt::format("steps must be an array of 1 to {}", MAX_INPUT_STEPS)};
    }
    std::vector<InputStep> steps;
    for (size_t index = 0; index < list.size(); ++index) {
        try {
            steps.push_back(ParseInputStep(list[index]));
        } catch (const std::exception& error) {
            throw std::runtime_error{fmt::format("step {}: {}", index, error.what())};
        }
    }
    return RunInput(state, steps, params, client_gone);
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
    /// Mode "any": the count still running, closed into the recording at the next present.
    struct OpenCount {
        u64 present;
        HID::InjectedInput input;
    };
    std::optional<OpenCount> open;
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
void AddGameInput(HID::InjectedInput& input, const HID::EmulatedController& controller) {
    const HID::NpadButton held = controller.GetNpadButtons().raw;
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
    const HID::AnalogSticks sticks = controller.GetSticks();
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

void ClearRecordHooks() {
    g_nx2_present_hook.store(nullptr, std::memory_order_release);
    HID::g_npad_read_hook.store(nullptr, std::memory_order_release);
}

/// Mode "any": moves the open count into the recording. False once the recording is truncated.
bool CloseCount(Recorder& recorder) {
    if (!recorder.open) {
        return true;
    }
    const Recorder::OpenCount open = *recorder.open;
    recorder.open.reset();
    return recorder.recording->Add(open.present, open.input);
}

/// g_nx2_present_hook while input_record records. Runs right after the present count becomes
/// present, so it reads the input the injector and the owner's pad give at the start of that
/// count. Mode "any" then keeps the count open for the npad reads during it.
void RecordPresent(u64 present) {
    Recorder& recorder = GetRecorder();
    std::scoped_lock lock{recorder.mutex};
    if (!recorder.recording || recorder.recording->Truncated() ||
        present < recorder.recording->StartPresent() ||
        (recorder.open && present <= recorder.open->present)) {
        return;
    }
    const HID::InjectedInput input = GameInput(recorder.sources);
    const bool kept =
        recorder.any ? CloseCount(recorder) : recorder.recording->Add(present, input);
    if (!kept) {
        ClearRecordHooks();
        return;
    }
    if (recorder.any) {
        recorder.open = Recorder::OpenCount{.present = present, .input = input};
    }
}

/// HID::g_npad_read_hook in mode "any": ORs the buttons the npad updater reads for the game into
/// the open count. A read that may belong to another count (the count moved around it) is left
/// out, so scripted input of the next count never leaks into this one.
void RecordNpadRead(const HID::EmulatedController& controller) {
    Recorder& recorder = GetRecorder();
    std::scoped_lock lock{recorder.mutex};
    if (!recorder.open || std::ranges::find(recorder.sources, &controller) == recorder.sources.end()) {
        return;
    }
    const u64 before = g_nx2_present_count.load(std::memory_order_acquire);
    if (before != recorder.open->present) {
        return;
    }
    HID::InjectedInput input{};
    AddGameInput(input, controller);
    if (g_nx2_present_count.load(std::memory_order_acquire) == before) {
        recorder.open->input.buttons |= input.buttons;
    }
}

/// Ends any recording without a result, so neither the present path nor the npad updater reads
/// the controllers for it any more.
void StopRecording() {
    Recorder& recorder = GetRecorder();
    std::scoped_lock lock{recorder.mutex};
    ClearRecordHooks();
    recorder.recording.reset();
    recorder.open.reset();
    recorder.sources = {};
}

/// {action: "start" | "status" | "stop", mode?: "any" | "sample"}. Records the input the game
/// is given at each present from the next one on; stop returns it as input_sequence steps.
json InputRecord(ToolState& state, const json& params) {
    for (const auto& [key, value] : params.items()) {
        if (key != "action" && key != "mode") {
            throw std::runtime_error{fmt::format("unknown key \"{}\": want action or mode", key)};
        }
    }
    const json action_value = params.value("action", json{});
    const std::string action = action_value.is_string() ? action_value.get<std::string>() : "";
    Recorder& recorder = GetRecorder();
    if (action == "start") {
        const std::string mode = params.value("mode", std::string{"any"});
        if (mode != "any" && mode != "sample") {
            throw std::runtime_error{"mode must be any or sample"};
        }
        const auto picked = InputController(state);
        std::scoped_lock lock{recorder.mutex};
        if (recorder.recording) {
            throw std::runtime_error{fmt::format(
                "already recording since present {}; stop it first",
                recorder.recording->StartPresent())};
        }
        recorder.sources = RecordedControllers(state);
        recorder.controller_name = picked.second;
        recorder.any = mode == "any";
        recorder.open.reset();
        // The hooks go in first. A present counted meanwhile waits for the lock, then is
        // recorded or ignored by the start present read below, so none is lost.
        g_nx2_present_hook.store(RecordPresent, std::memory_order_release);
        if (recorder.any) {
            HID::g_npad_read_hook.store(RecordNpadRead, std::memory_order_release);
        }
        recorder.recording.emplace(g_nx2_present_count.load(std::memory_order_relaxed) + 1,
                                   MAX_INPUT_FRAMES, MAX_INPUT_STEPS);
        return {{"recording", true},
                {"start_present", recorder.recording->StartPresent()},
                {"controller", picked.second},
                {"mode", mode}};
    }
    if (action == "status") {
        json controllers = ControllerStates(RecordedControllers(state));
        std::scoped_lock lock{recorder.mutex};
        if (!recorder.recording) {
            return {{"recording", false}, {"controllers", std::move(controllers)}};
        }
        return {{"recording", true},
                {"start_present", recorder.recording->StartPresent()},
                {"presents", recorder.recording->Presents() + (recorder.open ? 1 : 0)},
                {"steps", recorder.recording->Steps().size()},
                {"controller", recorder.controller_name},
                {"mode", recorder.any ? "any" : "sample"},
                {"truncated", recorder.recording->Truncated()},
                {"controllers", std::move(controllers)}};
    }
    if (action == "stop") {
        std::optional<InputRecording> recording;
        std::string name;
        bool any{};
        {
            std::scoped_lock lock{recorder.mutex};
            if (!recorder.recording) {
                throw std::runtime_error{"not recording"};
            }
            ClearRecordHooks();
            // The count running now ends here: its step holds what the game got so far.
            static_cast<void>(CloseCount(recorder));
            any = recorder.any;
            recording = std::move(recorder.recording);
            recorder.recording.reset();
            recorder.sources = {};
            name = std::move(recorder.controller_name);
        }
        json steps = json::array();
        for (const InputStep& step : recording->Steps()) {
            steps.push_back(InputStepJson(step));
        }
        return {{"steps", std::move(steps)},
                {"presents", recording->Presents()},
                {"start_present", recording->StartPresent()},
                {"end_present", recording->StartPresent() + recording->Presents() - 1},
                {"controller", std::move(name)},
                {"mode", any ? "any" : "sample"},
                {"truncated", recording->Truncated()}};
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
    if (params.contains("presents")) {
        const json& presents = params.at("presents");
        if (!presents.is_number_integer() || presents.get<s64>() < 1 ||
            presents.get<u64>() > MAX_LUMA_WATCH) {
            throw std::runtime_error{fmt::format("presents must be 1 to {}", MAX_LUMA_WATCH)};
        }
        VideoCore::Nx2Debug::ArmLumaWatch(presents.get<u32>());
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
json Features(ToolState&, const json&) {
    return {{"features",
             {"input_ms", "measure", "input_record", "record_mode", "start_present",
              "landed_present", "screenshot_present", "luma_watch"}},
            {"kit", 1}};
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
    return {
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
        {"read_gpu_memory", handler(ReadGpuMemory)},
        {"input", waiting(Input)},
        {"input_sequence", waiting(InputSequence)},
        {"input_record", handler(InputRecord)},
        {"luma_watch", waiting(LumaWatch)},
    };
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
        if (!ms.is_number_integer() || ms.get<s64>() < 1 || ms.get<u64>() > MAX_INPUT_MS) {
            throw std::runtime_error{fmt::format("ms must be 1 to {}", MAX_INPUT_MS)};
        }
        result.ms = ms.get<u64>();
    } else {
        const json& frames = step.at("frames");
        if (!frames.is_number_integer() || frames.get<s64>() < 1 ||
            frames.get<u64>() > MAX_INPUT_FRAMES) {
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
}

Service::~Service() {
    state->stopping.store(true, std::memory_order_relaxed);
    running_services.fetch_sub(1, std::memory_order_relaxed);
    // After the server, whose one thread runs every handler, so no input_record start follows.
    server.reset();
    StopRecording();
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
