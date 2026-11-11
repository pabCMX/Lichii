// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <exception>

#include <QPainter>

#include "common/logging.h"
#include "core/debugger/nx2_debug_tools.h"
#include "lichii/bootmanager.h"
#include "lichii/render/trainer_overlay.h"

namespace {

using TrainerHud::StickTrail;
using TrainerHud::TrailPoint;

s64 NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// Adds the stick position of a new present to the trail. Only moves go in, so a still stick
/// leaves an empty trail and the overlay stops repainting; the first move after a still spell
/// also keeps where it started. last is the position at the previous present.
void Follow(StickTrail& trail, TrailPoint& last, s32 x, s32 y, s64 now_ms) {
    if (x != last.x || y != last.y) {
        if (trail.Size() == 0 || trail[trail.Size() - 1].x != last.x ||
            trail[trail.Size() - 1].y != last.y) {
            trail.Push(last);
        }
        trail.Push({.x = x, .y = y, .ms = now_ms});
    }
    last = {.x = x, .y = y, .ms = now_ms};
}

bool SameInput(const TrainerHud::InputFrame& a, const TrainerHud::InputFrame& b) {
    return a.buttons == b.buttons && a.left_x == b.left_x && a.left_y == b.left_y &&
           a.right_x == b.right_x && a.right_y == b.right_y && a.recording == b.recording &&
           a.recorded == b.recorded && a.replaying == b.replaying;
}

} // Anonymous namespace

TrainerOverlay::TrainerOverlay(QWidget* parent, GRenderWindow* game_view_, Core::System& system_,
                               std::function<bool()> hidden_)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowTransparentForInput |
                          Qt::WindowDoesNotAcceptFocus | Qt::NoDropShadowWindowHint),
      game_view{game_view_}, system{system_}, hidden{std::move(hidden_)} {
    // Never takes the focus, a click or a key from the game.
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFocusPolicy(Qt::NoFocus);
    frame.left_trail = &left_trail;
    frame.right_trail = &right_trail;
    timer.setInterval(16);
    connect(&timer, &QTimer::timeout, this, &TrainerOverlay::Tick);
}

void TrainerOverlay::SetWanted(bool wanted_) {
    wanted = wanted_;
    if (!wanted) {
        timer.stop();
        hide();
        return;
    }
    left_trail.Clear();
    right_trail.Clear();
    record_start.reset();
    timer.start();
    Tick();
}

void TrainerOverlay::Tick() {
    if (!Place()) {
        hide();
        return;
    }
    const TrainerHud::InputFrame before = frame;
    const bool trails_before = left_trail.Size() + right_trail.Size() != 0;
    ReadInput();
    if (isHidden()) {
        show();
    } else if (!SameInput(before, frame) || trails_before) {
        update();
    }
}

bool TrainerOverlay::Place() {
    if (hidden() || !game_view->isVisible()) {
        return false;
    }
    // Owned by the window that shows the game (the main window, or the render window in
    // separate-window mode), so it stays above that window and minimises with it.
    QWidget* const top = game_view->window();
    if (top->isMinimized()) {
        return false;
    }
    if (parentWidget() != top) {
        setParent(top, windowFlags());
    }
    // The game image inside the render window, from its framebuffer layout (device pixels).
    QRect image = game_view->rect();
    const auto& screen = game_view->GetFramebufferLayout().screen;
    if (screen.GetWidth() != 0 && screen.GetHeight() != 0) {
        const qreal ratio = game_view->windowPixelRatio();
        image = QRectF{screen.left / ratio, screen.top / ratio, screen.GetWidth() / ratio,
                       screen.GetHeight() / ratio}
                    .toRect();
    }
    image.moveTopLeft(game_view->mapToGlobal(image.topLeft()));
    const QRect target = TrainerHud::PanelRect(image);
    if (geometry() != target) {
        setGeometry(target);
    }
    return true;
}

void TrainerOverlay::ReadInput() {
    const Core::Nx2Debug::GamePadView view = Core::Nx2Debug::ReadGamePad(system);
    const s64 now = NowMs();
    // The trails take one position per present: what the game got, not every poll.
    if (view.present != last_present) {
        last_present = view.present;
        Follow(left_trail, last_left, view.left.x, view.left.y, now);
        Follow(right_trail, last_right, view.right.x, view.right.y, now);
    }
    left_trail.Expire(now, TrainerHud::TRAIL_MS);
    right_trail.Expire(now, TrainerHud::TRAIL_MS);

    if (!view.recording) {
        record_start.reset();
    } else if (!record_start) {
        // Once per recording; a recording that ended meanwhile is read again at the next tick.
        try {
            const auto status = Core::Nx2Debug::RecordInput(system, "status");
            if (status["recording"].get<bool>()) {
                record_start = status["start_present"].get<u64>();
            }
        } catch (const std::exception& error) {
            LOG_WARNING(Frontend, "Trainer overlay: input_record status: {}", error.what());
            record_start = view.present + 1;
        }
    }

    frame.buttons = view.buttons;
    frame.left_x = view.left.x;
    frame.left_y = view.left.y;
    frame.right_x = view.right.x;
    frame.right_y = view.right.y;
    frame.now_ms = now;
    frame.recording = view.recording;
    frame.recorded =
        record_start && view.present >= *record_start ? view.present - *record_start + 1 : 0;
    frame.replaying = view.replaying;
}

void TrainerOverlay::paintEvent(QPaintEvent*) {
    const qreal ratio = devicePixelRatioF();
    const QSize pixels = (QSizeF{size()} * ratio).toSize();
    const QPointF scale{width() / TrainerHud::INPUT_PANEL_SIZE.width(),
                        height() / TrainerHud::INPUT_PANEL_SIZE.height()};
    // The idle panel costs most of a paint (antialiased fills), so it is drawn once per size.
    if (base.size() != pixels) {
        base = QImage{pixels, QImage::Format_ARGB32_Premultiplied};
        base.setDevicePixelRatio(ratio);
        base.fill(Qt::transparent);
        QPainter base_painter{&base};
        base_painter.scale(scale.x(), scale.y());
        TrainerHud::PaintInputBase(base_painter);
    }
    QPainter painter{this};
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.drawImage(QPointF{}, base);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.scale(scale.x(), scale.y());
    TrainerHud::PaintInputLive(painter, frame);
}
