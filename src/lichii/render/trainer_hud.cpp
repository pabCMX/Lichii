// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>

#include <QFont>
#include <QImage>
#include <QPainter>
#include <QString>

#include "lichii/render/trainer_hud.h"

namespace TrainerHud {
namespace {

using Core::HID::NpadButton;

const QColor PANEL{0, 0, 0, 150};
const QColor IDLE_FILL{255, 255, 255, 40};
const QColor IDLE_EDGE{255, 255, 255, 120};
const QColor LIT{255, 202, 40};
const QColor LIT_TEXT{30, 30, 30};
const QColor TEXT{235, 235, 235};
const QColor TRAIL{80, 220, 255};
const QColor REC{229, 57, 53};
const QColor REPLAY{30, 136, 229};
const QColor WAIT{120, 120, 120};
const QColor PRE{21, 101, 192}; ///< a darker REPLAY: replayed input before the clip
const QColor RING{70, 70, 70};
const QColor WARNING{239, 108, 0};
const QColor DEADZONE{255, 110, 110};
const QColor RAW{255, 150, 150, 220};
const QColor BAR_GREEN{60, 214, 88};
const QColor BAR_ORANGE{255, 150, 0};
const QColor BAR_RED{255, 56, 56};
const QColor CLIP_CAP{255, 255, 255};
const QColor GRAPH_BACK{0, 0, 0, 80};
const QColor TARGET_LINE{255, 255, 255, 210};

constexpr double STICK_RADIUS = 30.0;

bool Held(const InputFrame& frame, NpadButton button) {
    return (frame.buttons & button) != NpadButton::None;
}

QFont Font(double pixels, bool bold) {
    QFont font;
    font.setPixelSize(int(pixels));
    font.setBold(bold);
    return font;
}

/// A font whose digits all have the same width, so numbers that change keep their place.
QFont NumberFont(double pixels, bool bold) {
    QFont font = Font(pixels, bold);
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    font.setFeature(QFont::Tag("tnum"), 1);
#endif
    return font;
}

/// Which layer a paint call draws: the base has everything that never changes (the panel and the
/// idle shapes), the live layer what the input changes. The overlay caches the base as an image.
enum class Layer { Base, Live };

/// A button as a shape that lights while held, with its label inside: idle in the base layer,
/// lit over it in the live layer.
void Button(QPainter& painter, Layer layer, const QRectF& rect, bool held, const QString& label,
            bool round) {
    if (layer == Layer::Live && !held) {
        return;
    }
    held = layer == Layer::Live;
    painter.setPen(QPen{held ? LIT : IDLE_EDGE, 1.2});
    painter.setBrush(held ? LIT : IDLE_FILL);
    if (round) {
        painter.drawEllipse(rect);
    } else {
        painter.drawRoundedRect(rect, 4.0, 4.0);
    }
    if (!label.isEmpty()) {
        painter.setPen(held ? LIT_TEXT : TEXT);
        painter.drawText(rect, Qt::AlignCenter, label);
    }
}

void RoundButton(QPainter& painter, Layer layer, QPointF centre, bool held, const QString& label) {
    constexpr double r = 9.0;
    Button(painter, layer, QRectF{centre.x() - r, centre.y() - r, 2 * r, 2 * r}, held, label, true);
}

QPointF StickPoint(QPointF centre, s32 x, s32 y) {
    // Screen y grows down; the npad's y grows up.
    return centre + QPointF{x / STICK_MAX * STICK_RADIUS, -y / STICK_MAX * STICK_RADIUS};
}

/// A stick as a dot in a circle, with the trail of its last positions; the circle lights while
/// the stick is clicked. The deadzone is a ring at its radius, and raw (the stick before the
/// deadzone and range) a small pink dot where it is away from the dot of what the game gets.
void Stick(QPainter& painter, Layer layer, QPointF centre, s32 x, s32 y, float deadzone,
           const std::optional<TrailPoint>& raw, const StickTrail* trail, s64 now_ms,
           bool clicked) {
    if (layer == Layer::Base) {
        painter.setPen(QPen{IDLE_EDGE, 1.2});
        painter.setBrush(QColor{0, 0, 0, 70});
        painter.drawEllipse(centre, STICK_RADIUS, STICK_RADIUS);
        painter.setPen(QPen{QColor{255, 255, 255, 45}, 1.0});
        painter.drawLine(centre - QPointF{STICK_RADIUS, 0}, centre + QPointF{STICK_RADIUS, 0});
        painter.drawLine(centre - QPointF{0, STICK_RADIUS}, centre + QPointF{0, STICK_RADIUS});
        return;
    }
    if (clicked) {
        painter.setPen(QPen{LIT, 3.0});
        painter.setBrush(QColor{255, 202, 40, 60});
        painter.drawEllipse(centre, STICK_RADIUS, STICK_RADIUS);
    }
    if (deadzone > 0.0f) {
        const double radius = std::min(double(deadzone), 1.0) * STICK_RADIUS;
        QColor fill = DEADZONE;
        fill.setAlpha(45);
        painter.setPen(QPen{DEADZONE, 1.0});
        painter.setBrush(fill);
        painter.drawEllipse(centre, radius, radius);
    }

    // Older segments fade out.
    if (trail != nullptr) {
        for (size_t i = 1; i < trail->Size(); ++i) {
            const TrailPoint& from = (*trail)[i - 1];
            const TrailPoint& to = (*trail)[i];
            const double age = double(now_ms - to.ms) / double(TRAIL_MS);
            QColor colour = TRAIL;
            colour.setAlphaF(float(std::clamp(1.0 - age, 0.0, 1.0) * 0.9));
            painter.setPen(QPen{colour, 2.0, Qt::SolidLine, Qt::RoundCap});
            painter.drawLine(StickPoint(centre, from.x, from.y), StickPoint(centre, to.x, to.y));
        }
    }
    const QPointF dot = StickPoint(centre, x, y);
    if (x == 0 && y == 0 && deadzone > 0.0f && !clicked) {
        // Hollow while the game gets 0, so the deadzone and the raw dot show through.
        painter.setPen(QPen{QColor{255, 255, 255}, 1.2});
        painter.setBrush(Qt::NoBrush);
    } else {
        painter.setPen(QPen{QColor{0, 0, 0, 160}, 1.0});
        painter.setBrush(clicked ? LIT : QColor{255, 255, 255});
    }
    painter.drawEllipse(dot, 4.0, 4.0);
    // Small and over the game's dot, so it shows inside a small deadzone too.
    if (raw) {
        const QPointF raw_dot = StickPoint(centre, raw->x, raw->y);
        if (std::hypot(raw_dot.x() - dot.x(), raw_dot.y() - dot.y()) > 1.0) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(RAW);
            painter.drawEllipse(raw_dot, 2.2, 2.2);
        }
    }
}

/// The stick's angle (hundredths of a degree) and magnitude (thousandths) as two lines, right
/// aligned in a box at top_left that holds 359.99°, so the digits keep their place.
void StickText(QPainter& painter, QPointF top_left, s32 x, s32 y) {
    const StickReading reading = ReadStick(x, y);
    const QRectF angle_rect{top_left, QSizeF{62.0, 17.0}};
    const QRectF magnitude_rect{top_left + QPointF{0, 18.0}, QSizeF{62.0, 17.0}};
    painter.setPen(TEXT);
    painter.setFont(NumberFont(13, true));
    painter.drawText(angle_rect, Qt::AlignRight | Qt::AlignVCenter,
                     reading.angle < 0 ? QStringLiteral("---")
                                       : QStringLiteral("%1°").arg(reading.angle, 0, 'f', 2));
    painter.setFont(NumberFont(13, false));
    painter.drawText(magnitude_rect, Qt::AlignRight | Qt::AlignVCenter,
                     QString::number(reading.magnitude, 'f', 3));
}

/// The REC, PRE-ROLL, REPLAY, PRE and WAIT marks.
void StateMarks(QPainter& painter, const InputFrame& frame) {
    painter.setFont(Font(12, true));
    double x = 8.0;
    const auto mark = [&](const QString& text, const QColor& colour) {
        const double width = 14.0 + painter.fontMetrics().horizontalAdvance(text);
        const QRectF rect{x, 6.0, width, 18.0};
        painter.setPen(Qt::NoPen);
        painter.setBrush(colour);
        painter.drawRoundedRect(rect, 9.0, 9.0);
        painter.setPen(Qt::white);
        painter.drawText(rect, Qt::AlignCenter, text);
        x += width + 6.0;
    };
    if (frame.recording) {
        mark(QStringLiteral("● REC %1%2")
                 .arg(frame.recorded)
                 .arg(frame.record_full ? QStringLiteral(" full") : QString{}),
             REC);
    }
    // The ring matters before Ctrl+F8: a lost present makes the replay start elsewhere.
    if (frame.ring_armed && !frame.recording) {
        if (frame.ring_lost != 0) {
            mark(QStringLiteral("PRE-ROLL LOST %1").arg(frame.ring_lost), WARNING);
        } else {
            mark(QStringLiteral("PRE-ROLL %1").arg(frame.ring_kept), RING);
        }
    }
    if (frame.replaying) {
        mark(QStringLiteral("▶ REPLAY"), REPLAY);
    }
    if (frame.pre_roll_left != 0) {
        mark(QStringLiteral("▶ PRE %1").arg(frame.pre_roll_left), PRE);
    }
    if (frame.replay_wait != 0) {
        mark(QStringLiteral("WAIT %1").arg(frame.replay_wait), WAIT);
    }
}

} // Anonymous namespace

