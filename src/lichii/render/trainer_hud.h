// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>

#include <QRect>
#include <QSize>
#include <QSizeF>

#include "hid_core/hid_types.h"
#include "lichii/render/trainer_bars.h"
#include "lichii/render/trainer_input.h"

class QImage;
class QPainter;

/// The panels of the trainer overlay. Both grow with the game image by PanelScale. The input panel
/// is painted in layout units, which the overlay scales to pixels; the bars panel in whole device
/// pixels, so every bar has the same width.
/// Needs Qt Gui only, so tools/nx2/trainer_hud_preview.cpp paints the same panels into a PNG.
namespace TrainerHud {

/// What the input panel shows in one frame.
struct InputFrame {
    Core::HID::NpadButton buttons{};
    s32 left_x{};
    s32 left_y{};
    s32 right_x{};
    s32 right_y{};
    /// Per stick, the configured deadzone (0 to 1 of a full deflection), 0 when not known.
    float left_deadzone{};
    float right_deadzone{};
    /// Per stick, the position before the deadzone and range, when it gives the value above.
    std::optional<TrailPoint> left_raw;
    std::optional<TrailPoint> right_raw;
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

/// The bars panel in device pixels at unit device pixels per layout unit: the game image's
/// PanelScale times the device pixel ratio, so it grows with the image like the input panel.
struct BarsLayout {
    double unit{};
    BarPixels pixels{};
    QSize size;  ///< the whole panel
    QRect graph; ///< the bars, inside the panel
};
[[nodiscard]] BarsLayout LayoutBars(double unit);

/// Where the bars panel goes over the game image (pixels, like PanelRect): centred along the
/// bottom, or above the input panel when input is set and they do not fit side by side.
/// size is the panel's size in the same pixels.
[[nodiscard]] QRect BarsRect(const QRect& image, QSize size, const std::optional<QRect>& input);

/// The bars as an image of layout.graph's size (device pixels): one column of layout.pixels.pitch
/// per FrameBars slot, transparent where no bar is. PaintBarSlot redraws one slot.
void PaintBarSlot(QImage& graph, const BarsLayout& layout, const FrameBars& bars, size_t slot);
void PaintBarSlots(QImage& graph, const BarsLayout& layout, const FrameBars& bars);

/// Paints the bars panel with its top-left corner at the painter's origin. The painter must map
/// one unit to one device pixel. graph is the image of PaintBarSlots; first_slot is drawn at the
/// left edge. The text line shows stats: mean frame time, FPS and the worst frame.
void PaintBarsPanel(QPainter& painter, const BarsLayout& layout, const QImage& graph,
                    size_t first_slot, const BarStats& stats);

/// Paints the input panel with its top-left corner at the painter's origin: the base layer (the
/// panel and the idle shapes, which never change), then the live layer over it.
void PaintInputPanel(QPainter& painter, const InputFrame& frame);
void PaintInputBase(QPainter& painter);
void PaintInputLive(QPainter& painter, const InputFrame& frame);

} // namespace TrainerHud
