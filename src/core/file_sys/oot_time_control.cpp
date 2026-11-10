// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>

#include "common/logging.h"
#include "core/file_sys/oot_time_control.h"
#include "core/loader/nso.h"

namespace FileSys::OotTimeControl {
namespace {

constexpr u32 OriginalInstruction = 0x1E201828; // fdiv s8, s1, s0
constexpr u32 AddonCall = 0x94D57D4F;           // bl 0x3C46200 (FPS Unlock, Dynamic FPS)
constexpr u32 RoutineCall = 0x94D57D8F;         // bl 0x3C46300

// Assembled with clang --target=aarch64; offsets are module offsets. It clobbers only what the
// add-on routine already clobbers (x10 to x12, s2, s3, flags) and saves x30 on the stack around the
// nested call. docs/nx2/trainer-kit.md has the source.
// The game's step is in ticks of s1 Hz (s1 is 30 or 60; the original is s1 / s0, FPS Unlock's is
// seconds * s1), and s1 differs between callers. "scale" multiplies it, so every caller keeps its
// ratio. "fixed" gives value / 60 seconds per frame in each caller's own unit: value * s1 / 60.
// With the trace magic it records s1, s0 and the step before the mode, then the step returned,
// and only then counts the call, so a record is complete before the host can read it.
//   3C46300 ldr w11, 0x3C46200 ; cbz w11, 3C46318      ; no add-on routine: zero padding
//   3C46308 str x30, [sp, #-16]! ; bl 0x3C46200 ; ldr x30, [sp], #16 ; b 3C4631C
//   3C46318 fdiv s8, s1, s0                            ; the original instruction
//   3C4631C adrp x10, 0x4E9F000 ; add x10, x10, #0xF00 ; control block
//   3C46324 ldr w11, [x10, #16] ; ldr w12, 3C463B8 ; cmp w11, w12 ; b.ne 3C46348   ; trace?
//   3C46334 ldr w11, [x10, #20] ; and w11, w11, #7 ; add x11, x10, x11, lsl #4
//   3C46340 stp s1, s0, [x11, #32] ; str s8, [x11, #40]          ; inputs, the game's own step
//   3C46348 ldr w11, [x10] ; ldr w12, 3C463B0 ; cmp w11, w12 ; b.ne 3C46384      ; no magic: as is
//   3C46358 ldr w11, [x10, #4] ; ldr s3, [x10, #8] ; cmp w11, #1 ; b.ne 3C46370
//   3C46368 fmul s8, s8, s3 ; b 3C46384                ; scale
//   3C46370 cmp w11, #2 ; b.ne 3C46384
//   3C46378 ldr s2, 3C463B4 ; fmul s3, s3, s1 ; fmul s8, s3, s2   ; fixed: value * s1 / 60
//   3C46384 ldr w11, [x10, #16] ; ldr w12, 3C463B8 ; cmp w11, w12 ; b.ne 3C463AC   ; trace?
//   3C46394 ldr w11, [x10, #20] ; and w12, w11, #7 ; add x12, x10, x12, lsl #4
//   3C463A0 str s8, [x12, #44] ; add w11, w11, #1 ; str w11, [x10, #20]   ; the step returned, count
//   3C463AC ret ; .word Magic ; .float 1/60 ; .word TraceMagic
constexpr std::array<u8, RoutineSize> ROUTINE{
    0x0B, 0xF8, 0xFF, 0x18, 0xAB, 0x00, 0x00, 0x34, 0xFE, 0x0F, 0x1F, 0xF8, 0xBD, 0xFF, 0xFF, 0x97,
    0xFE, 0x07, 0x41, 0xF8, 0x02, 0x00, 0x00, 0x14, 0x28, 0x18, 0x20, 0x1E, 0xCA, 0x92, 0x00, 0xB0,
    0x4A, 0x01, 0x3C, 0x91, 0x4B, 0x11, 0x40, 0xB9, 0x8C, 0x04, 0x00, 0x18, 0x7F, 0x01, 0x0C, 0x6B,
    0xC1, 0x00, 0x00, 0x54, 0x4B, 0x15, 0x40, 0xB9, 0x6B, 0x09, 0x00, 0x12, 0x4B, 0x11, 0x0B, 0x8B,
    0x61, 0x01, 0x04, 0x2D, 0x68, 0x29, 0x00, 0xBD, 0x4B, 0x01, 0x40, 0xB9, 0x2C, 0x03, 0x00, 0x18,
    0x7F, 0x01, 0x0C, 0x6B, 0x81, 0x01, 0x00, 0x54, 0x4B, 0x05, 0x40, 0xB9, 0x43, 0x09, 0x40, 0xBD,
    0x7F, 0x05, 0x00, 0x71, 0x61, 0x00, 0x00, 0x54, 0x08, 0x09, 0x23, 0x1E, 0x06, 0x00, 0x00, 0x14,
    0x7F, 0x09, 0x00, 0x71, 0x81, 0x00, 0x00, 0x54, 0xE2, 0x01, 0x00, 0x1C, 0x63, 0x08, 0x21, 0x1E,
    0x68, 0x08, 0x22, 0x1E, 0x4B, 0x11, 0x40, 0xB9, 0x8C, 0x01, 0x00, 0x18, 0x7F, 0x01, 0x0C, 0x6B,
    0xE1, 0x00, 0x00, 0x54, 0x4B, 0x15, 0x40, 0xB9, 0x6C, 0x09, 0x00, 0x12, 0x4C, 0x11, 0x0C, 0x8B,
    0x88, 0x2D, 0x00, 0xBD, 0x6B, 0x05, 0x00, 0x11, 0x4B, 0x15, 0x00, 0xB9, 0xC0, 0x03, 0x5F, 0xD6,
    0x54, 0x52, 0x4E, 0x52, 0x89, 0x88, 0x88, 0x3C, 0x54, 0x52, 0x43, 0x45,
};
static_assert(TraceOffset == BlockOffset + 16 && TraceRecordSize == 16 && TraceRecords == 8,
              "the routine's offsets (#16, #20, #32 + 16 * (count & 7))");

std::atomic<bool> g_pending{};
std::atomic<u64> g_block{};

bool Fits(const std::vector<u8>& data, size_t offset, size_t size) {
    return data.size() >= sizeof(Loader::NSOHeader) &&
           offset <= data.size() - sizeof(Loader::NSOHeader) &&
           size <= data.size() - sizeof(Loader::NSOHeader) - offset;
}

u32 Word(const std::vector<u8>& data, size_t offset) {
    u32 word;
    std::memcpy(&word, data.data() + sizeof(Loader::NSOHeader) + offset, sizeof(word));
    return word;
}

bool Zero(const std::vector<u8>& data, size_t offset, size_t size) {
    const auto begin = data.begin() + sizeof(Loader::NSOHeader) + offset;
    return std::all_of(begin, begin + size, [](u8 byte) { return byte == 0; });
}

/// Why the routine cannot go in, or null. The same layout rules as the built-in patches.
const char* Refusal(const std::vector<u8>& original, const std::vector<u8>& out) {
    constexpr size_t Reserved = BlockSize + TraceSize; // the control block and the step trace
    if (!Fits(original, BlockOffset, Reserved) || !Fits(out, BlockOffset, Reserved)) {
        return "the module is too short";
    }
    Loader::NSOHeader header;
    std::memcpy(&header, original.data(), sizeof(header));
    const auto& text = header.segments[0];
    const u64 text_end = u64{text.location} + text.size;
    const u64 executable_end = (text_end + 0xFFF) & ~u64{0xFFF};
    if (RoutineOffset < text_end || RoutineOffset + ROUTINE.size() > executable_end ||
        RoutineOffset + ROUTINE.size() > header.segments[1].location ||
        !Zero(original, RoutineOffset, ROUTINE.size()) || !Zero(out, RoutineOffset, ROUTINE.size())) {
        return "its code cave is not free executable padding";
    }
    const auto& data = header.segments[2];
    const u64 bss_start = u64{data.location} + data.size;
    const u64 bss_end = bss_start + data.bss_size;
    if (BlockOffset < bss_start || BlockOffset + Reserved > bss_end ||
        !Zero(original, BlockOffset, Reserved) || !Zero(out, BlockOffset, Reserved)) {
        return "its control block is not zero-filled BSS";
    }
    if (Word(original, HookOffset) != OriginalInstruction) {
        return "unexpected original bytes at the step hook";
    }
    const u32 hook = Word(out, HookOffset);
    if (hook != OriginalInstruction && hook != AddonCall) {
        return "a user mod changed the step hook";
    }
    return nullptr;
}

} // Anonymous namespace

bool Requested() {
    return std::getenv("EDEN_NX2_OOT_TIME_CONTROL") != nullptr;
}

bool Apply(bool enabled, const std::vector<u8>& original, std::vector<u8>& out, u64 title_id,
           std::string_view module, std::string_view build_id) {
    if (module == "main") {
        g_pending.store(false, std::memory_order_relaxed);
    }
    if (!enabled || title_id != TitleId || module != "main") {
        return false;
    }
    if (build_id != BuildId) {
        LOG_INFO(Loader, "    - Skipping time control: build id {} is not {}", build_id, BuildId);
        return false;
    }
    if (const char* const refusal = Refusal(original, out)) {
        LOG_WARNING(Loader, "    - Skipping time control: {}", refusal);
        return false;
    }
    const bool addon = Word(out, HookOffset) == AddonCall;
    u8* const image = out.data() + sizeof(Loader::NSOHeader);
    std::memcpy(image + HookOffset, &RoutineCall, sizeof(RoutineCall));
    std::memcpy(image + RoutineOffset, ROUTINE.data(), ROUTINE.size());
    g_pending.store(true, std::memory_order_relaxed);
    LOG_INFO(Loader, "    - Applying time control to main ({}; control block main+{:#x})",
             addon ? "around the step add-on" : "around the game's own step", BlockOffset);
    return true;
}

void Publish(std::string_view module, u64 module_address) {
    if (module != "main") {
        return;
    }
    const bool installed = g_pending.exchange(false, std::memory_order_relaxed);
    g_block.store(installed ? module_address + BlockOffset : 0, std::memory_order_relaxed);
}

u64 BlockAddress() {
    return g_block.load(std::memory_order_relaxed);
}

} // namespace FileSys::OotTimeControl