namespace {

/// Layout units to pixels for a game image.
double PanelScale(const QRect& image) {
    return std::clamp(image.height() / 720.0, 0.75, 4.0);
}

} // Anonymous namespace

QRect PanelRect(const QRect& image) {
    const double scale = PanelScale(image);
    const double margin = 16.0 * scale;
    const QSizeF size = INPUT_PANEL_SIZE * scale;
    return QRectF{image.left() + margin, image.bottom() + 1 - margin - size.height(), size.width(),
                  size.height()}
        .toRect();
}

namespace {

void Paint(QPainter& painter, Layer layer, const InputFrame& frame) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    const bool live = layer == Layer::Live;
    if (live) {
        StateMarks(painter, frame);
    } else {
        painter.setPen(Qt::NoPen);
        painter.setBrush(PANEL);
        painter.drawRoundedRect(QRectF{QPointF{}, INPUT_PANEL_SIZE}, 10.0, 10.0);
    }

    // Shoulders, and the Joy-Con side buttons when held sideways.
    painter.setFont(Font(11, true));
    Button(painter, layer, {8, 30, 40, 16}, Held(frame, NpadButton::ZL), QStringLiteral("ZL"),
           false);
    Button(painter, layer, {52, 30, 40, 16}, Held(frame, NpadButton::L), QStringLiteral("L"),
           false);
    Button(painter, layer, {140, 30, 28, 16}, Held(frame, NpadButton::LeftSL | NpadButton::RightSL),
           QStringLiteral("SL"), false);
    Button(painter, layer, {176, 30, 28, 16}, Held(frame, NpadButton::LeftSR | NpadButton::RightSR),
           QStringLiteral("SR"), false);
    Button(painter, layer, {252, 30, 40, 16}, Held(frame, NpadButton::R), QStringLiteral("R"),
           false);
    Button(painter, layer, {296, 30, 40, 16}, Held(frame, NpadButton::ZR), QStringLiteral("ZR"),
           false);

