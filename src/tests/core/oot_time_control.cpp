// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <bit>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "common/common_funcs.h"
#include "core/core.h"
#include "core/debugger/nx2_debug_tools.h"
#include "core/file_sys/oot_time_control.h"
#include "core/loader/nso.h"
#include "dynarmic/interface/A64/a64.h"
#include "tests/core/system_test_context.h"

namespace {

namespace TC = FileSys::OotTimeControl;

constexpr size_t Header = sizeof(Loader::NSOHeader);
constexpr u32 Original = 0x1E201828;  // fdiv s8, s1, s0
constexpr u32 AddonCall = 0x94D57D4F; // bl 0x3C46200
constexpr u32 RoutineCall = 0x94D57D8F;
constexpr size_t AddonOffset = 0x3C46200;

/// Ocarina's main as the built-in patch tests lay it out: text ends at 0x3C461D0, read-only data
/// starts at 0x3C47000, BSS ends at 0x4EA0000.
std::vector<u8> MakeMain() {
    std::vector<u8> nso(Header + TC::BlockOffset + TC::BlockSize + TC::TraceSize);
    Loader::NSOHeader header{};
    header.magic = Common::MakeMagic('N', 'S', 'O', '0');
    header.segments[0].size = 63201744;
    header.segments[1].location = 63205376;
    header.segments[1].size = 12603520;
    header.segments[2].location = 75812864;
    header.segments[2].size = 5217152;
    header.segments[2].bss_size = 1414272;
    std::memcpy(nso.data(), &header, sizeof(header));
    std::memcpy(nso.data() + Header + TC::HookOffset, &Original, sizeof(Original));
    return nso;
}

u32 WordAt(const std::vector<u8>& nso, size_t offset) {
    u32 word;
    std::memcpy(&word, nso.data() + Header + offset, sizeof(word));
    return word;
}

void SetWord(std::vector<u8>& nso, size_t offset, u32 word) {
    std::memcpy(nso.data() + Header + offset, &word, sizeof(word));
}

bool Apply(const std::vector<u8>& original, std::vector<u8>& out, bool enabled = true,
           std::string_view build_id = TC::BuildId, std::string_view module = "main",
           u64 title_id = TC::TitleId) {
    return TC::Apply(enabled, original, out, title_id, module, build_id);
}

/// Runs the patched hook in dynarmic with the module at Base: memory is a byte map, code reads
/// come from it too, and svc #0 after the hook ends the run.
class StepEnvironment final : public Dynarmic::A64::UserCallbacks {
public:
    static constexpr u64 Base = 0x8000000;
    static constexpr u64 Stack = 0x20000000;

    Dynarmic::A64::Jit* jit{};
    std::map<u64, u8> memory;

    void Load(const std::vector<u8>& nso, size_t offset, size_t size) {
        for (size_t i = 0; i < size; ++i) {
            memory[Base + offset + i] = nso[Header + offset + i];
        }
    }
    void Write32(u64 address, u32 value) {
        MemoryWrite(address, value, 4);
    }

    std::optional<u32> MemoryReadCode(u64 address) override {
        return static_cast<u32>(MemoryRead(address, 4));
    }
    u64 MemoryRead(u64 address, size_t size) override {
        u64 value = 0;
        for (size_t i = 0; i < size; ++i) {
            const auto it = memory.find(address + i);
            value |= u64{it == memory.end() ? u8{0} : it->second} << (8 * i);
        }
        return value;
    }
    Dynarmic::A64::Vector MemoryRead128(u64) override {
        FAIL("Unexpected 128-bit read");
        return {};
    }
    void MemoryWrite(u64 address, u64 value, size_t size) override {
        for (size_t i = 0; i < size; ++i) {
            memory[address + i] = static_cast<u8>(value >> (8 * i));
        }
    }
    void MemoryWrite128(u64, Dynarmic::A64::Vector) override {
        FAIL("Unexpected 128-bit write");
    }
    void CallSVC(u32) override {
        jit->HaltExecution();
    }
    void ExceptionRaised(u64 pc, Dynarmic::A64::Exception) override {
        FAIL("Unexpected exception at " << std::hex << pc);
    }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override {
        return 1000;
    }
    u64 GetCNTPCT() override {
        return 0;
    }
};

/// The patched hook in one dynarmic JIT, run again and again as the game calls it each frame,
/// with the control block changed between runs as time_control changes it while the game runs.
class Hook {
public:
    explicit Hook(const std::vector<u8>& patched) {
        env.Load(patched, TC::HookOffset, 4);
        env.Load(patched, AddonOffset, 0x100 + TC::RoutineSize);
        env.Write32(StepEnvironment::Base + TC::HookOffset + 4, 0xD4000001); // svc #0
        Dynarmic::A64::UserConfig config;
        config.callbacks = &env;
        config.enable_cycle_counting = false;
        jit = std::make_unique<Dynarmic::A64::Jit>(config);
        env.jit = jit.get();
    }

