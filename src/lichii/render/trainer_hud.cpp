// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>

#include <QFont>
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

constexpr double STICK_RADIUS = 26.0;

bool Held(const InputFrame& frame, NpadButton button) {
    return (frame.buttons & button) != NpadButton::None;
}

QFont Font(double pixels, bool bold) {
    QFont font;
    font.setPixelSize(int(pixels));
    font.setBold(bold);
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
/// the stick is clicked.
void Stick(QPainter& painter, Layer layer, QPointF centre, s32 x, s32 y, const StickTrail* trail,
           s64 now_ms, bool clicked) {
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
    painter.setPen(QPen{QColor{0, 0, 0, 160}, 1.0});
    painter.setBrush(clicked ? LIT : QColor{255, 255, 255});
    painter.drawEllipse(StickPoint(centre, x, y), 5.0, 5.0);
}

/// The stick's angle and magnitude as two lines, at top_left.
void StickText(QPainter& painter, QPointF top_left, s32 x, s32 y) {
    const StickReading reading = ReadStick(x, y);
    const QRectF angle_rect{top_left, QSizeF{64.0, 17.0}};
    const QRectF magnitude_rect{top_left + QPointF{0, 18.0}, QSizeF{64.0, 17.0}};
    painter.setPen(TEXT);
    painter.setFont(Font(15, true));
    painter.drawText(angle_rect, Qt::AlignLeft | Qt::AlignVCenter,
                     reading.angle < 0 ? QStringLiteral("---")
                                       : QStringLiteral("%1°").arg(reading.angle));
    painter.setFont(Font(14, false));
    painter.drawText(magnitude_rect, Qt::AlignLeft | Qt::AlignVCenter,
                     QString::number(reading.magnitude, 'f', 2));
}

/// The REC, REPLAY and WAIT marks.
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
    if (frame.replaying) {
        mark(QStringLiteral("▶ REPLAY"), REPLAY);
    }
    if (frame.replay_wait != 0) {
        mark(QStringLiteral("WAIT %1").arg(frame.replay_wait), WAIT);
    }
}

} // Anonymous namespace

QRect PanelRect(const QRect& image) {
    const double scale = std::clamp(image.height() / 720.0, 0.75, 4.0);
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

    Stick(painter, layer, {44, 84}, frame.left_x, frame.left_y, frame.left_trail, frame.now_ms,
          Held(frame, NpadButton::StickL));
    if (live) {
        StickText(painter, {76, 66}, frame.left_x, frame.left_y);
    }
    Stick(painter, layer, {240, 136}, frame.right_x, frame.right_y, frame.right_trail, frame.now_ms,
          Held(frame, NpadButton::StickR));
    if (live) {
        StickText(painter, {272, 118}, frame.right_x, frame.right_y);
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

} // namespace TrainerHud
