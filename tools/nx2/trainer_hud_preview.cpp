// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Paints the trainer overlay's input panel over a stand-in game image at 1280x720 and 3840x2160,
// so its layout can be checked on a machine without a display. Not part of the build:
//
//   g++ -std=c++20 -fPIC -Isrc $(pkg-config --cflags Qt6Gui) tools/nx2/trainer_hud_preview.cpp
//       src/lichii/render/trainer_hud.cpp $(pkg-config --libs Qt6Gui) -o /tmp/trainer_hud_preview
//   QT_QPA_PLATFORM=offscreen /tmp/trainer_hud_preview <output directory>

#include <cstdio>

#include <QGuiApplication>
#include <QImage>
#include <QLinearGradient>
#include <QPainter>

#include "lichii/render/trainer_hud.h"

using namespace TrainerHud;
using Core::HID::NpadButton;

namespace {

/// A flick of the left stick from rest to the upper right, over the last 0.3 s.
StickTrail Flick(s64 now_ms) {
    StickTrail trail;
    for (int i = 0; i <= 18; ++i) {
        const double t = i / 18.0;
        trail.Push({.x = s32(t * 26000), .y = s32(t * t * 20000), .ms = now_ms - 300 + i * 16});
    }
    return trail;
}

void Render(const QString& path, QSize image_size, const InputFrame& frame) {
    QImage image{image_size, QImage::Format_ARGB32_Premultiplied};
    QPainter painter{&image};
    QLinearGradient sky{0, 0, 0, double(image_size.height())};
    sky.setColorAt(0, QColor{70, 110, 160});
    sky.setColorAt(1, QColor{60, 90, 50});
    painter.fillRect(image.rect(), sky);
    // Stand-ins for Ocarina's minimap (bottom right) and the centre of the image.
    const double unit = image_size.height() / 720.0;
    painter.setPen(QPen{Qt::white, 2 * unit});
    painter.setBrush(QColor{0, 0, 0, 90});
    painter.drawRect(QRectF{image_size.width() - 260 * unit, image_size.height() - 230 * unit,
                            200 * unit, 170 * unit});
    painter.drawText(QRectF{image_size.width() - 260 * unit, image_size.height() - 230 * unit,
                            200 * unit, 170 * unit},
                     Qt::AlignCenter, QStringLiteral("minimap"));
    painter.drawEllipse(QPointF{image_size.width() / 2.0, image_size.height() / 2.0}, 40 * unit,
                        40 * unit);

    const QRect panel = PanelRect(QRect{QPoint{}, image_size});
    painter.translate(panel.topLeft());
    painter.scale(panel.width() / INPUT_PANEL_SIZE.width(), panel.height() / INPUT_PANEL_SIZE.height());
    PaintInputPanel(painter, frame);
    painter.end();
    if (!image.save(path)) {
        std::fprintf(stderr, "cannot write %s\n", qPrintable(path));
    } else {
        std::printf("%s\n", qPrintable(path));
    }
}

} // Anonymous namespace

int main(int argc, char** argv) {
    QGuiApplication app{argc, argv};
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <output directory>\n", argv[0]);
        return 1;
    }
    const QString out = QString::fromLocal8Bit(argv[1]);
    const s64 now = 10'000;
    const StickTrail flick = Flick(now);
    StickTrail right;
    right.Push({.x = -32767, .y = 0, .ms = now - 120});
    right.Push({.x = -23000, .y = -23000, .ms = now - 60});

    InputFrame recording{.buttons = NpadButton::A | NpadButton::ZR | NpadButton::Up |
                                    NpadButton::StickR,
                         .left_x = 26000,
                         .left_y = 20000,
                         .right_x = -23000,
                         .right_y = -23000,
                         .left_trail = &flick,
                         .right_trail = &right,
                         .now_ms = now,
                         .recording = true,
                         .recorded = 1234,
                         .replaying = false};
    InputFrame replaying{.buttons = NpadButton::B | NpadButton::L | NpadButton::Minus,
                         .left_x = 0,
                         .left_y = -32767,
                         .right_x = 0,
                         .right_y = 0,
                         .now_ms = now,
                         .replaying = true};
    InputFrame idle{.now_ms = now};

    Render(out + QStringLiteral("/hud-720-rec.png"), {1280, 720}, recording);
    Render(out + QStringLiteral("/hud-720-replay.png"), {1280, 720}, replaying);
    Render(out + QStringLiteral("/hud-720-idle.png"), {1280, 720}, idle);
    Render(out + QStringLiteral("/hud-2160-rec.png"), {3840, 2160}, recording);
    return 0;
}