    void Set(std::optional<TC::Mode> mode, float value = 0.0f, u32 magic = TC::Magic) {
        const u64 block = StepEnvironment::Base + TC::BlockOffset;
        env.Write32(block, mode ? magic : 0);
        env.Write32(block + 4, mode ? static_cast<u32>(*mode) : 0);
        env.Write32(block + 8, std::bit_cast<u32>(value));
    }

    void Trace(u32 magic) {
        env.Write32(StepEnvironment::Base + TC::TraceOffset, magic);
    }
    [[nodiscard]] u32 Read32(size_t module_offset) {
        return static_cast<u32>(env.MemoryRead(StepEnvironment::Base + module_offset, 4));
    }
    /// Record i of the trace: s1, s0, the game's own step, the step returned.
    [[nodiscard]] std::array<float, 4> Record(size_t i) {
        std::array<float, 4> out;
        for (size_t f = 0; f < out.size(); ++f) {
            out[f] = std::bit_cast<float>(Read32(TC::TraceOffset + 16 + i * TC::TraceRecordSize + 4 * f));
        }
        return out;
    }

    /// The step the game gets (s8) for its s1 / s0. Checks that every register the routine may not
    /// touch is unchanged, s1 included.
    float Run(float s1 = 60.0f, float s0 = 30.0f) {
        for (size_t i = 0; i < 31; ++i) {
            jit->SetRegister(i, 0x12340000 + i);
        }
        jit->SetVector(0, {std::bit_cast<u32>(s0), 0});
        jit->SetVector(1, {std::bit_cast<u32>(s1), 0});
        jit->SetSP(StepEnvironment::Stack);
        jit->SetPC(StepEnvironment::Base + TC::HookOffset);
        jit->Run();
        REQUIRE(jit->GetPC() == StepEnvironment::Base + TC::HookOffset + 8);
        REQUIRE(jit->GetSP() == StepEnvironment::Stack);
        REQUIRE(jit->GetRegister(30) == StepEnvironment::Base + TC::HookOffset + 4);
        for (size_t i = 0; i < 30; ++i) {
            if (i < 10 || i > 12) {
                REQUIRE(jit->GetRegister(i) == 0x12340000 + i);
            }
        }
        REQUIRE(std::bit_cast<float>(static_cast<u32>(jit->GetVector(1)[0])) == s1);
        return std::bit_cast<float>(static_cast<u32>(jit->GetVector(8)[0]));
    }

private:
    StepEnvironment env;
    std::unique_ptr<Dynarmic::A64::Jit> jit;
};

/// One run of the hook for s1 / s0 = 60 / 30.
float RunHook(const std::vector<u8>& patched, std::optional<TC::Mode> mode = std::nullopt,
              float value = 0.0f, u32 magic = TC::Magic) {
    Hook hook{patched};
    hook.Set(mode, value, magic);
    return hook.Run();
}

} // Anonymous namespace