    painter.setFont(Font(12, true));
    RoundButton(painter, layer, {154, 70}, Held(frame, NpadButton::Minus), QStringLiteral("−"));
    RoundButton(painter, layer, {190, 70}, Held(frame, NpadButton::Plus), QStringLiteral("+"));

    // Face buttons in the Switch diamond: X top, A right, B bottom, Y left.
    painter.setFont(Font(11, true));
    RoundButton(painter, layer, {300, 66}, Held(frame, NpadButton::X), QStringLiteral("X"));
    RoundButton(painter, layer, {318, 84}, Held(frame, NpadButton::A), QStringLiteral("A"));
    RoundButton(painter, layer, {300, 102}, Held(frame, NpadButton::B), QStringLiteral("B"));
    RoundButton(painter, layer, {282, 84}, Held(frame, NpadButton::Y), QStringLiteral("Y"));

    // D-pad.
    const QPointF pad{120, 136};
    const auto arm = [&](QPointF offset, NpadButton button) {
        Button(painter, layer, QRectF{pad + offset - QPointF{7, 7}, QSizeF{14, 14}},
               Held(frame, button), QString{}, false);
    };
    arm({0, -15}, NpadButton::Up);
    arm({15, 0}, NpadButton::Right);
    arm({0, 15}, NpadButton::Down);
    arm({-15, 0}, NpadButton::Left);

