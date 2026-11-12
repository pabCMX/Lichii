// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Paints the trainer overlay's panels (input, frame-time bars) over a stand-in game image at
// 800x450, 1280x720, 1920x1080, 2560x1440 and 3840x2160 from made-up data, so their layout can be
// checked on a machine without a display. Not part of the build:
//
//   g++ -std=c++20 -fPIC -Isrc $(pkg-config --cflags Qt6Gui) tools/nx2/trainer_hud_preview.cpp
//       src/lichii/render/trainer_hud.cpp $(pkg-config --libs Qt6Gui) -o /tmp/trainer_hud_preview
//   QT_QPA_PLATFORM=offscreen /tmp/trainer_hud_preview <output directory>

#include <cmath>
#include <cstdio>
#include <optional>

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

void Save(const QImage& image, const QString& path) {
    if (!image.save(path)) {
        std::fprintf(stderr, "cannot write %s\n", qPrintable(path));
    } else {
        std::printf("%s\n", qPrintable(path));
    }
}

/// The bars panel alone at unit device pixels per layout unit, as the overlay paints it.
QImage BarsPanel(const FrameBars& bars, double unit) {
    const BarsLayout layout = LayoutBars(unit);
    QImage graph{layout.graph.size(), QImage::Format_ARGB32_Premultiplied};
    PaintBarSlots(graph, layout, bars);
    QImage panel{layout.size, QImage::Format_ARGB32_Premultiplied};
    panel.fill(Qt::transparent);
    QPainter painter{&panel};
    PaintBarsPanel(painter, layout, graph, bars.FirstSlot(), bars.Stats());
    return panel;
}

/// The input panel alone at scale times its layout size, to look at the details.
QImage InputPanel(const InputFrame& frame, double scale) {
    QImage panel{(INPUT_PANEL_SIZE * scale).toSize(), QImage::Format_ARGB32_Premultiplied};
    panel.fill(QColor{60, 90, 50});
    QPainter painter{&panel};
    painter.scale(scale, scale);
    PaintInputPanel(painter, frame);
    return panel;
}

/// A stand-in game image: sky, ground, Ocarina's minimap (bottom right) and the centre.
QImage Scene(QSize image_size) {
    QImage image{image_size, QImage::Format_ARGB32_Premultiplied};
    QPainter painter{&image};
    QLinearGradient sky{0, 0, 0, double(image_size.height())};
    sky.setColorAt(0, QColor{70, 110, 160});
    sky.setColorAt(1, QColor{60, 90, 50});
    painter.fillRect(image.rect(), sky);
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
    return image;
}

/// The game image with the panels placed and scaled as the overlay does it (device pixel ratio 1). bars or frame may
/// be null to leave that panel out.
void Render(const QString& path, QSize image_size, const InputFrame* frame,
            const FrameBars* bars) {
    QImage image = Scene(image_size);
    QPainter painter{&image};
    const QRect image_rect{QPoint{}, image_size};
    const QRect panel = PanelRect(image_rect);
    if (frame != nullptr) {
        painter.save();
        painter.translate(panel.topLeft());
        painter.scale(panel.width() / INPUT_PANEL_SIZE.width(),
                      panel.height() / INPUT_PANEL_SIZE.height());
        PaintInputPanel(painter, *frame);
        painter.restore();
    }
    if (bars != nullptr) {
        const QImage bars_panel = BarsPanel(*bars, PanelScale(image_size.height()));
        const QRect place = BarsRect(image_rect, bars_panel.size(),
                                     frame != nullptr ? std::optional{panel} : std::nullopt);
        painter.drawImage(place.topLeft(), bars_panel);
    }
    painter.end();
    Save(image, path);
}