TEST_CASE("Time control is off unless asked and leaves the module unchanged", "[oot_time_control]") {
    const auto nso = MakeMain();
    auto out = nso;
    REQUIRE_FALSE(Apply(nso, out, false));
    REQUIRE(out == nso);
    REQUIRE_FALSE(Apply(nso, out, true, "0123"));
    REQUIRE_FALSE(Apply(nso, out, true, TC::BuildId, "sdk"));
    REQUIRE_FALSE(Apply(nso, out, true, TC::BuildId, "main", 0x0100000000010000));
    REQUIRE(out == nso);
    TC::Publish("main", 0x8000000);
    REQUIRE(TC::BlockAddress() == 0);
}

TEST_CASE("Time control installs around the game's step or the add-on's", "[oot_time_control]") {
    const auto nso = MakeMain();
    for (const bool addon : {false, true}) {
        auto out = nso;
        if (addon) {
            SetWord(out, TC::HookOffset, AddonCall);
        }
        REQUIRE(Apply(nso, out));
        REQUIRE(WordAt(out, TC::HookOffset) == RoutineCall);
        REQUIRE(WordAt(out, TC::RoutineOffset) == 0x18FFF80B);
        REQUIRE(WordAt(out, TC::RoutineOffset + TC::RoutineSize - 12) == TC::Magic);
        REQUIRE(WordAt(out, TC::RoutineOffset + TC::RoutineSize - 4) == TC::TraceMagic);
        REQUIRE(WordAt(out, TC::BlockOffset) == 0);
        REQUIRE(WordAt(out, TC::TraceOffset) == 0);
        TC::Publish("sdk", 0x9000000);
        TC::Publish("main", 0x8000000);
        REQUIRE(TC::BlockAddress() == 0x8000000 + TC::BlockOffset);
        // The next boot without the patch clears it.
        TC::Publish("main", 0x8000000);
        REQUIRE(TC::BlockAddress() == 0);
    }
}

TEST_CASE("features gives the control block address only while time control is installed",
          "[oot_time_control]") {
    ScopedSystemTestContext context;
    Core::System system;
    const auto features = [&] {
        return Core::Nx2Debug::CallTool(system, "features", nlohmann::json::object());
    };
    // Two Publish calls clear whatever an earlier Apply in this process left pending.
    TC::Publish("main", 0);
    TC::Publish("main", 0);
    REQUIRE_FALSE(features().contains("time_control_block"));
    const auto nso = MakeMain();
    auto out = nso;
    REQUIRE(Apply(nso, out));
    TC::Publish("main", 0x80747000); // HTPC-B5: "Module main loaded at 0x80747000"
    REQUIRE(features()["time_control_block"] == "0x855e6f00");
    TC::Publish("main", 0x80747000); // the next boot without the patch
    REQUIRE_FALSE(features().contains("time_control_block"));
}

TEST_CASE("Time control refuses changed bytes and layouts", "[oot_time_control]") {
    const auto check = [](auto&& change_original, auto&& change_out) {
        auto nso = MakeMain();
        change_original(nso);
        auto out = nso;
        change_out(out);
        const auto before = out;
        REQUIRE_FALSE(Apply(nso, out));
        REQUIRE(out == before);
    };
    const auto none = [](std::vector<u8>&) {};
    // Another original instruction, or a user mod at the hook.
    check([](auto& nso) { SetWord(nso, TC::HookOffset, 0xD503201F); }, none);
    check(none, [](auto& out) { SetWord(out, TC::HookOffset, 0x94000001); });
    // Code cave or control block not zero.
    check([](auto& nso) { SetWord(nso, TC::RoutineOffset + 8, 1); }, none);
    check(none, [](auto& out) { SetWord(out, TC::RoutineOffset, 1); });
    check(none, [](auto& out) { SetWord(out, TC::BlockOffset + 12, 1); });
    // The end of the longer routine, and the step trace after the block, must be free too.
    check([](auto& nso) { SetWord(nso, TC::RoutineOffset + TC::RoutineSize - 4, 1); }, none);
    check(none, [](auto& out) { SetWord(out, TC::TraceOffset + TC::TraceSize - 4, 1); });
    // Text that reaches the cave, read-only data before its end, BSS that ends before the block.
    const auto header = [](auto&& edit) {
        return [edit](std::vector<u8>& nso) {
            Loader::NSOHeader value;
            std::memcpy(&value, nso.data(), sizeof(value));
            edit(value);
            std::memcpy(nso.data(), &value, sizeof(value));
        };
    };
    check(header([](auto& h) { h.segments[0].size = TC::RoutineOffset + 4; }), none);
    check(header([](auto& h) { h.segments[1].location = TC::RoutineOffset + 64; }), none);
    check(header([](auto& h) { h.segments[2].bss_size -= 0x200; }), none);
    check(header([](auto& h) { h.segments[2].bss_size -= 0x70; }), none); // BSS ends inside the trace
    // A truncated module.
    check([](auto& nso) { nso.resize(nso.size() - 1); }, none);
}