    Stick(painter, layer, {44, 84}, frame.left_x, frame.left_y, frame.left_deadzone, frame.left_raw,
          frame.left_trail, frame.now_ms, Held(frame, NpadButton::StickL));
    if (live) {
        StickText(painter, {78, 66}, frame.left_x, frame.left_y);
    }
    Stick(painter, layer, {240, 136}, frame.right_x, frame.right_y, frame.right_deadzone,
          frame.right_raw, frame.right_trail, frame.now_ms, Held(frame, NpadButton::StickR));
    if (live) {
        StickText(painter, {274, 118}, frame.right_x, frame.right_y);
    }
    painter.restore();
}

} // Anonymous namespace

void PaintInputBase(QPainter& painter) {
    Paint(painter, Layer::Base, InputFrame{});
}

void PaintInputLive(QPainter& painter, const InputFrame& frame) {
    Paint(painter, Layer::Live, frame);
}

void PaintInputPanel(QPainter& painter, const InputFrame& frame) {
    PaintInputBase(painter);
    PaintInputLive(painter, frame);
}

namespace {

/// The bars panel in device-independent pixels.
constexpr double BARS_PADDING = 6.0;
constexpr double BARS_TEXT_HEIGHT = 16.0;
constexpr double BARS_GRAPH_HEIGHT = 100.0;
/// Right of the graph, for the "60" and "30" labels.
constexpr double BARS_LABEL_WIDTH = 20.0;

QColor BarColourOf(double ms) {
    switch (ColourOf(ms)) {
    case BarColour::Green:
        return BAR_GREEN;
    case BarColour::Orange:
        return BAR_ORANGE;
    case BarColour::Red:
        break;
    }
    return BAR_RED;
}

int Pixels(double units, double ratio) {
    return int(std::lround(units * ratio));
}

} // Anonymous namespace

BarsLayout LayoutBars(double ratio) {
    const BarPixels pixels = BarPixelsFor(ratio);
    const int padding = Pixels(BARS_PADDING, ratio);
    const QRect graph{padding, padding + Pixels(BARS_TEXT_HEIGHT + 4.0, ratio),
                      int(BAR_COUNT) * pixels.pitch, Pixels(BARS_GRAPH_HEIGHT, ratio)};
    const QSize size{graph.right() + 1 + Pixels(BARS_LABEL_WIDTH, ratio) + padding,
                     graph.bottom() + 1 + padding};
    return {.ratio = ratio, .pixels = pixels, .size = size, .graph = graph};
}

QRect BarsRect(const QRect& image, QSize size, const std::optional<QRect>& input) {
    const double scale = PanelScale(image);
    std::optional<Box> input_box;
    if (input) {
        input_box = Box{input->x(), input->y(), input->width(), input->height()};
    }
    const Box box = PlaceBars({image.x(), image.y(), image.width(), image.height()}, size.width(),
                              size.height(), int(std::lround(16.0 * scale)), input_box,
                              int(std::lround(8.0 * scale)));
    return {box.x, box.y, box.width, box.height};
}

