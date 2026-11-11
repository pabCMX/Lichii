// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <thread>

#include <boost/asio.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "common/nx2_kit_stall.h"
#include "common/page_table.h"
#include "common/scope_exit.h"
#include "core/core.h"
#include "core/debugger/nx2_debug_server.h"
#include "core/debugger/nx2_debug_tools.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/memory.h"
#include "hid_core/hid_core.h"
#include "hid_core/resource_manager.h"
#include "tests/core/system_test_context.h"
#include "video_core/nx2_debug_control.h"

namespace {

using namespace std::chrono_literals;
using Core::Nx2Debug::ClientGone;
using Core::Nx2Debug::Handlers;
using nlohmann::json;

Handlers EchoHandlers() {
    return {
        {"echo", [](const json& params, const ClientGone&) { return params; }},
        {"fail", [](const json&, const ClientGone&) -> json { throw std::runtime_error{"broken"}; }},
    };
}

void MapPage(Common::PageTable& table, u64 page, u8* host, Common::PageType type) {
    auto& entry = table.entries.GetAndFault(page);
    entry.Store(false, Common::PageType::Memory, 1, reinterpret_cast<uintptr_t>(host) - (page << 12));
    if (type == Common::PageType::RasterizerCachedMemory) {
        entry.MarkRasterizerCached();
    }
}

/// Guest pages 4, 5, 7 (rasterizer cached) and 8 over host pages 1, 0, 2 and 3 of a zeroed
/// backing; page 6 and everything else are unmapped.
struct GuestPages {
    GuestPages() {
        system.Initialize();
        auto& table = process.GetPageTable().GetImpl();
        table.Resize(20, 12);
        backing = system.DeviceMemory().buffer.BackingBasePointer() + 0x10000;
        std::memset(backing, 0, 0x4000);
        MapPage(table, 4, backing + 0x1000, Common::PageType::Memory);
        MapPage(table, 5, backing, Common::PageType::Memory);
        MapPage(table, 7, backing + 0x2000, Common::PageType::RasterizerCachedMemory);
        MapPage(table, 8, backing + 0x3000, Common::PageType::Memory);
        process.GetMemory().SetCurrentPageTable(process);
    }
    ~GuestPages() {
        system.Kernel().UnregisterKernelObject(&process);
    }

    ScopedSystemTestContext context;
    Core::System system;
    Kernel::KProcess process{system.Kernel()};
    u8* backing{};
};

} // Anonymous namespace

TEST_CASE("Debug server endpoints are loopback only", "[core][nx2_debug_server]") {
    std::string error;
    const auto port_only = Core::Nx2Debug::ParseEndpoint("7777", error);
    REQUIRE(port_only);
    REQUIRE(port_only->host == "127.0.0.1");
    REQUIRE(port_only->port == 7777);
    const auto localhost = Core::Nx2Debug::ParseEndpoint("localhost:1234", error);
    REQUIRE(localhost);
    REQUIRE(localhost->host == "127.0.0.1");
    const auto ipv6 = Core::Nx2Debug::ParseEndpoint("[::1]:80", error);
    REQUIRE(ipv6);
    REQUIRE(ipv6->host == "::1");
    REQUIRE_FALSE(Core::Nx2Debug::ParseEndpoint("0.0.0.0:7777", error));
    REQUIRE(error.find("loopback") != std::string::npos);
    REQUIRE_FALSE(Core::Nx2Debug::ParseEndpoint("192.168.1.2:7777", error));
    REQUIRE_FALSE(Core::Nx2Debug::ParseEndpoint("127.0.0.1:", error));
    REQUIRE_FALSE(Core::Nx2Debug::ParseEndpoint("127.0.0.1:70000", error));
    REQUIRE_FALSE(Core::Nx2Debug::ParseEndpoint("12ab", error));
}

TEST_CASE("Guest presents advance debug input across the timestamp ring boundary",
          "[core][nx2_debug_server]") {
    auto& count = Core::g_nx2_present_count;
    auto& ring = Core::g_nx2_present_times_ns;
    const u64 previous_count = count.load();
    const u64 previous_zero = ring[0].load();
    const u64 previous_one = ring[1].load();
    SCOPE_EXIT {
        count.store(previous_count);
        ring[0].store(previous_zero);
        ring[1].store(previous_one);
    };
    const u64 start = ring.size();
    count.store(start - 1);
    ring[0].store(0);
    ring[1].store(0);
    const std::vector<Core::Nx2Debug::InputStep> steps{
        Core::Nx2Debug::ParseInputStep(json{{"buttons", {"L", "R"}}, {"frames", 1}})};
    REQUIRE_FALSE(Core::Nx2Debug::InputAt(steps, start, count.load()));
    Core::RecordGuestPresent();
    const auto input = Core::Nx2Debug::InputAt(steps, start, count.load());
    REQUIRE(input);
    REQUIRE(input->buttons == (Core::HID::NpadButton::L | Core::HID::NpadButton::R));
    REQUIRE(ring[0].load() > 0);
    Core::RecordGuestPresent();
    REQUIRE_FALSE(Core::Nx2Debug::InputAt(steps, start, count.load()));
    REQUIRE(ring[1].load() >= ring[0].load());
}