TEST_CASE("Time control routine scales or replaces the step, and only with its magic",
          "[oot_time_control]") {
    const auto nso = MakeMain();
    auto plain = nso;
    REQUIRE(Apply(nso, plain));
    // A stand-in add-on routine: fmov s8, #0.5 ; ret.
    auto addon = nso;
    SetWord(addon, AddonOffset, 0x1E2C1008);
    SetWord(addon, AddonOffset + 4, 0xD65F03C0);
    SetWord(addon, TC::HookOffset, AddonCall);
    REQUIRE(Apply(nso, addon));

    REQUIRE(RunHook(plain) == 2.0f);
    REQUIRE(RunHook(addon) == 0.5f);
    REQUIRE(RunHook(plain, TC::Mode::Scale, 0.25f) == 0.5f);
    REQUIRE(RunHook(addon, TC::Mode::Scale, 3.0f) == 1.5f);
    REQUIRE(RunHook(plain, TC::Mode::Off, 3.0f) == 2.0f);
    REQUIRE(RunHook(plain, static_cast<TC::Mode>(3), 3.0f) == 2.0f);
    REQUIRE(RunHook(plain, TC::Mode::Fixed, 1.0f, 0) == 2.0f);
}

TEST_CASE("Time control fixed gives every caller the same game time in its own unit",
          "[oot_time_control]") {
    // The step is in ticks of s1 Hz (s1 / s0 originally). A fixed step that ignores s1 runs a
    // 30 Hz caller and a 60 Hz caller at different relative speeds: the live D3 failure.
    const auto nso = MakeMain();
    auto plain = nso;
    REQUIRE(Apply(nso, plain));
    auto addon = nso;
    SetWord(addon, AddonOffset, 0x1E2C1008); // the add-on's step: 0.5
    SetWord(addon, AddonOffset + 4, 0xD65F03C0);
    SetWord(addon, TC::HookOffset, AddonCall);
    REQUIRE(Apply(nso, addon));
    for (const auto* patched : {&plain, &addon}) {
        Hook hook{*patched};
        hook.Set(TC::Mode::Fixed, 1.0f);
        REQUIRE_THAT(hook.Run(60.0f, 60.0f), Catch::Matchers::WithinRel(1.0f, 1e-6f));
        REQUIRE_THAT(hook.Run(30.0f, 60.0f), Catch::Matchers::WithinRel(0.5f, 1e-6f));
        hook.Set(TC::Mode::Fixed, 0.5f);
        // Half the game time per frame for both: the 60 Hz and 30 Hz callers keep their 2:1 ratio.
        REQUIRE_THAT(hook.Run(60.0f, 60.0f) / hook.Run(30.0f, 60.0f), Catch::Matchers::WithinRel(2.0f, 1e-6f));
    }
}

