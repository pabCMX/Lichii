// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string_view>
#include <vector>

#include "common/common_types.h"

/// Time control for Ocarina of Time (docs/nx2/trainer-kit.md). With EDEN_NX2_OOT_TIME_CONTROL set
/// at boot, the game's step hook at main+0x6E6CC4 calls a routine that can scale or replace the
/// step the game gets each frame. The debug server's time_control method sets the mode through a
/// small control block in BSS. Without the variable, nothing changes.
namespace FileSys::OotTimeControl {

constexpr u64 TitleId = 0x0400486024DEC000;
constexpr std::string_view BuildId = "E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348";

constexpr size_t HookOffset = 0x6E6CC4;
/// After the add-on step routine at 0x3C46200 and the 0x3C46280 cave of Lichii PR #1152.
constexpr size_t RoutineOffset = 0x3C46300;
/// After the add-on timer state at 0x4E9FE00 (0xA0 bytes with PR #1152).
constexpr size_t BlockOffset = 0x4E9FF00;
constexpr size_t BlockSize = 16;

/// The control block: magic, mode, value (f32), reserved. The routine ignores the block until the
/// magic is there, so a zero block, or one the game wrote, leaves the step alone.
constexpr u32 Magic = 0x524E5254; // "TRNR"
enum class Mode : u32 {
    Off = 0,
    Scale = 1, ///< step * value
    Fixed = 2, ///< value / 60 s of game time per frame, in each caller's unit (value * s1 / 60)
};

/// Whether EDEN_NX2_OOT_TIME_CONTROL is set.
[[nodiscard]] bool Requested();

/// Called by PatchManager::PatchNSO after the built-in patches. original and out are the NSO
/// header followed by the module image, before and after patching. Installs the hook and the
/// routine into out when enabled and the build, original bytes and layout match; out keeps the
/// add-on's call only as the hook's current word. Returns whether it installed them.
bool Apply(bool enabled, const std::vector<u8>& original, std::vector<u8>& out, u64 title_id,
           std::string_view module, std::string_view build_id);

/// Called by the NSO loader when it maps a module into the process. For "main", records where
/// the control block is if the last Apply for it installed the routine, else clears it.
void Publish(std::string_view module, u64 module_address);

/// The guest address of the control block of the running game, or 0 when it is not installed.
[[nodiscard]] u64 BlockAddress();

} // namespace FileSys::OotTimeControl