TEST_CASE("Debug frame times measure intervals and retain the ring tail", "[core][nx2_debug_server]") {
    auto& ring = Core::g_nx2_present_times_ns;
    SCOPE_EXIT {
        for (auto& timestamp : ring) {
            timestamp.store(0);
        }
    };
    const auto read = [](u64 after, u64 end_ns) {
        return Core::Nx2Debug::ReadFrameTimes(0, after, 1, end_ns, 60.0);
    };
    REQUIRE(read(0, 1)["frames"] == 0);
    ring[1] = 1;
    REQUIRE(read(1, 1)["mean_ms"] == 0.0);
    ring[2] = 16'666'668;
    ring[3] = 50'000'001;
    const auto mixed = read(3, 50'000'001);
    REQUIRE(mixed["frames"] == 2);
    REQUIRE(mixed["mean_ms"] == 25.0);
    REQUIRE(mixed["p50_ms"] == 25.0);
    REQUIRE(mixed["fps"] == 40.0);
    REQUIRE(mixed["vsync_buckets"]["1"] == 0.5);
    REQUIRE(mixed["vsync_buckets"]["2"] == 0.5);
    REQUIRE(mixed["intervals_ms"] == json::array({16.7, 33.3}));
    REQUIRE(mixed["stutters"] == 0);
    REQUIRE(mixed["truncated"] == false);
    ring[4] = 150'000'001;
    REQUIRE(read(4, 150'000'001)["stutters"] == 1);
    // An overwritten slot must not make an interval across the missing sample.
    ring[2] = 200'000'001;
    REQUIRE(read(3, 50'000'001)["frames"] == 0);
    REQUIRE(read(3, 50'000'001)["truncated"] == true);
    for (u64 frame = 1; frame <= 5000; ++frame) {
        ring[frame % ring.size()] = frame * 1'000'000;
    }
    const auto overflow = read(5000, 5'000'000'000);
    REQUIRE(overflow["frames"] == ring.size() - 1);
    REQUIRE(overflow["mean_ms"] == 1.0);
    REQUIRE(overflow["truncated"] == true);
    REQUIRE(overflow["intervals_ms"].size() == 120);
    // A counted present whose timestamp store is pending still holds the previous lap's value.
    ring[5000 % ring.size()] = (5000 - ring.size()) * 1'000'000;
    const auto pending = read(5000, 5'000'000'000);
    REQUIRE(pending["frames"] == ring.size() - 2);
    REQUIRE(pending["max_ms"] == 1.0);
}

TEST_CASE("Measured input excludes boundary frames and rejects incomplete coverage", "[core][nx2_debug_server]") {
    auto& ring = Core::g_nx2_present_times_ns;
    const auto old_count = Core::g_nx2_present_count.load();
    SCOPE_EXIT {
        Core::g_nx2_present_count = old_count;
        for (auto& timestamp : ring) {
            timestamp = 0;
        }
    };
    ring[9] = 1;
    ring[10] = 100'000'000;
    ring[11] = 116'123'456;
    ring[12] = 900'000'000;
    Core::g_nx2_present_count = 12;
    const auto read = [](u64 total) {
        return Core::Nx2Debug::ReadInputFrameTimes(10, total, 90'000'000, 1'000'000'000);
    };
    REQUIRE(read(1)["complete"] == true);
    REQUIRE(read(1)["frames"] == 0);
    REQUIRE(read(2)["intervals_ms"] == json::array({16.123456}));
    REQUIRE(read(2)["first_present"] == 10);
    REQUIRE(read(2)["last_present"] == 11);
    for (u64 invalid : {u64{0}, u64{1}, u64{1'000'000'001}}) {
        ring[11] = invalid;
        REQUIRE(read(2)["complete"] == false);
        REQUIRE(read(2)["intervals_ms"].empty());
    }
    ring[11] = 116'123'456;
    Core::g_nx2_present_count = 4106;
    REQUIRE(read(2)["truncated"] == true);
    REQUIRE_THROWS(read(0));
    REQUIRE_NOTHROW(read(3840));
    REQUIRE_THROWS(read(3841));
}

TEST_CASE("Debug server requests map to handlers", "[core][nx2_debug_server]") {
    const Handlers handlers = EchoHandlers();
    REQUIRE(json::parse(Core::Nx2Debug::HandleRequest(
                handlers, R"({"id":7,"method":"echo","params":{"a":1}})")) ==
            json{{"id", 7}, {"result", {{"a", 1}}}});
    REQUIRE(json::parse(Core::Nx2Debug::HandleRequest(handlers, R"({"id":"x","method":"echo"})")) ==
            json{{"id", "x"}, {"result", json::object()}});
    REQUIRE(json::parse(Core::Nx2Debug::HandleRequest(handlers, R"({"id":1,"method":"fail"})")) ==
            json{{"id", 1}, {"error", "broken"}});
    const json unknown = json::parse(Core::Nx2Debug::HandleRequest(handlers, R"({"id":2,"method":"nope"})"));
    REQUIRE(unknown["id"] == 2);
    REQUIRE(unknown["error"].get<std::string>().find("unknown method") != std::string::npos);
    const json garbage = json::parse(Core::Nx2Debug::HandleRequest(handlers, "{not json"));
    REQUIRE(garbage["id"].is_null());
    REQUIRE(garbage.contains("error"));
    const json bad_params =
        json::parse(Core::Nx2Debug::HandleRequest(handlers, R"({"id":3,"method":"echo","params":[1]})"));
    REQUIRE(bad_params.contains("error"));
}

TEST_CASE("Debug server shader hashes need all 16 hex digits", "[core][nx2_debug_server]") {
    REQUIRE(Core::Nx2Debug::ParseShaderHash("00000000621c4755") == 0x621c4755);
    REQUIRE(Core::Nx2Debug::ParseShaderHash("3DDFB1D7621C4755") == 0x3ddfb1d7621c4755);
    REQUIRE(Core::Nx2Debug::ParseShaderHash("0x3ddfb1d7621c4755") == 0x3ddfb1d7621c4755);
    for (const char* bad : {"621c4755", "3ddfb1d7", "0x621c4755", "", "0x", "3ddfb1d7621c47551",
                            "3ddfb1d7621c475g", "-ddfb1d7621c4755", " 3ddfb1d7621c475"}) {
        try {
            (void)Core::Nx2Debug::ParseShaderHash(bad);
            FAIL("accepted " << bad);
        } catch (const std::runtime_error& error) {
            REQUIRE(std::string_view{error.what()}.find("16 hex digits") != std::string_view::npos);
        }
    }
}

TEST_CASE("Debug server base64 pads like RFC 4648", "[core][nx2_debug_server]") {
    const auto encode = [](std::string_view text) {
        return Core::Nx2Debug::Base64(
            {reinterpret_cast<const u8*>(text.data()), text.size()});
    };
    REQUIRE(encode("") == "");
    REQUIRE(encode("f") == "Zg==");
    REQUIRE(encode("fo") == "Zm8=");
    REQUIRE(encode("foo") == "Zm9v");
    REQUIRE(encode("foobar") == "Zm9vYmFy");
}

TEST_CASE("Debug server answers over loopback TCP", "[core][nx2_debug_server]") {
    Core::Nx2Debug::Server server{{"127.0.0.1", 0}, EchoHandlers()};
    REQUIRE(server.Port() != 0);
    boost::asio::io_context io;
    boost::asio::ip::tcp::socket socket{io};
    socket.connect({boost::asio::ip::make_address("127.0.0.1"), server.Port()});
    const std::string requests =
        "{\"id\":1,\"method\":\"echo\",\"params\":{\"n\":1}}\n\r\n{\"id\":2,\"method\":\"fail\"}\r\n";
    boost::asio::write(socket, boost::asio::buffer(requests));
    std::string input;
    for (int line = 0; line < 2; ++line) {
        const size_t length = boost::asio::read_until(socket, boost::asio::dynamic_buffer(input), '\n');
        const json response = json::parse(input.substr(0, length - 1));
        input.erase(0, length);
        if (line == 0) {
            REQUIRE(response == json{{"id", 1}, {"result", {{"n", 1}}}});
        } else {
            REQUIRE(response == json{{"id", 2}, {"error", "broken"}});
        }
    }
}

TEST_CASE("Debug control hands captures to the rasterizer once", "[core][nx2_debug_server]") {
    VideoCore::Nx2Debug::BeginSession();
    REQUIRE_FALSE(VideoCore::Nx2Debug::HasRequests());
    VideoCore::Nx2Debug::RequestCapture({.id = 5, .directory = "dir", .sampled = true, .buffers = true});
    REQUIRE(VideoCore::Nx2Debug::HasRequests());
    const auto request = VideoCore::Nx2Debug::TakeCapture();
    REQUIRE(request);
    REQUIRE(request->id == 5);
    REQUIRE(request->sampled);
    REQUIRE(request->buffers);
    REQUIRE_FALSE(VideoCore::Nx2Debug::HasRequests());
    REQUIRE_FALSE(VideoCore::Nx2Debug::TakeCapture());
    REQUIRE_FALSE(VideoCore::Nx2Debug::WaitCapture(5, 1ms));
    std::thread writer{[] { VideoCore::Nx2Debug::FinishCapture(5, {.error = "disk full"}); }};
    const auto result = VideoCore::Nx2Debug::WaitCapture(5, 10s);
    writer.join();
    REQUIRE(result);
    REQUIRE(result->error == "disk full");

    VideoCore::Nx2Debug::RequestCapture({.id = 6, .directory = "dir", .sampled = false, .buffers = false});
    VideoCore::Nx2Debug::CancelCapture(6);
    REQUIRE_FALSE(VideoCore::Nx2Debug::HasRequests());

    // Ending the session wakes a waiter instead of leaving it to its timeout.
    std::thread stopper{[] {
        std::this_thread::sleep_for(20ms);
        VideoCore::Nx2Debug::EndSession();
    }};
    const auto begin = std::chrono::steady_clock::now();
    REQUIRE_FALSE(VideoCore::Nx2Debug::WaitCapture(7, 30s));
    stopper.join();
    REQUIRE(std::chrono::steady_clock::now() - begin < 10s);
}

TEST_CASE("Debug control counts reject generations", "[core][nx2_debug_server]") {
    VideoCore::Nx2Debug::BeginSession();
    REQUIRE_FALSE(VideoCore::Nx2Debug::AnyRejected());
    REQUIRE_FALSE(VideoCore::Nx2Debug::IsRejected(0x1234));
    const std::array<u64, 2> hashes{0x1234, 0x5678};
    const u64 generation = VideoCore::Nx2Debug::SetRejected(hashes, true);
    REQUIRE(VideoCore::Nx2Debug::IsRejected(0x1234));
    REQUIRE(VideoCore::Nx2Debug::SetRejected(hashes, true) == generation);
    REQUIRE_FALSE(VideoCore::Nx2Debug::WaitRejectApplied(generation, 1ms));
    const auto change = VideoCore::Nx2Debug::TakeRejectChange();
    REQUIRE(change);
    REQUIRE_FALSE(VideoCore::Nx2Debug::TakeRejectChange());
    VideoCore::Nx2Debug::NoteRejectApplied(change->generation);
    REQUIRE(VideoCore::Nx2Debug::WaitRejectApplied(generation, 1ms));
    const std::array<u64, 2> both{0x1234, 0x5678};
    VideoCore::Nx2Debug::SetRejected(both, false);
    REQUIRE_FALSE(VideoCore::Nx2Debug::AnyRejected());
    REQUIRE(VideoCore::Nx2Debug::TakeRejectChange()->unrejected);
    VideoCore::Nx2Debug::EndSession();
}

TEST_CASE("Debug memory reads guest pages and names the first unmapped byte", "[core][nx2_debug_server]") {
    GuestPages guest;
    for (u32 i = 0; i < 0x4000; ++i) {
        guest.backing[i] = static_cast<u8>(i * 7 + (i >> 12));
    }
    auto& memory = guest.process.GetMemory();
    std::array<u8, 0x20> out{};
    REQUIRE_FALSE(Core::Nx2Debug::ReadGuestMemory(memory, 0x4ff0, out));
    REQUIRE(std::memcmp(out.data(), guest.backing + 0x1ff0, 0x10) == 0);
    REQUIRE(std::memcmp(out.data() + 0x10, guest.backing, 0x10) == 0);
    REQUIRE_FALSE(Core::Nx2Debug::ReadGuestMemory(memory, 0x7000, out));
    REQUIRE(std::memcmp(out.data(), guest.backing + 0x2000, out.size()) == 0);
    REQUIRE(Core::Nx2Debug::ReadGuestMemory(memory, 0x5ff0, out) == u64{0x6000});
    REQUIRE(Core::Nx2Debug::ReadGuestMemory(memory, 0x6800, out) == u64{0x6800});
    REQUIRE(Core::Nx2Debug::ReadGuestMemory(memory, 0x100000, out) == u64{0x100000});
    // The top 16 bits would alias page 4 through GetPointerSilent's mask.
    REQUIRE(Core::Nx2Debug::ReadGuestMemory(memory, (1ULL << 48) | 0x4000, out) ==
            ((1ULL << 48) | 0x4000));
}

TEST_CASE("Debug memory search spans mapped pages, stops at limits and resumes", "[core][nx2_debug_server]") {
    GuestPages guest;
    constexpr std::array<u8, 5> pattern{0xde, 0xad, 0xbe, 0xef, 0x11};
    const auto plant = [&](u8* host, size_t first, size_t count) {
        std::memcpy(host, pattern.data() + first, count);
    };
    plant(guest.backing + 0x1ffe, 0, 2); // 0x4ffe, continued on page 5
    plant(guest.backing, 2, 3);
    plant(guest.backing + 0x0ffe, 0, 2); // 0x5ffe, cut off by unmapped page 6
    plant(guest.backing + 0x2010, 0, 5); // 0x7010
    plant(guest.backing + 0x3000, 0, 5); // 0x8000
    plant(guest.backing + 0x3100, 0, 5); // 0x8100
    auto& memory = guest.process.GetMemory();
    const auto later = std::chrono::steady_clock::now() + 60s;

    const auto all = Core::Nx2Debug::SearchGuestMemory(memory, pattern, 0x1000, 0x20000, 256, later);
    REQUIRE(all.hits == std::vector<u64>{0x4ffe, 0x7010, 0x8000, 0x8100});
    REQUIRE(all.complete);
    REQUIRE(all.scanned == 0x4000);

    const auto first = Core::Nx2Debug::SearchGuestMemory(memory, pattern, 0x1000, 0x20000, 2, later);
    REQUIRE(first.hits == std::vector<u64>{0x4ffe, 0x7010});
    REQUIRE_FALSE(first.complete);
    REQUIRE(first.next == 0x7011);
    const auto rest = Core::Nx2Debug::SearchGuestMemory(memory, pattern, first.next, 0x20000, 2, later);
    REQUIRE(rest.hits == std::vector<u64>{0x8000, 0x8100});

    const auto inside = Core::Nx2Debug::SearchGuestMemory(memory, pattern, 0x4fff, 0x8004, 256, later);
    REQUIRE(inside.hits == std::vector<u64>{0x7010});

    const auto expired = Core::Nx2Debug::SearchGuestMemory(memory, pattern, 0x4000, 0x9000, 256,
                                                           std::chrono::steady_clock::now());
    REQUIRE(expired.hits.empty());
    REQUIRE_FALSE(expired.complete);
    REQUIRE(expired.next == 0x4000);
}

TEST_CASE("Memory scans keep typed hits and refine them by their change",
          "[core][nx2_debug_server]") {
    using Core::Nx2Debug::ScanOp;
    using Core::Nx2Debug::ScanType;
    GuestPages guest;
    // Guest 0x4000 is host +0x1000, 0x5000 is +0, 0x7000 is +0x2000, 0x8000 is +0x3000.
    const auto put = [&](size_t host, auto value) {
        std::memcpy(guest.backing + host, &value, sizeof(value));
    };
    put(0x1010, 97.5f);   // 0x4010
    put(0x0020, 98.0f);   // 0x5020
    put(0x2030, 250.0f);  // 0x7030
    put(0x1022, 95.0f);   // 0x4022: not aligned to 4, never a hit
    auto& memory = guest.process.GetMemory();
    const auto later = std::chrono::steady_clock::now() + 60s;
    const std::array whole{std::pair<u64, u64>{0x4000, 0x9000}};
    const auto range = [](double low, double high) {
        return Core::Nx2Debug::ScanTest{.op = ScanOp::Range, .low = low, .high = high};
    };
    auto list = Core::Nx2Debug::ScanGuestMemory(memory, whole, ScanType::F32, range(90, 300), 100, later);
    REQUIRE(list.addresses == std::vector<u64>{0x4010, 0x5020, 0x7030});
    REQUIRE(list.complete);
    REQUIRE_FALSE(list.truncated);
    REQUIRE(list.scanned == 0x4000); // page 6 is not mapped

    // Link turns: one value goes up, one down, one stays.
    put(0x1010, 120.0f);
    put(0x2030, 240.0f);
    auto changed = list;
    Core::Nx2Debug::RefineScan(memory, changed, {.op = ScanOp::Changed});
    REQUIRE(changed.addresses == std::vector<u64>{0x4010, 0x7030});
    // A refine keeps the values it read: the next one compares with them.
    REQUIRE(Core::Nx2Debug::ScanValue(ScanType::F32, changed.values[0]) == 120.0);
    auto again = changed;
    Core::Nx2Debug::RefineScan(memory, again, {.op = ScanOp::Increased});
    REQUIRE(again.addresses.empty());
    auto up = list;
    Core::Nx2Debug::RefineScan(memory, up, {.op = ScanOp::Increased});
    REQUIRE(up.addresses == std::vector<u64>{0x4010});
    auto down = list;
    Core::Nx2Debug::RefineScan(memory, down, {.op = ScanOp::Decreased});
    REQUIRE(down.addresses == std::vector<u64>{0x7030});
    auto same = list;
    Core::Nx2Debug::RefineScan(memory, same, {.op = ScanOp::Unchanged});
    REQUIRE(same.addresses == std::vector<u64>{0x5020});
    auto equal = list;
    Core::Nx2Debug::RefineScan(memory, equal, {.op = ScanOp::Equal, .low = 240.0, .high = 240.0});
    REQUIRE(equal.addresses == std::vector<u64>{0x7030});
    auto narrowed = list;
    Core::Nx2Debug::RefineScan(memory, narrowed, range(100, 130));
    REQUIRE(narrowed.addresses == std::vector<u64>{0x4010});

    // Integers are compared by their type; the hit cap truncates; ranges bound the scan.
    put(0x3000, u32{0xffffffff}); // 0x8000
    const auto equal_to = [](double value) {
        return Core::Nx2Debug::ScanTest{.op = ScanOp::Equal, .low = value, .high = value};
    };
    REQUIRE(Core::Nx2Debug::ScanGuestMemory(memory, whole, ScanType::I32, equal_to(-1), 100, later)
                .addresses == std::vector<u64>{0x8000});
    REQUIRE(Core::Nx2Debug::ScanGuestMemory(memory, whole, ScanType::U32, equal_to(4294967295.0), 100,
                                            later)
                .addresses == std::vector<u64>{0x8000});
    REQUIRE(Core::Nx2Debug::ScanGuestMemory(memory, whole, ScanType::U16, equal_to(65535), 100, later)
                .addresses == std::vector<u64>{0x8000, 0x8002});
    const auto capped = Core::Nx2Debug::ScanGuestMemory(memory, whole, ScanType::F32, range(90, 300), 2, later);
    REQUIRE(capped.addresses == std::vector<u64>{0x4010, 0x5020});
    REQUIRE(capped.truncated);
    const std::array part{std::pair<u64, u64>{0x4011, 0x5024}};
    REQUIRE(Core::Nx2Debug::ScanGuestMemory(memory, part, ScanType::F32, range(90, 300), 100, later)
                .addresses == std::vector<u64>{0x5020});
    const auto expired = Core::Nx2Debug::ScanGuestMemory(memory, whole, ScanType::F32, range(90, 300), 100,
                                                         std::chrono::steady_clock::now());
    REQUIRE_FALSE(expired.complete);
    REQUIRE(expired.addresses.empty());
}

TEST_CASE("Value watches read their addresses once per present of the call",
          "[core][nx2_debug_server]") {
    using Core::Nx2Debug::ScanType;
    GuestPages guest;
    float facing = 97.0f;
    std::memcpy(guest.backing + 0x1010, &facing, sizeof(facing)); // guest 0x4010
    const u16 angle = 0x8000;
    std::memcpy(guest.backing + 0x3000, &angle, sizeof(angle)); // guest 0x8000
    Core::Nx2Debug::ValueWatch watch{guest.process.GetMemory(),
                                     {{0x4010, ScanType::F32}, {0x6000, ScanType::U32},
                                      {0x8000, ScanType::U16}},
                                     10, 14};
    watch.Sample(9);
    watch.Sample(10);
    facing = 243.0f;
    std::memcpy(guest.backing + 0x1010, &facing, sizeof(facing));
    watch.Sample(11);
    watch.Sample(11);
    watch.Sample(13);
    watch.Sample(14);
    const json result = watch.Json();
    REQUIRE(result["addresses"][0] == json{{"address", "0x4010"}, {"as", "f32"}});
    REQUIRE(result["presents"] == 4);
    REQUIRE(result["sampled"] == 3);
    // Page 6 is not mapped: its value is null.
    REQUIRE(result["rows"] == json{{10, 97.0, nullptr, 32768}, {11, 243.0, nullptr, 32768},
                                   {13, 243.0, nullptr, 32768}});
}

TEST_CASE("A finished watch hands its rows over before the reply is built",
          "[core][nx2_debug_server]") {
    using Core::Nx2Debug::ScanType;
    GuestPages guest;
    Core::Nx2Debug::WatchSlot slot;
    REQUIRE_FALSE(slot.Active());
    slot.Start(Core::Nx2Debug::ValueWatch{guest.process.GetMemory(), {{0x4010, ScanType::F32}}, 10, 20});
    REQUIRE(slot.Active());
    slot.Sample(10);
    slot.Sample(11);
    // Finish leaves the slot empty and inactive: the rows belong to the caller, so building the
    // JSON from them takes no lock that a present reply needs.
    const auto done = slot.Finish();
    REQUIRE_FALSE(slot.Active());
    slot.Sample(12);
    REQUIRE_FALSE(slot.Finish());
    REQUIRE(done);
    REQUIRE(done->Json()["sampled"] == 2);
}

TEST_CASE("Watches name at most 8 aligned addresses, frame steps only", "[core][nx2_debug_server]") {
    ScopedSystemTestContext context;
    Core::System system;
    auto& controller = *system.HIDCore().GetEmulatedController(Core::HID::NpadIdType::Player1);
    controller.SetNpadStyleIndex(Core::HID::NpadStyleIndex::Fullkey);
    controller.Connect();
    const auto refusal = [&system](const json& watch, const json& steps = {{{"frames", 10}}}) -> std::string {
        try {
            static_cast<void>(Core::Nx2Debug::CallTool(system, "input_sequence",
                                                       {{"steps", steps}, {"watch", watch}}));
        } catch (const std::exception& error) {
            return error.what();
        }
        return {};
    };
    const json one = {{{"address", "0x4010"}, {"as", "f32"}}};
    json nine = json::array();
    for (int i = 0; i < 9; ++i) {
        nine.push_back({{"address", 0x4000 + i * 4}, {"as", "u32"}});
    }
    using Catch::Matchers::ContainsSubstring;
    REQUIRE_THAT(refusal(one, {{{"ms", 50}}}), ContainsSubstring("needs frame steps"));
    REQUIRE_THAT(refusal(nine), ContainsSubstring("1 to 8"));
    REQUIRE_THAT(refusal({{{"address", "0x4011"}, {"as", "f32"}}}), ContainsSubstring("not aligned"));
    REQUIRE_THAT(refusal({{{"address", "0x4010"}, {"as", "s16"}}}), ContainsSubstring("want u8"));
    REQUIRE_THAT(refusal({{{"as", "u8"}}}), ContainsSubstring("{address, as}"));
    REQUIRE_THAT(refusal(one), ContainsSubstring("no application process"));
    REQUIRE(Core::g_nx2_present_reply_hook.load() == nullptr);
}

TEST_CASE("scan_memory checks its parameters before it reads memory", "[core][nx2_debug_server]") {
    ScopedSystemTestContext context;
    Core::System system;
    const auto refusal = [&system](const json& params) -> std::string {
        try {
            static_cast<void>(Core::Nx2Debug::CallTool(system, "scan_memory", params));
        } catch (const std::exception& error) {
            return error.what();
        }
        return {};
    };
    using Catch::Matchers::ContainsSubstring;
    REQUIRE(Core::Nx2Debug::CallTool(system, "scan_memory", {{"action", "drop"}}) == json{{"dropped", 0}});
    REQUIRE_THAT(refusal({{"action", "refine"}, {"op", "changed"}}), ContainsSubstring("no scan list"));
    REQUIRE_THAT(refusal({{"action", "first"}, {"type", "s16"}, {"value", 1}}),
                 ContainsSubstring("want u8, u16, u32, i32, f32 or f64"));
    REQUIRE_THAT(refusal({{"action", "first"}, {"type", "u32"}, {"value", 1.5}}),
                 ContainsSubstring("whole"));
    REQUIRE_THAT(refusal({{"action", "first"}, {"type", "f32"}, {"op", "changed"}}),
                 ContainsSubstring("a first scan wants equal or range"));
    REQUIRE_THAT(refusal({{"action", "first"}, {"type", "f32"}, {"min", 2}, {"max", 1}}),
                 ContainsSubstring("min must not exceed max"));
    REQUIRE_THAT(refusal({{"action", "first"}, {"type", "f32"}, {"value", 1}}),
                 ContainsSubstring("no application process"));
    REQUIRE_THAT(refusal({{"action", "peek"}}), ContainsSubstring("action must be"));
}

TEST_CASE("Debug memory patterns are hex byte pairs", "[core][nx2_debug_server]") {
    const std::vector<u8> one{0x00, 0x00, 0x80, 0x3f};
    REQUIRE(Core::Nx2Debug::ParseHexBytes("0000803f") == one);
    REQUIRE(Core::Nx2Debug::ParseHexBytes("0x00 00 80 3F") == one);
    for (const char* bad : {"", "0x", "abc", "zz", "0x-1"}) {
        CAPTURE(bad);
        REQUIRE_THROWS_AS(Core::Nx2Debug::ParseHexBytes(bad), std::runtime_error);
    }
}

TEST_CASE("Debug memory formats as hex, words and floats", "[core][nx2_debug_server]") {
    std::array<u8, 16> bytes{};
    const std::array<float, 4> floats{1.0f, std::numeric_limits<float>::quiet_NaN(),
                                      -std::numeric_limits<float>::infinity(), -2.5f};
    std::memcpy(bytes.data(), floats.data(), bytes.size());
    REQUIRE(Core::Nx2Debug::FormatMemory(std::span{bytes}.first(4), "hex") ==
            json{{"hex", "0000803f"}});
    REQUIRE(Core::Nx2Debug::FormatMemory(bytes, "f32") ==
            json{{"f32", {1.0, "nan", "-inf", -2.5}}});
    REQUIRE(Core::Nx2Debug::FormatMemory(std::span{bytes}.first(4), "u32") ==
            json{{"u32", {0x3f800000u}}});
    REQUIRE(Core::Nx2Debug::FormatMemory(std::span{bytes}.first(8), "u64") ==
            json{{"u64", {"0x7fc000003f800000"}}});
    REQUIRE_THROWS_AS(Core::Nx2Debug::FormatMemory(std::span{bytes}.first(12), "u64"),
                      std::runtime_error);
    REQUIRE_THROWS_AS(Core::Nx2Debug::FormatMemory(bytes, "f16"), std::runtime_error);
}

TEST_CASE("Debug control hands GPU reads to the rasterizer once", "[core][nx2_debug_server]") {
    VideoCore::Nx2Debug::BeginSession();
    VideoCore::Nx2Debug::RequestGpuRead({.id = 3, .gpu_address = 0x1000, .size = 8});
    REQUIRE(VideoCore::Nx2Debug::HasRequests());
    const auto request = VideoCore::Nx2Debug::TakeGpuRead();
    REQUIRE(request);
    REQUIRE(request->gpu_address == 0x1000);
    REQUIRE(request->size == 8);
    REQUIRE_FALSE(VideoCore::Nx2Debug::HasRequests());
    REQUIRE_FALSE(VideoCore::Nx2Debug::TakeGpuRead());
    std::thread rasterizer{[] {
        VideoCore::Nx2Debug::GpuReadResult result{};
        result.bytes = {1, 2, 3};
        result.address_space = 9;
        VideoCore::Nx2Debug::FinishGpuRead(3, std::move(result));
    }};
    const auto result = VideoCore::Nx2Debug::WaitGpuRead(3, 10s);
    rasterizer.join();
    REQUIRE(result);
    REQUIRE(result->bytes == std::vector<u8>{1, 2, 3});
    REQUIRE(result->address_space == 9);

    VideoCore::Nx2Debug::RequestGpuRead({.id = 4, .gpu_address = 0, .size = 4});
    VideoCore::Nx2Debug::CancelGpuRead(4);
    REQUIRE_FALSE(VideoCore::Nx2Debug::HasRequests());
    REQUIRE_FALSE(VideoCore::Nx2Debug::WaitGpuRead(4, 1ms));
    VideoCore::Nx2Debug::EndSession();
}

TEST_CASE("Debug server tells a waiting handler that its client left", "[core][nx2_debug_server]") {
    std::atomic<bool> noticed{};
    Handlers handlers{{"wait", [&noticed](const json&, const ClientGone& client_gone) -> json {
                           const auto deadline = std::chrono::steady_clock::now() + 10s;
                           while (!client_gone()) {
                               if (std::chrono::steady_clock::now() >= deadline) {
                                   return {};
                               }
                               std::this_thread::sleep_for(2ms);
                           }
                           noticed = true;
                           return {};
                       }}};
    Core::Nx2Debug::Server server{{"127.0.0.1", 0}, std::move(handlers)};
    {
        boost::asio::io_context io;
        boost::asio::ip::tcp::socket socket{io};
        socket.connect({boost::asio::ip::make_address("127.0.0.1"), server.Port()});
        boost::asio::write(socket, boost::asio::buffer(std::string{"{\"id\":1,\"method\":\"wait\"}\n"}));
        std::this_thread::sleep_for(50ms);
    }
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!noticed && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE(noticed);
}

TEST_CASE("Debug input steps parse buttons, sticks and frames", "[core][nx2_debug_server]") {
    const auto step = Core::Nx2Debug::ParseInputStep(
        json{{"buttons", {"A", "ZL"}}, {"left_stick", {1.0, -0.5}}, {"frames", 3}});
    REQUIRE(step.frames == 3);
    REQUIRE(step.input.buttons == (Core::HID::NpadButton::A | Core::HID::NpadButton::ZL));
    REQUIRE(step.input.left->x == Core::HID::HID_JOYSTICK_MAX);
    REQUIRE(step.input.left->y == -16384);
    REQUIRE_FALSE(step.input.right);
    const auto idle = Core::Nx2Debug::ParseInputStep(json{{"frames", 1}});
    REQUIRE(idle.input.buttons == Core::HID::NpadButton::None);
    for (const json& bad : {json{{"buttons", {"A"}}}, json{{"frames", 0}}, json{{"frames", 36001}},
                            json{{"frames", 1}, {"buttons", {"Home"}}},
                            json{{"frames", 1}, {"stick_l", {0, 0}}},
                            json{{"frames", 1}, {"left_stick", {1.5, 0}}},
                            json{{"frames", 1}, {"right_stick", {0}}}, json::array()}) {
        CAPTURE(bad.dump());
        REQUIRE_THROWS_AS(Core::Nx2Debug::ParseInputStep(bad), std::exception);
    }
}

TEST_CASE("Debug input steps follow the present count exactly", "[core][nx2_debug_server]") {
    std::vector<Core::Nx2Debug::InputStep> steps(3);
    steps[0] = {.input = {.buttons = Core::HID::NpadButton::A, .left = {}, .right = {}}, .frames = 2};
    steps[1] = {.input = {}, .frames = 1};
    steps[2] = {.input = {.buttons = Core::HID::NpadButton::B, .left = {}, .right = {}}, .frames = 1};
    REQUIRE_FALSE(Core::Nx2Debug::InputAt(steps, 10, 9));
    REQUIRE(Core::Nx2Debug::InputAt(steps, 10, 10)->buttons == Core::HID::NpadButton::A);
    REQUIRE(Core::Nx2Debug::InputAt(steps, 10, 11)->buttons == Core::HID::NpadButton::A);
    REQUIRE(Core::Nx2Debug::InputAt(steps, 10, 12)->buttons == Core::HID::NpadButton::None);
    REQUIRE(Core::Nx2Debug::InputAt(steps, 10, 13)->buttons == Core::HID::NpadButton::B);
    REQUIRE_FALSE(Core::Nx2Debug::InputAt(steps, 10, 14));
}

TEST_CASE("Debug input steps can follow host time instead of presents", "[core][nx2_debug_server]") {
    const auto timed = Core::Nx2Debug::ParseInputStep(json{{"buttons", {"A"}}, {"ms", 250}});
    REQUIRE(timed.ms == 250);
    REQUIRE(timed.frames == 0);
    for (const json& bad : {json{{"ms", 0}}, json{{"ms", 600001}}, json{{"ms", 1.5}},
                            json{{"ms", 5}, {"frames", 5}}}) {
        CAPTURE(bad.dump());
        REQUIRE_THROWS_AS(Core::Nx2Debug::ParseInputStep(bad), std::exception);
    }
    // A decimal is not a range error: the message says what is wrong.
    REQUIRE_THROWS_WITH(Core::Nx2Debug::ParseInputStep(json{{"ms", 16.5}}),
                        Catch::Matchers::ContainsSubstring("ms must be a whole number, 1 to 600000 (got 16.5)"));
    REQUIRE_THROWS_WITH(Core::Nx2Debug::ParseInputStep(json{{"frames", 2.5}}),
                        Catch::Matchers::ContainsSubstring("frames must be a whole number"));
    std::vector<Core::Nx2Debug::InputStep> steps(2);
    steps[0] = {.input = {.buttons = Core::HID::NpadButton::A, .left = {}, .right = {}}, .frames = 0,
                .ms = 2};
    steps[1] = {.input = {.buttons = Core::HID::NpadButton::B, .left = {}, .right = {}}, .frames = 0,
                .ms = 1};
    REQUIRE(Core::Nx2Debug::InputAtTime(steps, 0)->buttons == Core::HID::NpadButton::A);
    REQUIRE(Core::Nx2Debug::InputAtTime(steps, 1999)->buttons == Core::HID::NpadButton::A);
    REQUIRE(Core::Nx2Debug::InputAtTime(steps, 2000)->buttons == Core::HID::NpadButton::B);
    REQUIRE_FALSE(Core::Nx2Debug::InputAtTime(steps, 3000));
}

TEST_CASE("Debug timed input steps bound ms and never count presents", "[core][nx2_debug_server]") {
    REQUIRE(Core::Nx2Debug::ParseInputStep(json{{"ms", 600000}}).ms == 600000);
    for (const json& bad : {json::object(), json{{"ms", -1}}, json{{"ms", "5"}},
                            json{{"ms", std::numeric_limits<u64>::max()}},
                            json{{"ms", 5}, {"frames", 0}}}) {
        CAPTURE(bad.dump());
        REQUIRE_THROWS_AS(Core::Nx2Debug::ParseInputStep(bad), std::exception);
    }
    std::vector<Core::Nx2Debug::InputStep> steps(1);
    steps[0] = {.input = {.buttons = Core::HID::NpadButton::A, .left = {}, .right = {}}, .frames = 0,
                .ms = 600000};
    REQUIRE_FALSE(Core::Nx2Debug::InputAt(steps, 10, 10));
    REQUIRE(Core::Nx2Debug::InputAtTime(steps, 599'999'999)->buttons == Core::HID::NpadButton::A);
    REQUIRE_FALSE(Core::Nx2Debug::InputAtTime(steps, 600'000'000));
    REQUIRE_FALSE(Core::Nx2Debug::InputAtTime({}, 0));
}

namespace {

/// Stamps a present every 2 ms, like the GPU thread at 500 fps, and calls the present and reply
/// hooks as nx2_gsv.h does, until it is destroyed.
class FakePresenter {
public:
    FakePresenter()
        : thread{[this] {
              while (!stop) {
                  const u64 present = ++Core::g_nx2_present_count;
                  Core::g_nx2_present_times_ns[present % Core::g_nx2_present_times_ns.size()] =
                      std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
                  if (const auto hook = Core::g_nx2_present_hook.load()) {
                      hook(present);
                  }
                  if (const auto reply = Core::g_nx2_present_reply_hook.load()) {
                      reply(present);
                  }
                  std::this_thread::sleep_for(2ms);
              }
          }} {}
    ~FakePresenter() {
        stop = true;
        thread.join();
    }

private:
    std::atomic<bool> stop{};
    std::thread thread;
};

/// Calls the npad read hook for controller every 0.3 ms, like the npad updater, until destroyed.
class FakeNpad {
public:
    explicit FakeNpad(const Core::HID::EmulatedController& controller)
        : thread{[this, &controller] {
              while (!stop) {
                  if (const auto hook = Core::HID::g_npad_read_hook.load()) {
                      hook(controller, controller.GetNpadButtons().raw, controller.GetSticks());
                  }
                  std::this_thread::sleep_for(300us);
              }
          }} {}
    ~FakeNpad() {
        stop = true;
        thread.join();
    }

private:
    std::atomic<bool> stop{};
    std::thread thread;
};

} // Anonymous namespace

TEST_CASE("Debug input answers as before unless the client asks for ms or measure",
          "[core][nx2_debug_server]") {
    ScopedSystemTestContext context;
    Core::System system;
    auto& controller = *system.HIDCore().GetEmulatedController(Core::HID::NpadIdType::Player1);
    controller.SetNpadStyleIndex(Core::HID::NpadStyleIndex::Fullkey);
    controller.Connect();
    REQUIRE(controller.IsConnected());
    const auto old_count = Core::g_nx2_present_count.load();
    SCOPE_EXIT {
        Core::g_nx2_present_count = old_count;
        for (auto& timestamp : Core::g_nx2_present_times_ns) {
            timestamp = 0;
        }
    };
    const auto call = [&](std::string_view method, const json& params) {
        FakePresenter presenter;
        // A measured frame-counted call refuses when a present lands while it installs its
        // injector. That is its designed check, so try again a few times.
        for (int attempt = 1;; ++attempt) {
            try {
                return Core::Nx2Debug::CallTool(system, method, params);
            } catch (const std::runtime_error& error) {
                if (attempt == 5 || !std::string_view{error.what()}.starts_with("present advanced")) {
                    throw;
                }
            }
        }
    };
    const auto keys = [](const json& result) {
        std::vector<std::string> names;
        for (const auto& [key, value] : result.items()) {
            names.push_back(key);
        }
        return names;
    };
    // The response origin gives: no measurement, clock or ms, and frames as requested.
    const std::vector<std::string> plain_keys{"controller", "elapsed_s", "first_present", "frames",
                                              "steps"};
    const json plain = call("input", {{"buttons", {"A"}}, {"frames", 3}});
    REQUIRE(keys(plain) == plain_keys);
    REQUIRE(plain["frames"] == 3);
    REQUIRE(plain["controller"] == "player1");
    const json sequence = call(
        "input_sequence", {{"steps", {{{"buttons", {"A"}}, {"frames", 2}}, {{"frames", 1}}}}});
    REQUIRE(keys(sequence) == plain_keys);
    REQUIRE(sequence["frames"] == 3);
    REQUIRE(sequence["steps"] == 2);
    REQUIRE(controller.GetNpadButtons().raw == Core::HID::NpadButton::None);

    const json measured = call("input", {{"buttons", {"A"}}, {"frames", 3}, {"measure", true}});
    REQUIRE(measured["frames"] == 3);
    REQUIRE(measured["landed_present"] == measured["first_present"]);
    REQUIRE(measured["measurement"]["supported"] == "held-input-v1");
    REQUIRE(measured["measurement"]["complete"] == true);
    REQUIRE(measured["measurement"]["intervals_ms"].size() == 2);
    REQUIRE_FALSE(measured.contains("clock"));

    const json timed = call("input_sequence", {{"steps", {{{"buttons", {"A"}}, {"ms", 100}}}},
                                               {"measure", true}});
    REQUIRE(timed["clock"] == "host_time");
    REQUIRE(timed["ms"] == 100);
    REQUIRE(timed["measurement"]["complete"] == true);
    REQUIRE(timed["measurement"]["window_ms"] == 100);
    REQUIRE(timed["frames"].get<u64>() >= 2);
    REQUIRE(controller.GetNpadButtons().raw == Core::HID::NpadButton::None);
    REQUIRE_THROWS(call("input_sequence", {{"steps", {{{"frames", 1}}, {{"ms", 1}}}}}));
}

TEST_CASE("Injected input merges buttons and replaces sticks while installed", "[core][nx2_debug_server]") {
    Core::HID::EmulatedController controller{Core::HID::NpadIdType::Player1};
    REQUIRE(controller.GetNpadButtons().raw == Core::HID::NpadButton::None);
    Core::HID::InjectedInput input{.buttons = Core::HID::NpadButton::A | Core::HID::NpadButton::ZR,
                                   .left = Core::HID::AnalogStickState{.x = -30000, .y = 20000},
                                   .right = {}};
    bool active = true;
    controller.SetInputInjector([&]() -> std::optional<Core::HID::InjectedInput> {
        return active ? std::optional{input} : std::nullopt;
    });
    using enum Core::HID::NpadButton;
    REQUIRE(controller.GetNpadButtons().raw == (A | ZR | StickLLeft | StickLUp));
    REQUIRE(controller.GetSticks().left.x == -30000);
    REQUIRE(controller.GetSticks().left.y == 20000);
    REQUIRE(controller.GetSticks().right.x == 0);
    active = false;
    REQUIRE(controller.GetNpadButtons().raw == None);
    active = true;
    controller.SetInputInjector({});
    REQUIRE(controller.GetNpadButtons().raw == None);
    REQUIRE(controller.GetSticks().left.x == 0);
}

TEST_CASE("Input recordings merge equal presents and stop at their limits",
          "[core][nx2_debug_server]") {
    using Core::HID::AnalogStickState;
    const Core::HID::InjectedInput a{.buttons = Core::HID::NpadButton::A,
                                     .left = AnalogStickState{}, .right = AnalogStickState{}};
    Core::HID::InjectedInput b = a;
    b.left = AnalogStickState{.x = 100, .y = -100};
    Core::Nx2Debug::InputRecording recording{10, 6, 2};
    REQUIRE(recording.Add(9, b));
    REQUIRE(recording.Add(10, a));
    REQUIRE(recording.Add(11, a));
    REQUIRE(recording.Add(11, b));
    // Present 12 was skipped: it gets the input of 13.
    REQUIRE(recording.Add(13, b));
    REQUIRE(recording.Presents() == 4);
    REQUIRE(recording.Steps().size() == 2);
    REQUIRE(recording.Steps()[0].frames == 2);
    REQUIRE(recording.Steps()[0].input.left->x == 0);
    REQUIRE(recording.Steps()[1].frames == 2);
    REQUIRE(recording.Steps()[1].input.left->x == 100);
    REQUIRE_FALSE(recording.Add(14, a));
    REQUIRE(recording.Truncated());
    REQUIRE(recording.Presents() == 4);

    Core::Nx2Debug::InputRecording short_recording{1, 3, 10};
    REQUIRE(short_recording.Add(1, a));
    REQUIRE_FALSE(short_recording.Add(5, a));
    REQUIRE(short_recording.Truncated());
    REQUIRE(short_recording.Presents() == 3);
    REQUIRE(short_recording.Steps()[0].frames == 3);
}

TEST_CASE("Pre-roll rings keep the newest presents and cut at a present",
          "[core][nx2_debug_server]") {
    const auto in = [](s32 x) {
        return Core::HID::InjectedInput{.buttons = {},
                                        .left = Core::HID::AnalogStickState{.x = x, .y = 0},
                                        .right = Core::HID::AnalogStickState{}};
    };
    const auto per_present = [](const std::vector<Core::Nx2Debug::InputStep>& steps) {
        std::vector<s32> x;
        for (const auto& step : steps) {
            x.insert(x.end(), step.frames, step.input.left->x);
        }
        return x;
    };
    Core::Nx2Debug::PreRollRing ring{10, 6, 3};
    ring.Add(9, in(1));
    ring.Add(10, in(1));
    ring.Add(11, in(1));
    // Present 12 was skipped: it gets the input of 13.
    ring.Add(13, in(2));
    REQUIRE(per_present(ring.StepsBefore(100)) == std::vector<s32>{1, 1, 2, 2});
    REQUIRE(per_present(ring.StepsBefore(13)) == std::vector<s32>{1, 1, 2});
    // Past 6 presents the oldest leave; past 3 steps the oldest step leaves.
    ring.Add(16, in(3));
    REQUIRE(ring.FirstPresent() == 11);
    REQUIRE(per_present(ring.StepsBefore(100)) == std::vector<s32>{1, 2, 2, 3, 3, 3});
    ring.Add(17, in(4));
    REQUIRE(ring.FirstPresent() == 12);
    REQUIRE(ring.NextPresent() == 18);
    REQUIRE(per_present(ring.StepsBefore(100)) == std::vector<s32>{2, 2, 3, 3, 3, 4});
    ring.DropBefore(13);
    REQUIRE(per_present(ring.StepsBefore(100)) == std::vector<s32>{2, 3, 3, 3, 4});
    // An anchor after everything held: the ring starts again there.
    ring.DropBefore(30);
    REQUIRE(ring.FirstPresent() == 30);
    ring.Add(29, in(5));
    ring.Add(30, in(5));
    REQUIRE(per_present(ring.StepsBefore(100)) == std::vector<s32>{5});
    REQUIRE(Core::Nx2Debug::IsNeutral(in(0)));
    REQUIRE_FALSE(Core::Nx2Debug::IsNeutral(in(1)));
}

TEST_CASE("Recorded input steps parse back to the same input", "[core][nx2_debug_server]") {
    using Core::HID::AnalogStickState;
    using Core::HID::HID_JOYSTICK_MAX;
    REQUIRE(Core::Nx2Debug::InputStepJson({.input = {}, .frames = 4}) == json{{"frames", 4}});
    for (s32 x = -HID_JOYSTICK_MAX; x <= HID_JOYSTICK_MAX; ++x) {
        const Core::Nx2Debug::InputStep step{
            .input = {.buttons = Core::HID::NpadButton::L | Core::HID::NpadButton::StickR,
                      .left = AnalogStickState{.x = x, .y = -x},
                      .right = AnalogStickState{.x = 0, .y = x}},
            .frames = 7};
        const json text = Core::Nx2Debug::InputStepJson(step);
        const auto back = Core::Nx2Debug::ParseInputStep(text);
        if (back.frames != 7 || back.input.buttons != step.input.buttons ||
            back.input.left->x != x || back.input.left->y != -x || back.input.right->x != 0 ||
            back.input.right->y != x) {
            CAPTURE(x, text.dump());
            FAIL("recorded step does not parse back");
        }
    }
}

TEST_CASE("Debug input recordings replay the input the game was given at each present",
          "[core][nx2_debug_server]") {
    ScopedSystemTestContext context;
    Core::System system;
    auto& controller = *system.HIDCore().GetEmulatedController(Core::HID::NpadIdType::Player1);
    controller.SetNpadStyleIndex(Core::HID::NpadStyleIndex::Fullkey);
    controller.Connect();
    const auto old_count = Core::g_nx2_present_count.load();
    SCOPE_EXIT {
        if (Core::g_nx2_present_hook.load() != nullptr) {
            static_cast<void>(Core::Nx2Debug::CallTool(system, "input_record", {{"action", "stop"}}));
        }
        Core::g_nx2_present_count = old_count;
        for (auto& timestamp : Core::g_nx2_present_times_ns) {
            timestamp = 0;
        }
    };
    const auto record = [&system](std::string_view action) {
        return Core::Nx2Debug::CallTool(system, "input_record", {{"action", action}});
    };
    REQUIRE_THROWS(record("stop"));
    const json idle_status = record("status");
    REQUIRE(idle_status["recording"] == false);
    REQUIRE(idle_status["controllers"][0] ==
            json{{"npad", "player1"}, {"connected", true}, {"style", 3}, {"input", false}});
    REQUIRE(idle_status["controllers"][1]["npad"] == "handheld");
    REQUIRE_THROWS(record("pause"));

    FakePresenter presenter;
    FakeNpad npad{controller};
    // The game's input at each present of steps; a step without a stick leaves the owner's
    // stick, which is centred here.
    const auto per_present = [](const json& steps) {
        std::vector<Core::HID::InjectedInput> inputs;
        for (const json& step : steps) {
            auto parsed = Core::Nx2Debug::ParseInputStep(step);
            parsed.input.left = parsed.input.left.value_or(Core::HID::AnalogStickState{});
            parsed.input.right = parsed.input.right.value_or(Core::HID::AnalogStickState{});
            inputs.insert(inputs.end(), parsed.frames, parsed.input);
        }
        return inputs;
    };
    const auto same = [](const Core::HID::InjectedInput& a, const Core::HID::InjectedInput& b) {
        return a.buttons == b.buttons && a.left->x == b.left->x && a.left->y == b.left->y &&
               a.right->x == b.right->x && a.right->y == b.right->y;
    };
    // Plays steps while recording. The recording holds the steps' input from the replay's first
    // present on, and the owner's idle pad before and after.
    const auto check_round_trip = [&](const json& steps) {
        json started;
        json played;
        // Measured input refuses when a present lands while it installs its injector, which
        // would give its first present the owner's input. Record again in that case.
        for (int attempt = 1; played.is_null(); ++attempt) {
            started = record("start");
            REQUIRE(started["recording"] == true);
            REQUIRE(started["controller"] == "player1");
            REQUIRE_THROWS(record("start"));
            try {
                played = Core::Nx2Debug::CallTool(system, "input_sequence",
                                                  {{"steps", steps}, {"measure", true}});
            } catch (const std::runtime_error& error) {
                static_cast<void>(record("stop"));
                if (attempt == 5 || !std::string_view{error.what()}.starts_with("present advanced")) {
                    throw;
                }
            }
        }
        REQUIRE(record("status")["recording"] == true);
        const json recorded = record("stop");
        REQUIRE(Core::g_nx2_present_hook.load() == nullptr);
        REQUIRE(recorded["truncated"] == false);
        REQUIRE(recorded["controller"] == "player1");

        const auto wanted = per_present(steps);
        const auto got = per_present(recorded["steps"]);
        const u64 start = recorded["start_present"].get<u64>();
        const u64 first = played["first_present"].get<u64>();
        REQUIRE(start == started["start_present"].get<u64>());
        REQUIRE(got.size() == recorded["presents"].get<u64>());
        REQUIRE(recorded["end_present"].get<u64>() == start + got.size() - 1);
        REQUIRE(first >= start);
        REQUIRE(first - start + wanted.size() <= got.size());
        const Core::HID::InjectedInput idle{.buttons = {}, .left = Core::HID::AnalogStickState{},
                                            .right = Core::HID::AnalogStickState{}};
        for (u64 index = 0; index < got.size(); ++index) {
            const u64 offset = index - (first - start);
            const bool in_steps = index >= first - start && offset < wanted.size();
            if (!same(got[index], in_steps ? wanted[offset] : idle)) {
                CAPTURE(index, start, first);
                FAIL("recorded input differs at a present");
            }
        }
        return recorded;
    };
    const json script = {
        {{"buttons", {"A"}}, {"frames", 3}},
        {{"left_stick", {1.0, -0.5}}, {"frames", 2}},
        {{"buttons", {"B", "ZR"}},
         {"left_stick", {-0.25, 0.75}},
         {"right_stick", {0.5, 0.5}},
         {"frames", 4}},
        {{"frames", 1}},
        {{"buttons", {"Up"}}, {"right_stick", {-1.0, 0.3}}, {"frames", 2}},
    };
    const json first = check_round_trip(script);
    // Replaying the recording gives the game the same input at the same present offsets.
    static_cast<void>(check_round_trip(first["steps"]));

    // start_present puts step 0 at exactly that present, and the reply says where it landed.
    const auto refused = [&](const json& params) -> std::string {
        try {
            static_cast<void>(Core::Nx2Debug::CallTool(system, "input_sequence", params));
        } catch (const std::runtime_error& error) {
            return error.what();
        }
        return {};
    };
    const u64 passed = Core::g_nx2_present_count.load();
    REQUIRE(refused({{"steps", script}, {"start_present", passed}}).starts_with("start_present"));
    REQUIRE(refused({{"steps", {{{"ms", 5}}}}, {"start_present", passed + 100}})
                .starts_with("start_present needs frame steps"));
    const u64 recording_start = record("start")["start_present"].get<u64>();
    const u64 target = Core::g_nx2_present_count.load() + 50;
    const json aimed = Core::Nx2Debug::CallTool(
        system, "input_sequence", {{"steps", script}, {"start_present", target}, {"measure", true}});
    const json aimed_recording = record("stop");
    const auto wanted = per_present(script);
    REQUIRE(aimed["first_present"].get<u64>() == target);
    REQUIRE(aimed["landed_present"].get<u64>() == target);
    REQUIRE(aimed["measurement"]["first_present"].get<u64>() == target);
    REQUIRE(aimed["measurement"]["last_present"].get<u64>() == target + wanted.size() - 1);
    const auto aimed_inputs = per_present(aimed_recording["steps"]);
    const u64 offset = target - recording_start;
    REQUIRE(offset + wanted.size() <= aimed_inputs.size());
    REQUIRE(aimed_inputs[offset - 1].buttons == Core::HID::NpadButton::None);
    for (u64 index = 0; index < wanted.size(); ++index) {
        if (!same(aimed_inputs[offset + index], wanted[index])) {
            CAPTURE(index, target, recording_start);
            FAIL("input aimed at start_present differs at a present");
        }
    }

    // The owner's pad may drive the handheld npad while player 1 is connected and idle: the game
    // reads both, so the recording holds both.
    auto& handheld = *system.HIDCore().GetEmulatedController(Core::HID::NpadIdType::Handheld);
    handheld.SetNpadStyleIndex(Core::HID::NpadStyleIndex::Handheld);
    handheld.Connect();
    handheld.SetInputInjector([] {
        return std::optional{Core::HID::InjectedInput{
            .buttons = Core::HID::NpadButton::Y,
            .left = Core::HID::AnalogStickState{.x = 1000, .y = -2000},
            .right = {}}};
    });
    SCOPE_EXIT {
        handheld.SetInputInjector({});
        handheld.Disconnect();
    };
    REQUIRE(record("start")["controller"] == "player1");
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    json status;
    do {
        std::this_thread::sleep_for(5ms);
        status = record("status");
    } while (status["presents"].get<u64>() < 5 && std::chrono::steady_clock::now() < deadline);
    REQUIRE(status["controllers"][0]["input"] == false);
    REQUIRE(status["controllers"][1]["input"] == true);
    const json both = record("stop");
    REQUIRE(both["presents"].get<u64>() >= 5);
    for (const auto& input : per_present(both["steps"])) {
        REQUIRE(input.buttons == Core::HID::NpadButton::Y);
        REQUIRE(input.left->x == 1000);
        REQUIRE(input.left->y == -2000);
    }
}

TEST_CASE("Input recordings hold presses shorter than a present unless the mode is sample",
          "[core][nx2_debug_server]") {
    ScopedSystemTestContext context;
    Core::System system;
    auto& controller = *system.HIDCore().GetEmulatedController(Core::HID::NpadIdType::Player1);
    controller.SetNpadStyleIndex(Core::HID::NpadStyleIndex::Fullkey);
    controller.Connect();
    bool pressed = false;
    controller.SetInputInjector([&pressed] {
        return std::optional{Core::HID::InjectedInput{
            .buttons = pressed ? Core::HID::NpadButton::Y : Core::HID::NpadButton::None,
            .left = {},
            .right = {}}};
    });
    const auto old_count = Core::g_nx2_present_count.load();
    SCOPE_EXIT {
        if (Core::g_nx2_present_hook.load() != nullptr) {
            static_cast<void>(Core::Nx2Debug::CallTool(system, "input_record", {{"action", "stop"}}));
        }
        controller.SetInputInjector({});
        Core::g_nx2_present_count = old_count;
    };
    // Records one run: per count, the button state at its present and at each npad read after
    // it. Returns whether Y is in the recording at each present.
    const auto record = [&](const std::vector<std::vector<bool>>& counts) {
        const json started =
            Core::Nx2Debug::CallTool(system, "input_record", {{"action", "start"}});
        REQUIRE(started["mode"] == "any");
        for (const auto& reads : counts) {
            pressed = reads.front();
            const u64 present = ++Core::g_nx2_present_count;
            if (const auto hook = Core::g_nx2_present_hook.load()) {
                hook(present);
            }
            for (size_t index = 1; index < reads.size(); ++index) {
                pressed = reads[index];
                if (const auto hook = Core::HID::g_npad_read_hook.load()) {
                    hook(controller, controller.GetNpadButtons().raw, controller.GetSticks());
                }
            }
        }
        const json stopped =
            Core::Nx2Debug::CallTool(system, "input_record", {{"action", "stop"}});
        REQUIRE(stopped["mode"] == "any");
        REQUIRE(Core::HID::g_npad_read_hook.load() == nullptr);
        std::vector<bool> held;
        for (const json& step : stopped["steps"]) {
            const auto parsed = Core::Nx2Debug::ParseInputStep(step);
            held.insert(held.end(), parsed.frames,
                        parsed.input.buttons == Core::HID::NpadButton::Y);
        }
        REQUIRE(held.size() == stopped["presents"].get<u64>());
        REQUIRE(held.size() == counts.size());
        return held;
    };
    // A tap that starts and ends inside the second count.
    const std::vector<std::vector<bool>> tap{{false}, {false, true, false}, {false}, {false}};
    REQUIRE(record(tap) == std::vector<bool>{false, true, false, false});
    // A press from the middle of the second count to the middle of the fifth: the count it
    // began in has it too.
    const std::vector<std::vector<bool>> hold{{false}, {false, true}, {true}, {true},
                                              {true, false}, {false}};
    REQUIRE(record(hold) == std::vector<bool>{false, true, true, true, true, false});
    // Stop closes the count that is still running, with what it had so far.
    REQUIRE(record({{false}, {false, true}}) == std::vector<bool>{false, true});
    REQUIRE_THROWS_WITH(Core::Nx2Debug::CallTool(system, "input_record",
                                                 {{"action", "start"}, {"mode", "sample"}}),
                        Catch::Matchers::ContainsSubstring("mode sample was removed"));
    REQUIRE_THROWS_WITH(Core::Nx2Debug::CallTool(system, "input_record",
                                                 {{"action", "start"}, {"npad_sync", false}}),
                        Catch::Matchers::ContainsSubstring("unknown key"));
}

TEST_CASE("Luma watch keeps one sample per composite until it is done", "[core][nx2_debug_server]") {
    using VideoCore::Nx2Debug::MeanLuma;
    SCOPE_EXIT {
        VideoCore::Nx2Debug::ArmLumaWatch(0);
    };
    // BGRA8: black, white, then pure red (Rec. 601 weight 77/256 of 255).
    REQUIRE(MeanLuma(std::vector<u8>{0, 0, 0, 255}) == 0.0f);
    REQUIRE(MeanLuma(std::vector<u8>{255, 255, 255, 255}) == 255.0f);
    REQUIRE(MeanLuma(std::vector<u8>{0, 0, 255, 255, 0, 0, 255, 255}) ==
            static_cast<float>(77.0 * 255.0 / 256.0));
    REQUIRE(MeanLuma({}) == 0.0f);

    const std::vector<u8> black(64, 0);
    const std::vector<u8> white(64, 255);
    REQUIRE_FALSE(VideoCore::Nx2Debug::LumaWatchArmed());
    VideoCore::Nx2Debug::AddLumaSample(1, white);
    REQUIRE(VideoCore::Nx2Debug::ReadLumaWatch().first.empty());
    VideoCore::Nx2Debug::ArmLumaWatch(3);
    REQUIRE(VideoCore::Nx2Debug::LumaWatchArmed());
    VideoCore::Nx2Debug::AddLumaSample(10, black);
    VideoCore::Nx2Debug::AddLumaSample(11, black);
    VideoCore::Nx2Debug::AddLumaSample(12, white);
    VideoCore::Nx2Debug::AddLumaSample(13, white);
    REQUIRE_FALSE(VideoCore::Nx2Debug::LumaWatchArmed());
    const auto [samples, remaining] = VideoCore::Nx2Debug::ReadLumaWatch();
    REQUIRE(remaining == 0);
    REQUIRE(samples.size() == 3);
    REQUIRE(samples[0].present == 10);
    REQUIRE(samples[1].luma == 0.0f);
    REQUIRE(samples[2].present == 12);
    REQUIRE(samples[2].luma == 255.0f);

    // The method: arm, read while nothing composites (no renderer here), cancel.
    ScopedSystemTestContext context;
    Core::System system;
    const auto call = [&system](const json& params) {
        return Core::Nx2Debug::CallTool(system, "luma_watch", params);
    };
    REQUIRE(call({{"presents", 2}})["armed"] == 2);
    const json pending = call(json::object());
    REQUIRE(pending["done"] == false);
    REQUIRE(pending["remaining"] == 2);
    REQUIRE(pending["samples"].empty());
    VideoCore::Nx2Debug::AddLumaSample(20, white);
    VideoCore::Nx2Debug::AddLumaSample(21, black);
    REQUIRE(call({{"wait_s", 1}}) ==
            json{{"samples", {{20, 255.0}, {21, 0.0}}}, {"remaining", 0}, {"done", true}});
    REQUIRE(call({{"presents", 5}})["armed"] == 5);
    REQUIRE(call({{"cancel", true}})["armed"] == 0);
    REQUIRE_FALSE(VideoCore::Nx2Debug::LumaWatchArmed());
    REQUIRE(call({{"presents", 3600}})["armed"] == 3600);
    REQUIRE_THROWS(call({{"presents", 0}}));
    REQUIRE_THROWS(call({{"presents", -1}}));
    REQUIRE_THROWS(call({{"presents", 3601}}));
    REQUIRE_THROWS(call({{"presents", 2.5}}));
    REQUIRE_THROWS(call({{"frames", 2}}));
}

TEST_CASE("Debug server features name what this build supports", "[core][nx2_debug_server]") {
    ScopedSystemTestContext context;
    Core::System system;
    REQUIRE(Core::Nx2Debug::CallTool(system, "features", json::object()) ==
            json{{"features",
                  {"input_ms", "measure", "input_record", "record_mode", "start_present",
                   "landed_present", "screenshot_present", "luma_watch", "npad_sync", "pause",
                   "step_frames", "time_control", "time_trace", "switches", "record_anchor",
                   "pre_roll", "capture_frames", "scan_memory", "watch", "notify"}},
                 {"kit", 3},
                 {"kit_hooks", true}});
}

namespace {
std::vector<u64> g_traced_presents;
void CountTrace(u64 present) {
    g_traced_presents.push_back(present);
}
} // Anonymous namespace

TEST_CASE("A recording and the step trace share the present reply hook",
          "[core][nx2_debug_server]") {
    ScopedSystemTestContext context;
    Core::System system;
    auto& controller = *system.HIDCore().GetEmulatedController(Core::HID::NpadIdType::Player1);
    controller.SetNpadStyleIndex(Core::HID::NpadStyleIndex::Fullkey);
    controller.Connect();
    const auto old_count = Core::g_nx2_present_count.load();
    SCOPE_EXIT {
        Core::Nx2Debug::SetPresentReplyTrace(nullptr);
        if (Core::g_nx2_present_hook.load() != nullptr) {
            static_cast<void>(Core::Nx2Debug::CallTool(system, "input_record", {{"action", "stop"}}));
        }
        Core::g_nx2_present_count = old_count;
    };
    g_traced_presents.clear();
    const auto present = [] {
        const u64 count = ++Core::g_nx2_present_count;
        if (const auto hook = Core::g_nx2_present_hook.load()) {
            hook(count);
        }
        if (const auto hook = Core::g_nx2_present_reply_hook.load()) {
            hook(count);
        }
        return count;
    };
    // The trace first, then a recording: both run at each reply.
    Core::Nx2Debug::SetPresentReplyTrace(CountTrace);
    REQUIRE(Core::g_nx2_present_reply_hook.load() != nullptr);
    static_cast<void>(Core::Nx2Debug::CallTool(system, "input_record", {{"action", "start"}}));
    const u64 first = present();
    const u64 second = present();
    // The recording's stop keeps the hook for the trace.
    REQUIRE(Core::Nx2Debug::CallTool(system, "input_record", {{"action", "stop"}})["presents"] == 2);
    REQUIRE(Core::g_nx2_present_reply_hook.load() != nullptr);
    const u64 third = present();
    // A recording started during the trace, and the trace ending first: the recording goes on.
    static_cast<void>(Core::Nx2Debug::CallTool(system, "input_record", {{"action", "start"}}));
    Core::Nx2Debug::SetPresentReplyTrace(nullptr);
    REQUIRE(Core::g_nx2_present_reply_hook.load() != nullptr);
    present();
    REQUIRE(Core::Nx2Debug::CallTool(system, "input_record", {{"action", "stop"}})["presents"] == 1);
    REQUIRE(Core::g_nx2_present_reply_hook.load() == nullptr);
    REQUIRE(g_traced_presents == std::vector<u64>{first, second, third});
}

TEST_CASE("Input recordings take the stick at the present's reply", "[core][nx2_debug_server]") {
    ScopedSystemTestContext context;
    Core::System system;
    auto& controller = *system.HIDCore().GetEmulatedController(Core::HID::NpadIdType::Player1);
    controller.SetNpadStyleIndex(Core::HID::NpadStyleIndex::Fullkey);
    controller.Connect();
    s32 stick_x = 0;
    controller.SetInputInjector([&stick_x] {
        return std::optional{Core::HID::InjectedInput{
            .buttons = {}, .left = Core::HID::AnalogStickState{.x = stick_x, .y = 0}, .right = {}}};
    });
    const auto old_count = Core::g_nx2_present_count.load();
    SCOPE_EXIT {
        if (Core::g_nx2_present_hook.load() != nullptr) {
            static_cast<void>(Core::Nx2Debug::CallTool(system, "input_record", {{"action", "stop"}}));
        }
        controller.SetInputInjector({});
        Core::g_nx2_present_count = old_count;
    };
    REQUIRE(Core::Nx2Debug::CallTool(system, "input_record", {{"action", "start"}})["mode"] ==
            "any");
    // Per count: the stick when the present is counted, and when its reply goes back.
    const std::vector<std::pair<s32, s32>> counts{{1000, 2000}, {3000, 4000}, {5000, 6000}};
    for (const auto& [at_count, at_reply] : counts) {
        stick_x = at_count;
        const u64 present = ++Core::g_nx2_present_count;
        Core::g_nx2_present_hook.load()(present);
        stick_x = at_reply;
        Core::g_nx2_present_reply_hook.load()(present);
    }
    // The last count's present has no reply yet: it keeps the stick of its start.
    stick_x = 7000;
    Core::g_nx2_present_hook.load()(++Core::g_nx2_present_count);
    const json stopped = Core::Nx2Debug::CallTool(system, "input_record", {{"action", "stop"}});
    REQUIRE(Core::g_nx2_present_reply_hook.load() == nullptr);
    const auto sticks = [](const json& steps) {
        std::vector<s32> x;
        for (const json& step : steps) {
            const auto parsed = Core::Nx2Debug::ParseInputStep(step);
            x.insert(x.end(), parsed.frames, parsed.input.left->x);
        }
        return x;
    };
    REQUIRE(sticks(stopped["steps"]) == std::vector<s32>{2000, 4000, 6000, 7000});
    REQUIRE(sticks(stopped["steps_at_count"]) == std::vector<s32>{1000, 3000, 5000, 7000});
    REQUIRE(stopped["presents"] == 4);
    REQUIRE(stopped["npad_sync"] == true);
    REQUIRE(stopped["blocked_us"]["presents"] == 3);
    REQUIRE(stopped["blocked_us"]["max"].get<u64>() >= stopped["blocked_us"]["mean"].get<u64>());
}

TEST_CASE("A frame-step replay holds the present reply hook only while it runs",
          "[core][nx2_debug_server]") {
    ScopedSystemTestContext context;
    Core::System system;
    auto& controller = *system.HIDCore().GetEmulatedController(Core::HID::NpadIdType::Player1);
    controller.SetNpadStyleIndex(Core::HID::NpadStyleIndex::Fullkey);
    controller.Connect();
    const auto old_count = Core::g_nx2_present_count.load();
    SCOPE_EXIT {
        Core::g_nx2_present_count = old_count;
        for (auto& timestamp : Core::g_nx2_present_times_ns) {
            timestamp = 0;
        }
    };
    const u64 target = Core::g_nx2_present_count.load() + 20;
    // Presents every 2 ms.
    std::atomic<bool> stop{};
    std::thread presenter{[&] {
        while (!stop) {
            const u64 present = ++Core::g_nx2_present_count;
            const u64 now = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                 std::chrono::steady_clock::now().time_since_epoch())
                                                 .count());
            Core::g_nx2_present_times_ns[present % Core::g_nx2_present_times_ns.size()] = now;
            if (const auto hook = Core::g_nx2_present_hook.load()) {
                hook(present);
            }
            if (const auto reply = Core::g_nx2_present_reply_hook.load()) {
                reply(present);
            }
            std::this_thread::sleep_for(2ms);
        }
    }};
    SCOPE_EXIT {
        stop = true;
        presenter.join();
    };
    const json played = Core::Nx2Debug::CallTool(
        system, "input_sequence",
        {{"steps", {{{"buttons", {"A"}}, {"frames", 3}}, {{"frames", 3}}}}, {"start_present", target}});
    REQUIRE(played["landed_present"].get<u64>() == target);
    REQUIRE_FALSE(played.contains("npad_sync"));
    REQUIRE_THROWS_WITH(Core::Nx2Debug::CallTool(system, "input_sequence",
                                                 {{"steps", {{{"frames", 1}}}}, {"npad_sync", false}}),
                        Catch::Matchers::ContainsSubstring("npad_sync was removed"));
    REQUIRE(Core::g_nx2_present_reply_hook.load() == nullptr);
}

TEST_CASE("Kit stall breadcrumbs name the waiter, the lock and its holder",
          "[core][nx2_debug_server]") {
    std::atomic<int> ready{};
    std::atomic<bool> release{};
    const auto park = [&](bool holds, const char* op) {
        return std::thread{[&, holds, op] {
            Common::KitStall::Scope stall{"test lock", op};
            if (holds) {
                stall.Held();
            }
            ++ready;
            while (!release) {
                std::this_thread::sleep_for(1ms);
            }
        }};
    };
    std::thread holder = park(true, "hold op");
    std::thread waiter = park(false, "wait op");
    while (ready < 2) {
        std::this_thread::sleep_for(1ms);
    }
    std::this_thread::sleep_for(20ms);
    const auto stalls = Common::KitStall::Check(10'000'000, Common::KitStall::NowNs());
    release = true;
    holder.join();
    waiter.join();
    const auto found = std::ranges::find_if(stalls, [](const auto& stall) {
        return stall.text.find("has waited") != std::string::npos;
    });
    REQUIRE(found != stalls.end());
    CAPTURE(found->text);
    REQUIRE(found->text.find("for test lock in wait op") != std::string::npos);
    REQUIRE(found->text.find("; held by ") != std::string::npos);
    REQUIRE(found->text.find(" in hold op for ") != std::string::npos);
    // Under the limit, and after the scopes end, nothing is reported.
    REQUIRE(Common::KitStall::Check(60'000'000'000, Common::KitStall::NowNs()).empty());
}

TEST_CASE("EDEN_NX2_KIT_HOOKS=0 refuses the kit's hook features", "[core][nx2_debug_server]") {
    ScopedSystemTestContext context;
    Core::System system;
    auto& controller = *system.HIDCore().GetEmulatedController(Core::HID::NpadIdType::Player1);
    controller.SetNpadStyleIndex(Core::HID::NpadStyleIndex::Fullkey);
    controller.Connect();
    Common::KitStall::SetHooksEnabled(false);
    SCOPE_EXIT {
        Common::KitStall::SetHooksEnabled(true);
    };
    const auto refused = [&](std::string_view method, const json& params) {
        try {
            static_cast<void>(Core::Nx2Debug::CallTool(system, method, params));
        } catch (const std::runtime_error& error) {
            return std::string_view{error.what()}.find("EDEN_NX2_KIT_HOOKS=0") !=
                   std::string_view::npos;
        }
        return false;
    };
    REQUIRE(refused("input_record", {{"action", "start"}}));
    REQUIRE(refused("record_anchor", json::object()));
    REQUIRE(refused("luma_watch", {{"presents", 5}}));
    REQUIRE(refused("input_sequence", {{"steps", {{{"frames", 1}}}},
                                       {"start_present", Core::g_nx2_present_count.load() + 100}}));
    REQUIRE(refused("input_sequence",
                    {{"steps", {{{"frames", 1}}}}, {"watch", {{{"address", "0x4000"}, {"as", "u32"}}}}}));
    REQUIRE(refused("input_sequence", {{"steps", {{{"frames", 1}}}}, {"capture_frames", {0}}}));
    REQUIRE(Core::g_nx2_present_hook.load() == nullptr);
    REQUIRE(Core::g_nx2_present_reply_hook.load() == nullptr);
    REQUIRE(Core::Nx2Debug::CallTool(system, "features", json::object()) ==
            json{{"features", {"input_ms", "measure", "pause", "step_frames", "time_control", "switches",
                               "pre_roll", "scan_memory", "notify"}},
                 {"kit", 3},
                 {"kit_hooks", false}});
}

TEST_CASE("The pre-roll ring holds the input from the anchor to a recording's start",
          "[core][nx2_debug_server]") {
    ScopedSystemTestContext context;
    Core::System system;
    auto& controller = *system.HIDCore().GetEmulatedController(Core::HID::NpadIdType::Player1);
    controller.SetNpadStyleIndex(Core::HID::NpadStyleIndex::Fullkey);
    controller.Connect();
    s32 stick_x = 0;
    controller.SetInputInjector([&stick_x] {
        return std::optional{Core::HID::InjectedInput{
            .buttons = {}, .left = Core::HID::AnalogStickState{.x = stick_x, .y = 0}, .right = {}}};
    });
    const auto old_count = Core::g_nx2_present_count.load();
    const auto call = [&system](std::string_view method, const json& params) {
        return Core::Nx2Debug::CallTool(system, method, params);
    };
    SCOPE_EXIT {
        for (const auto& [method, params] : {std::pair{"input_record", json{{"action", "stop"}}},
                                             std::pair{"record_anchor", json{{"off", true}}}}) {
            try {
                static_cast<void>(call(method, params));
            } catch (const std::runtime_error&) {
            }
        }
        controller.SetInputInjector({});
        Core::g_nx2_present_count = old_count;
    };
    const auto present = [&stick_x](s32 x) {
        stick_x = x;
        const u64 count = ++Core::g_nx2_present_count;
        if (const auto hook = Core::g_nx2_present_hook.load()) {
            hook(count);
        }
        if (const auto hook = Core::g_nx2_present_reply_hook.load()) {
            hook(count);
        }
        return count;
    };
    const auto sticks = [](const json& steps) {
        std::vector<s32> x;
        for (const json& step : steps) {
            const auto parsed = Core::Nx2Debug::ParseInputStep(step);
            x.insert(x.end(), parsed.frames, parsed.input.left->x);
        }
        return x;
    };
    // Armed before the load's anchor is known, as the harness does at the load.
    const json armed = call("record_anchor", json::object());
    REQUIRE(armed["armed"] == true);
    REQUIRE(armed["anchor_present"].is_null());
    REQUIRE(Core::g_nx2_present_hook.load() != nullptr);
    present(100);
    const u64 anchor = present(100);
    const json marked = call("record_anchor", {{"present", anchor}});
    REQUIRE(marked["anchor_present"] == anchor);
    REQUIRE(marked["first_present"] == anchor + 1);
    REQUIRE(marked["lost_presents"] == 0);
    for (const s32 x : {0, 0, 0, 500, 500}) {
        present(x);
    }
    const u64 start = call("input_record", {{"action", "start"}})["start_present"].get<u64>();
    REQUIRE(start == anchor + 6);
    REQUIRE_THROWS_WITH(call("record_anchor", json::object()),
                        Catch::Matchers::StartsWith("a recording runs"));
    present(700);
    present(700);
    const json stopped = call("input_record", {{"action", "stop"}});
    REQUIRE(sticks(stopped["steps"]) == std::vector<s32>{700, 700});
    const json& pre_roll = stopped["pre_roll"];
    REQUIRE(pre_roll["anchor_present"] == anchor);
    REQUIRE(pre_roll["first_present"] == anchor + 1);
    REQUIRE(pre_roll["presents"] == 5);
    REQUIRE(pre_roll["lost_presents"] == 0);
    REQUIRE(pre_roll["input_presents"] == 2);
    REQUIRE(sticks(pre_roll["steps"]) == std::vector<s32>{0, 0, 0, 500, 500});

    // The ring goes on after the stop, so a second clip of this load also gets its pre-roll.
    REQUIRE(Core::g_nx2_present_hook.load() != nullptr);
    const json status = call("input_record", {{"action", "status"}});
    REQUIRE(status["pre_roll"]["presents"] == 7);
    REQUIRE_THROWS_WITH(call("input_record", {{"action", "start"}, {"mode", "sample"}}),
                        Catch::Matchers::ContainsSubstring("mode sample was removed"));
    REQUIRE(call("record_anchor", {{"off", true}})["armed"] == false);
    REQUIRE(Core::g_nx2_present_hook.load() == nullptr);
    REQUIRE(Core::g_nx2_present_reply_hook.load() == nullptr);
    REQUIRE(Core::HID::g_npad_read_hook.load() == nullptr);
    // A recording with no ring has no pre_roll, as before.
    static_cast<void>(call("input_record", {{"action", "start"}}));
    present(1);
    REQUIRE_FALSE(call("input_record", {{"action", "stop"}}).contains("pre_roll"));

    // An anchor marked while the ring is off: the presents since the anchor are lost, and the
    // reply says how many.
    const u64 late = present(0);
    present(0);
    const u64 now = present(0);
    const json lost = call("record_anchor", {{"present", late}});
    REQUIRE(lost["first_present"] == now + 1);
    REQUIRE(lost["lost_presents"] == now - late);
}

TEST_CASE("Input sequences play a pre-roll before the clip", "[core][nx2_debug_server]") {
    ScopedSystemTestContext context;
    Core::System system;
    auto& controller = *system.HIDCore().GetEmulatedController(Core::HID::NpadIdType::Player1);
    controller.SetNpadStyleIndex(Core::HID::NpadStyleIndex::Fullkey);
    controller.Connect();
    const auto old_count = Core::g_nx2_present_count.load();
    SCOPE_EXIT {
        if (Core::g_nx2_present_hook.load() != nullptr) {
            static_cast<void>(Core::Nx2Debug::CallTool(system, "input_record", {{"action", "stop"}}));
        }
        Core::g_nx2_present_count = old_count;
        for (auto& timestamp : Core::g_nx2_present_times_ns) {
            timestamp = 0;
        }
    };
    const auto call = [&system](std::string_view method, const json& params) {
        return Core::Nx2Debug::CallTool(system, method, params);
    };
    const json pre_roll = {{{"frames", 3}}, {{"left_stick", {0.5, 0.0}}, {"frames", 2}}};
    const json clip = {{{"buttons", {"A"}}, {"frames", 2}}};
    REQUIRE_THROWS_WITH(call("input_sequence", {{"pre_roll", {{{"ms", 5}}}}, {"steps", clip}}),
                        Catch::Matchers::ContainsSubstring("frame steps"));
    FakePresenter presenter;
    FakeNpad npad{controller};
    const u64 recording_start = call("input_record", {{"action", "start"}})["start_present"].get<u64>();
    const u64 target = Core::g_nx2_present_count.load() + 50;
    const json played = call("input_sequence", {{"pre_roll", pre_roll},
                                                {"steps", clip},
                                                {"start_present", target},
                                                {"measure", true}});
    const json recorded = call("input_record", {{"action", "stop"}});
    REQUIRE(played["landed_present"] == target);
    REQUIRE(played["frames"] == 7);
    REQUIRE(played["pre_roll_frames"] == 5);
    REQUIRE(played["clip_present"] == target + 5);
    // The measurement covers the clip, not the pre-roll.
    REQUIRE(played["measurement"]["first_present"] == target + 5);
    REQUIRE(played["measurement"]["last_present"] == target + 6);
    std::vector<Core::HID::InjectedInput> inputs;
    for (const json& step : recorded["steps"]) {
        const auto parsed = Core::Nx2Debug::ParseInputStep(step);
        inputs.insert(inputs.end(), parsed.frames, parsed.input);
    }
    const u64 offset = target - recording_start;
    REQUIRE(offset + 7 <= inputs.size());
    REQUIRE(inputs[offset + 2].left->x == 0);
    REQUIRE(inputs[offset + 3].left->x > Core::HID::HID_JOYSTICK_MAX / 3);
    REQUIRE(inputs[offset + 4].buttons == Core::HID::NpadButton::None);
    REQUIRE(inputs[offset + 5].buttons == Core::HID::NpadButton::A);
    REQUIRE(inputs[offset + 6].buttons == Core::HID::NpadButton::A);
}

TEST_CASE("A clip capture is requested once the composite before its target has begun",
          "[core][nx2_debug_server]") {
    // The renderer leaves the request for the target's composite (screenshot_first_present).
    STATIC_REQUIRE_FALSE(Core::Nx2Debug::CaptureDue(6912, 6914));
    STATIC_REQUIRE(Core::Nx2Debug::CaptureDue(6913, 6914));
    STATIC_REQUIRE(Core::Nx2Debug::CaptureDue(6914, 6914));
    STATIC_REQUIRE(Core::Nx2Debug::CaptureDue(6915, 6914));
}

TEST_CASE("A capture without a file says why", "[core][nx2_debug_server]") {
    using Core::Nx2Debug::CaptureError;
    REQUIRE(CaptureError(false, false).starts_with("not requested"));
    REQUIRE(CaptureError(true, true) == "the file could not be written");
    REQUIRE(CaptureError(true, false) == "the screenshot did not finish in time");
}

TEST_CASE("Clip captures name distinct clip frames, at most 32", "[core][nx2_debug_server]") {
    ScopedSystemTestContext context;
    Core::System system;
    auto& controller = *system.HIDCore().GetEmulatedController(Core::HID::NpadIdType::Player1);
    controller.SetNpadStyleIndex(Core::HID::NpadStyleIndex::Fullkey);
    controller.Connect();
    const auto refusal = [&system](const json& params) -> std::string {
        try {
            static_cast<void>(Core::Nx2Debug::CallTool(system, "input_sequence", params));
        } catch (const std::runtime_error& error) {
            return error.what();
        }
        return {};
    };
    const json clip = {{{"frames", 10}}};
    json many = json::array();
    for (int frame = 0; frame < 33; ++frame) {
        many.push_back(frame);
    }
    using Catch::Matchers::ContainsSubstring;
    REQUIRE_THAT(refusal({{"steps", {{{"ms", 50}}}}, {"capture_frames", {0}}}),
                 ContainsSubstring("needs frame steps"));
    REQUIRE_THAT(refusal({{"steps", clip}, {"capture_frames", json::array()}}),
                 ContainsSubstring("1 to 32"));
    REQUIRE_THAT(refusal({{"steps", {{{"frames", 40}}}}, {"capture_frames", many}}),
                 ContainsSubstring("1 to 32"));
    REQUIRE_THAT(refusal({{"steps", clip}, {"capture_frames", {3, 3}}}), ContainsSubstring("twice"));
    REQUIRE_THAT(refusal({{"steps", clip}, {"capture_frames", {-1}}}),
                 ContainsSubstring("not a clip frame"));
    REQUIRE_THAT(refusal({{"steps", clip}, {"capture_frames", {10}}}),
                 ContainsSubstring("not a clip frame (0 to 9)"));
    // Clip frames count from the clip's step 0, after the pre-roll.
    REQUIRE_THAT(refusal({{"pre_roll", {{{"frames", 5}}}}, {"steps", clip}, {"capture_frames", {12}}}),
                 ContainsSubstring("not a clip frame (0 to 9)"));
}