TEST_CASE("Time control fixed whole steps match FPS Unlock at 60, 30, 20 and 10 FPS",
          "[oot_time_control]") {
    // FPS Unlock returns seconds per frame * s1, so at F FPS a caller gets s1 / F. fixed N must give
    // every caller that step at F = 60 / N: the steps the game already takes in real play.
    const auto nso = MakeMain();
    auto plain = nso;
    REQUIRE(Apply(nso, plain));
    Hook hook{plain};
    for (const auto& [n, fps] : {std::pair{1.0f, 60.0f}, {2.0f, 30.0f}, {3.0f, 20.0f}, {6.0f, 10.0f}}) {
        hook.Set(TC::Mode::Fixed, n);
        for (const float s1 : {60.0f, 30.0f}) {
            REQUIRE_THAT(hook.Run(s1, 60.0f), Catch::Matchers::WithinRel(s1 / fps, 1e-6f));
        }
    }
}

TEST_CASE("Time control mode changes on a running hook return to the game's step",
          "[oot_time_control]") {
    const auto nso = MakeMain();
    auto plain = nso;
    REQUIRE(Apply(nso, plain));
    Hook hook{plain};
    // The game's own step with the add-ons off: s1 / s0 for each caller.
    const auto own = [&] {
        REQUIRE(hook.Run(60.0f, 60.0f) == 1.0f);
        REQUIRE(hook.Run(30.0f, 60.0f) == 0.5f);
    };
    hook.Set(std::nullopt);
    own();
    hook.Set(TC::Mode::Fixed, 0.01f);
    REQUIRE_THAT(hook.Run(60.0f, 60.0f), Catch::Matchers::WithinRel(0.01f, 1e-5f));
    hook.Set(TC::Mode::Off);
    own();
    hook.Set(TC::Mode::Scale, 0.01f);
    REQUIRE_THAT(hook.Run(30.0f, 60.0f), Catch::Matchers::WithinRel(0.005f, 1e-5f));
    hook.Set(TC::Mode::Fixed, 6.0f);
    REQUIRE_THAT(hook.Run(30.0f, 60.0f), Catch::Matchers::WithinRel(3.0f, 1e-6f));
    hook.Set(TC::Mode::Off, 6.0f);
    own();
}

TEST_CASE("Time control traces each call with its inputs, own step and result, also when off",
          "[oot_time_control]") {
    const auto nso = MakeMain();
    auto plain = nso;
    REQUIRE(Apply(nso, plain));
    Hook hook{plain};
    // Not armed: nothing is written to the trace area.
    REQUIRE(hook.Run(60.0f, 30.0f) == 2.0f);
    REQUIRE(hook.Read32(TC::TraceOffset + 4) == 0);
    REQUIRE(hook.Record(0) == std::array<float, 4>{});
    // Armed with no control block magic (time control off): the game's own step, traced.
    hook.Trace(TC::TraceMagic);
    REQUIRE(hook.Run(60.0f, 30.0f) == 2.0f);
    REQUIRE(hook.Read32(TC::TraceOffset + 4) == 1);
    REQUIRE(hook.Record(0) == std::array<float, 4>{60.0f, 30.0f, 2.0f, 2.0f});
    // With a mode, the own step and the step returned differ.
    hook.Set(TC::Mode::Scale, 0.5f);
    REQUIRE(hook.Run(30.0f, 60.0f) == 0.25f);
    REQUIRE(hook.Record(1) == std::array<float, 4>{30.0f, 60.0f, 0.5f, 0.25f});
    hook.Set(TC::Mode::Fixed, 2.0f);
    REQUIRE_THAT(hook.Run(60.0f, 60.0f), Catch::Matchers::WithinRel(2.0f, 1e-6f));
    REQUIRE(hook.Record(2)[2] == 1.0f);
    // Call i goes to record i % 8.
    for (int i = 0; i < 6; ++i) {
        hook.Run(60.0f, 30.0f);
    }
    REQUIRE(hook.Read32(TC::TraceOffset + 4) == 9);
    REQUIRE(hook.Record(0)[1] == 30.0f);
    REQUIRE(hook.Record(1) == std::array<float, 4>{30.0f, 60.0f, 0.5f, 0.25f});
    // Stopped (the off magic): the count stays.
    hook.Trace(TC::TraceOffMagic);
    hook.Run(60.0f, 30.0f);
    REQUIRE(hook.Read32(TC::TraceOffset + 4) == 9);
}