/// count frames of ms each, with a little pacing noise.
void Run(FrameBars& bars, int count, double ms) {
    for (int i = 0; i < count; ++i) {
        bars.Push(float(ms + 0.35 * std::sin(i * 1.7)));
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
    // The marks before and during a replay with a pre-roll, and of the pre-roll ring.
    InputFrame wait{.now_ms = now, .replay_wait = 3300};
    InputFrame pre{.buttons = NpadButton::B, .now_ms = now, .pre_roll_left = 412};
    InputFrame ring{.now_ms = now, .ring_armed = true, .ring_kept = 2741};
    InputFrame ring_lost{.now_ms = now, .ring_armed = true, .ring_kept = 18000, .ring_lost = 604};

    Render(out + QStringLiteral("/hud-720-rec.png"), {1280, 720}, &recording, nullptr);
    Render(out + QStringLiteral("/hud-720-replay.png"), {1280, 720}, &replaying, nullptr);
    Render(out + QStringLiteral("/hud-720-idle.png"), {1280, 720}, &idle, nullptr);
    Render(out + QStringLiteral("/hud-720-wait.png"), {1280, 720}, &wait, nullptr);
    Render(out + QStringLiteral("/hud-720-pre.png"), {1280, 720}, &pre, nullptr);
    Render(out + QStringLiteral("/hud-720-preroll.png"), {1280, 720}, &ring, nullptr);
    Render(out + QStringLiteral("/hud-720-preroll-lost.png"), {1280, 720}, &ring_lost, nullptr);
    Render(out + QStringLiteral("/hud-2160-rec.png"), {3840, 2160}, &recording, nullptr);

    // The deadzone: the left stick moves inside its deadzone (the game gets 0, the raw dot
    // moves); the right stick at 179.65 degrees, magnitude 0.732, its raw dot further out.
    InputFrame deadzone{.left_x = 0,
                        .left_y = 0,
                        .right_x = 147,
                        .right_y = -23985,
                        .left_deadzone = 0.15f,
                        .right_deadzone = 0.15f,
                        .left_raw = TrailPoint{.x = 2900, .y = 2300, .ms = now},
                        .right_raw = TrailPoint{.x = 166, .y = -27300, .ms = now},
                        .now_ms = now};
    Render(out + QStringLiteral("/input-720-deadzone.png"), {1280, 720}, &deadzone, nullptr);
    Save(InputPanel(deadzone, 3.0), out + QStringLiteral("/input-zoom-deadzone.png"));
    Save(InputPanel(deadzone, 1.0), out + QStringLiteral("/input-1x-deadzone.png"));
    // A replay: no raw dot, the ring stays.
    InputFrame replay_ring{.left_x = 20000,
                           .left_y = 9000,
                           .left_deadzone = 0.15f,
                           .right_deadzone = 0.15f,
                           .now_ms = now,
                           .replaying = true};
    Render(out + QStringLiteral("/input-720-replay-ring.png"), {1280, 720}, &replay_ring, nullptr);
    Save(InputPanel(replay_ring, 3.0), out + QStringLiteral("/input-zoom-replay-ring.png"));

    FrameBars steady60;
    Run(steady60, 120, 1000.0 / 60.0);
    FrameBars steady30;
    Run(steady30, 120, 1000.0 / 30.0);
    // 60 FPS, a 20 ms run, back to 60, a 40 ms run, one 120 ms stutter, 60 to the end.
    FrameBars mix;
    Run(mix, 25, 1000.0 / 60.0);
    Run(mix, 20, 20.0);
    Run(mix, 15, 1000.0 / 60.0);
    Run(mix, 12, 40.0);
    Run(mix, 10, 1000.0 / 60.0);
    mix.Push(120.0f);
    Run(mix, 40, 1000.0 / 60.0);
    FrameBars started; // a few frames after the panel was turned on
    Run(started, 30, 1000.0 / 60.0);
    FrameBars empty;

    Save(BarsPanel(steady60, 1.0), out + QStringLiteral("/bars-60.png"));
    Save(BarsPanel(steady30, 1.0), out + QStringLiteral("/bars-30.png"));
    Save(BarsPanel(mix, 1.0), out + QStringLiteral("/bars-mix.png"));
    Save(BarsPanel(mix, 1.5), out + QStringLiteral("/bars-mix-ratio-1.5.png"));
    Save(BarsPanel(started, 1.0), out + QStringLiteral("/bars-started.png"));
    Save(BarsPanel(empty, 1.0), out + QStringLiteral("/bars-empty.png"));

    // Placement: alone, side by side with the input panel, and above it on a narrow image.
    Render(out + QStringLiteral("/both-720-bars-alone.png"), {1280, 720}, nullptr, &mix);
    Render(out + QStringLiteral("/both-720.png"), {1280, 720}, &deadzone, &mix);
    Render(out + QStringLiteral("/both-450-stacked.png"), {800, 450}, &deadzone, &mix);
    Render(out + QStringLiteral("/both-1080.png"), {1920, 1080}, &deadzone, &mix);
    Render(out + QStringLiteral("/both-1440.png"), {2560, 1440}, &deadzone, &mix);
    Render(out + QStringLiteral("/both-2160.png"), {3840, 2160}, &recording, &mix);
    return 0;
}
