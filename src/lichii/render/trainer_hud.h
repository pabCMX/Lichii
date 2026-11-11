// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QRect>
#include <QSizeF>

#include "hid_core/hid_types.h"
#include "lichii/render/trainer_input.h"

class QPainter;

/// The panels of the trainer overlay, painted in layout units: the overlay scales one unit to
/// pixels for the size of the game image. Needs Qt Gui only, so tools/nx2/trainer_hud_preview.cpp
/// paints the same panels into a PNG.
namespace TrainerHud {

/// What the input panel shows in one frame.
struct InputFrame {
    Core::HID::NpadButton buttons{};
    s32 left_x{};
    s32 left_y{};
    s32 right_x{};
    s32 right_y{};
    const StickTrail* left_trail{};
    const StickTrail* right_trail{};
    s64 now_ms{}; ///< the trails' clock now
    bool recording{};
    u64 recorded{}; ///< presents recorded so far
    bool record_full{};
    bool replaying{};
    u64 replay_wait{};   ///< presents until a scheduled replay starts
    u64 pre_roll_left{}; ///< presents until the clip, while the pre-roll plays
    /// The pre-roll ring, shown only while it is armed and no recording runs.
    bool ring_armed{};
    u64 ring_kept{};
    u64 ring_lost{};
};

/// The input panel's size in layout units.
constexpr QSizeF INPUT_PANEL_SIZE{344.0, 170.0};
/// How far back the trails reach.
constexpr s64 TRAIL_MS = 500;

/// Where the input panel goes over the game image (both in pixels): bottom-left with a margin,
/// clear of Ocarina's minimap (bottom-right) and the centre. One layout unit is a pixel at 720p.
[[nodiscard]] QRect PanelRect(const QRect& image);

/// Paints the input panel with its top-left corner at the painter's origin: the base layer (the
/// panel and the idle shapes, which never change), then the live layer over it.
void PaintInputPanel(QPainter& painter, const InputFrame& frame);
void PaintInputBase(QPainter& painter);
void PaintInputLive(QPainter& painter, const InputFrame& frame);

} // namespace TrainerHud