void PaintBarSlot(QImage& graph, const BarsLayout& layout, const FrameBars& bars, size_t slot) {
    const int pitch = layout.pixels.pitch;
    const int height = graph.height();
    QPainter painter{&graph};
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(int(slot) * pitch, 0, pitch, height, Qt::transparent);
    if (!bars.Used(slot)) {
        return;
    }
    const double ms = bars[slot];
    const BarSize bar = MeasureBar(ms, height);
    const int x = int(slot) * pitch;
    painter.fillRect(x, height - bar.height, layout.pixels.bar, bar.height, BarColourOf(ms));
    if (bar.clipped) {
        // A bright cap: this frame is longer than the scale.
        painter.fillRect(x, 0, layout.pixels.bar, std::max(3, Pixels(3.0, layout.ratio)), CLIP_CAP);
    }
}

void PaintBarSlots(QImage& graph, const BarsLayout& layout, const FrameBars& bars) {
    for (size_t slot = 0; slot < BAR_COUNT; ++slot) {
        PaintBarSlot(graph, layout, bars, slot);
    }
}

void PaintBarsPanel(QPainter& painter, const BarsLayout& layout, const QImage& graph,
                    size_t first_slot, const BarStats& stats) {
    const double ratio = layout.ratio;
    const QRect& area = layout.graph;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(PANEL);
    painter.drawRoundedRect(QRectF{QPointF{}, QSizeF{layout.size}}, 10.0 * ratio, 10.0 * ratio);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.fillRect(area, GRAPH_BACK);

    // The ring of slots: the first slot at the left edge, the newest bar at the right.
    const int split = int(first_slot) * layout.pixels.pitch;
    painter.drawImage(QPoint{area.left(), area.top()}, graph,
                      QRect{split, 0, graph.width() - split, graph.height()});
    if (split != 0) { // an empty source rect would draw the whole image
        painter.drawImage(QPoint{area.left() + graph.width() - split, area.top()}, graph,
                          QRect{0, 0, split, graph.height()});
    }

    // The target lines over the bars, labelled right of the graph.
    const int line = std::max(1, Pixels(1.0, ratio));
    painter.setFont(NumberFont(11.0 * ratio, true));
    for (const auto& [ms, label] : {std::pair{LINE_60_MS, QStringLiteral("60")},
                                    std::pair{LINE_30_MS, QStringLiteral("30")}}) {
        const int row = area.top() + LineRow(ms, area.height());
        painter.fillRect(area.left(), row, area.width(), line, TARGET_LINE);
        const QRectF label_rect{double(area.right() + 1 + Pixels(4.0, ratio)), row - 8.0 * ratio,
                                BARS_LABEL_WIDTH * ratio, 16.0 * ratio};
        painter.setPen(TARGET_LINE);
        painter.drawText(label_rect, Qt::AlignLeft | Qt::AlignVCenter, label);
    }

    // The text line: mean, FPS, worst. The worst frame takes its bar's colour.
    const QRectF text_rect{double(area.left()), BARS_PADDING * ratio, double(area.width()),
                           BARS_TEXT_HEIGHT * ratio};
    painter.setFont(NumberFont(13.0 * ratio, true));
    painter.setPen(TEXT);
    if (stats.count == 0) {
        painter.drawText(text_rect, Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("frame time: no presents yet"));
    } else {
        painter.drawText(text_rect, Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("%1 ms   %2 FPS")
                             .arg(stats.mean_ms, 0, 'f', 1)
                             .arg(1000.0 / stats.mean_ms, 0, 'f', 1));
        const QString worst = QStringLiteral("worst %1 ms").arg(stats.worst_ms, 0, 'f', 1);
        painter.setPen(BarColourOf(stats.worst_ms));
        painter.drawText(text_rect, Qt::AlignRight | Qt::AlignVCenter, worst);
    }
    painter.restore();
}

} // namespace TrainerHud
