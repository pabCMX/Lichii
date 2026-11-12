// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>

#include <QEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QRegion>
#include <QScreen>

#include "core/core.h"
#include "core/debugger/nx2_debug_tools.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "lichii/bootmanager.h"
#include "lichii/render/trainer_overlay.h"

namespace {

using TrainerHud::StickTrail;
using TrainerHud::TrailPoint;
using FrameTimeReader = Core::PerfStats::FrameTimeReader;

/// The bars panel's text line changes at most this often, so it does not flicker.
constexpr s64 BARS_TEXT_MS = 250;
/// How far back each tick reads the frame time history: far more than a tick, and the whole
/// panel at 60 FPS on the first tick.
constexpr auto BARS_READ_WINDOW = std::chrono::seconds{2};

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

bool SamePoint(const std::optional<TrailPoint>& a, const std::optional<TrailPoint>& b) {
    return a.has_value() == b.has_value() && (!a || (a->x == b->x && a->y == b->y));
}

bool SameInput(const TrainerHud::InputFrame& a, const TrainerHud::InputFrame& b) {
    return a.buttons == b.buttons && a.left_x == b.left_x && a.left_y == b.left_y &&
           a.right_x == b.right_x && a.right_y == b.right_y && a.left_deadzone == b.left_deadzone &&
           a.right_deadzone == b.right_deadzone && SamePoint(a.left_raw, b.left_raw) &&
           SamePoint(a.right_raw, b.right_raw) && a.recording == b.recording &&
           a.recorded == b.recorded && a.record_full == b.record_full &&
           a.replaying == b.replaying && a.replay_wait == b.replay_wait &&
           a.pre_roll_left == b.pre_roll_left && a.ring_armed == b.ring_armed &&
           a.ring_kept == b.ring_kept && a.ring_lost == b.ring_lost;
}

bool SameStats(const TrainerHud::BarStats& a, const TrainerHud::BarStats& b) {
    return a.count == b.count && a.mean_ms == b.mean_ms && a.worst_ms == b.worst_ms;
}

struct RawStick {
    float deadzone{};
    std::optional<TrailPoint> raw;
};

/// One stick of the controllers ReadGamePad reads: the configured deadzone, and the position
/// before the deadzone and range of the controller whose value is the one the game gets (game).
/// No raw position while injected is set, or when no controller's value matches (input from the
/// debug server replaces the stick). The raw position moves in steps of 1/64 of a deflection, so
/// stick noise does not repaint the panel at each tick.
RawStick ReadRaw(Core::System& system, Settings::NativeAnalog::Values index,
                 const Core::HID::AnalogStickState& game, bool injected, s64 now_ms) {
    // The values can change between ReadGamePad and this read.
    constexpr s32 TOLERANCE = 512;
    RawStick result;
    double best = -1.0;
    bool first = true;
    auto& hid = system.HIDCore();
    for (const auto id : {Core::HID::NpadIdType::Player1, Core::HID::NpadIdType::Handheld}) {
        const Core::HID::EmulatedController* const controller = hid.GetEmulatedController(id);
        if (!controller->IsConnected()) {
            continue;
        }
        const Common::Input::StickStatus stick = controller->GetSticksValues()[index];
        if (first) {
            result.deadzone = stick.x.properties.deadzone;
            first = false;
        }
        // As EmulatedController::SetStick makes the value the game reads.
        const s32 x = static_cast<s32>(stick.x.value * Core::HID::HID_JOYSTICK_MAX);
        const s32 y = static_cast<s32>(stick.y.value * Core::HID::HID_JOYSTICK_MAX);
        const double size = std::hypot(stick.x.raw_value, stick.y.raw_value);
        if (std::abs(x - game.x) > TOLERANCE || std::abs(y - game.y) > TOLERANCE || size <= best) {
            continue;
        }
        best = size;
        result.deadzone = stick.x.properties.deadzone;
        const auto step = [](float raw) {
            return static_cast<s32>(std::lround(raw * 64.0f) / 64.0 * TrainerHud::STICK_MAX);
        };
        result.raw =
            TrailPoint{.x = step(stick.x.raw_value), .y = step(stick.y.raw_value), .ms = now_ms};
    }
    if (injected) {
        result.raw.reset();
    }
    return result;
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
    SetInterval();
    connect(&timer, &QTimer::timeout, this, &TrainerOverlay::Tick);
    game_view->installEventFilter(this);
}

void TrainerOverlay::Refresh() {
    if ((input_wanted || bars_wanted) && !timer.isActive()) {
        SetInterval();
        timer.start();
        Tick();
    }
}

bool TrainerOverlay::eventFilter(QObject*, QEvent* event) {
    const auto type = event->type();
    if ((input_wanted || bars_wanted) &&
        (type == QEvent::Show || type == QEvent::Hide || type == QEvent::WindowStateChange)) {
        // Queued: Place may reparent this window, which must not happen inside the event.
        QTimer::singleShot(0, this, &TrainerOverlay::Refresh);
    }
    return false;
}

void TrainerOverlay::SetWanted(Panel panel, bool wanted) {
    if (panel == Panel::Input) {
        input_wanted = wanted;
        left_trail.Clear();
        right_trail.Clear();
    } else {
        bars_wanted = wanted;
        bars.Clear();
        last_bar_end = {};
        bars_stats = {};
        bars_stats_ms = 0;
        if (!bars_graph.isNull()) {
            bars_graph.fill(Qt::transparent);
        }
    }
    if (!input_wanted && !bars_wanted) {
        Stop();
        hide();
        return;
    }
    if (!bars_wanted) {
        Stop(); // lets go of the history; Refresh starts the input panel's timer again
    }
    if (timer.isActive()) {
        SetInterval();
        Tick();
        update();
    } else {
        Refresh();
    }
}

void TrainerOverlay::SetInterval() {
    // The bars step once per present. Half the screen's refresh period puts a present on screen
    // within half a refresh, so most presents show at the next refresh and the steps stay even.
    // The input panel alone keeps the 60 Hz coarse timer.
    const QScreen* const screen = game_view->screen();
    const double hz = screen != nullptr && screen->refreshRate() > 1.0 ? screen->refreshRate() : 60;
    const Qt::TimerType type = bars_wanted ? Qt::PreciseTimer : Qt::CoarseTimer;
    const int interval = bars_wanted ? std::clamp(int(500.0 / hz), 4, 8) : 16;
    if (timer.timerType() == type && timer.interval() == interval) {
        return;
    }
    const bool active = timer.isActive();
    timer.stop();
    timer.setTimerType(type);
    timer.setInterval(interval);
    if (active) {
        timer.start();
    }
}

void TrainerOverlay::Stop() {
    timer.stop();
    if (history_on && system.IsPoweredOn()) {
        system.GetPerfStats().SetFrameTimeHistoryEnabled(false, FrameTimeReader::TrainerBars);
    }
    history_on = false;
}

void TrainerOverlay::Tick() {
    if (!Place()) {
        hide();
        Stop();
        return;
    }
    QRegion dirty;
    if (input_wanted) {
        const TrainerHud::InputFrame before = frame;
        const bool trails_before = left_trail.Size() + right_trail.Size() != 0;
        ReadInput();
        if (before.recording != frame.recording || before.recorded != frame.recorded ||
            before.record_full != frame.record_full) {
            emit RecordChanged(frame.recording, frame.recorded, frame.record_full);
        }
        if (!SameInput(before, frame) || trails_before) {
            dirty += input_rect;
        }
    }
    if (bars_wanted && ReadBars()) {
        dirty += bars_rect;
    }
    if (isHidden()) {
        show();
    } else if (!dirty.isEmpty()) {
        update(dirty);
    }
}

bool TrainerOverlay::Place() {
    // Owned by the window that shows the game (the main window, or the render window in
    // separate-window mode), so it stays above that window and minimises with it.
    QWidget* const top = game_view->window();
    if (top != watched_top) {
        if (watched_top != nullptr) {
            watched_top->removeEventFilter(this);
        }
        watched_top = top;
        watched_top->installEventFilter(this);
    }
    if (hidden() || !game_view->isVisible() || top->isMinimized()) {
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

    const QRect input = TrainerHud::PanelRect(image);
    QRect target = input_wanted ? input : QRect{};
    QRect bars_global;
    if (bars_wanted) {
        const qreal ratio = devicePixelRatioF();
        if (bars_layout.ratio != ratio) {
            bars_layout = TrainerHud::LayoutBars(ratio);
            bars_graph = QImage{bars_layout.graph.size(), QImage::Format_ARGB32_Premultiplied};
            TrainerHud::PaintBarSlots(bars_graph, bars_layout, bars);
        }
        const QSize size{int(std::ceil(bars_layout.size.width() / ratio)),
                         int(std::ceil(bars_layout.size.height() / ratio))};
        bars_global =
            TrainerHud::BarsRect(image, size, input_wanted ? std::optional{input} : std::nullopt);
        target |= bars_global;
    }
    input_rect = input.translated(-target.topLeft());
    bars_rect = bars_global.translated(-target.topLeft());
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

    // A replay or its pre-roll gives the game its input, so the real stick is not what it gets.
    const bool injected = view.replaying || view.pre_roll_left != 0;
    const RawStick left = ReadRaw(system, Settings::NativeAnalog::LStick, view.left, injected, now);
    const RawStick right =
        ReadRaw(system, Settings::NativeAnalog::RStick, view.right, injected, now);

    frame.buttons = view.buttons;
    frame.left_x = view.left.x;
    frame.left_y = view.left.y;
    frame.right_x = view.right.x;
    frame.right_y = view.right.y;
    frame.left_deadzone = left.deadzone;
    frame.right_deadzone = right.deadzone;
    frame.left_raw = left.raw;
    frame.right_raw = right.raw;
    frame.now_ms = now;
    frame.recording = view.recording;
    frame.recorded = view.recorded;
    frame.record_full = view.record_full;
    frame.replaying = view.replaying;
    frame.replay_wait = view.replay_wait;
    frame.pre_roll_left = view.pre_roll_left;
    frame.ring_armed = view.pre_roll_armed;
    frame.ring_kept = view.pre_roll_kept;
    frame.ring_lost = view.pre_roll_lost;
}

bool TrainerOverlay::ReadBars() {
    if (!system.IsPoweredOn()) {
        return false;
    }
    // Asked at each tick: the perf stats are made again for each game. A set flag costs nothing.
    Core::PerfStats& perf = system.GetPerfStats();
    perf.SetFrameTimeHistoryEnabled(true, FrameTimeReader::TrainerBars);
    history_on = true;
    // A copy, taken under the perf stats mutex; the emulation threads never wait on the panel.
    const auto entries = perf.GetRecentFrameTimes(BARS_READ_WINDOW);
    const auto fresh = TrainerHud::NewFrames(entries, last_bar_end);
    for (const auto& entry : fresh) {
        const size_t slot = bars.FirstSlot();
        bars.Push(entry.ms);
        TrainerHud::PaintBarSlot(bars_graph, bars_layout, bars, slot);
    }
    bool changed = !fresh.empty();
    if (changed) {
        last_bar_end = fresh.back().end;
    }
    const s64 now = NowMs();
    if (now - bars_stats_ms >= BARS_TEXT_MS) {
        bars_stats_ms = now;
        const TrainerHud::BarStats stats = bars.Stats();
        changed |= !SameStats(stats, bars_stats);
        bars_stats = stats;
    }
    return changed;
}

void TrainerOverlay::paintEvent(QPaintEvent* event) {
    const qreal ratio = devicePixelRatioF();
    QPainter painter{this};
    if (input_wanted && event->rect().intersects(input_rect)) {
        const QSize pixels = (QSizeF{input_rect.size()} * ratio).toSize();
        const QPointF scale{input_rect.width() / TrainerHud::INPUT_PANEL_SIZE.width(),
                            input_rect.height() / TrainerHud::INPUT_PANEL_SIZE.height()};
        // The idle panel costs most of a paint (antialiased fills), so it is drawn once per size.
        if (base.size() != pixels) {
            base = QImage{pixels, QImage::Format_ARGB32_Premultiplied};
            base.setDevicePixelRatio(ratio);
            base.fill(Qt::transparent);
            QPainter base_painter{&base};
            base_painter.scale(scale.x(), scale.y());
            TrainerHud::PaintInputBase(base_painter);
        }
        painter.save();
        painter.translate(input_rect.topLeft());
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.drawImage(QPointF{}, base);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        painter.scale(scale.x(), scale.y());
        TrainerHud::PaintInputLive(painter, frame);
        painter.restore();
    }
    if (bars_wanted && !bars_graph.isNull() && event->rect().intersects(bars_rect)) {
        // In device pixels, so each bar is whole pixels wide.
        painter.save();
        painter.scale(1.0 / ratio, 1.0 / ratio);
        painter.translate(std::round(bars_rect.x() * ratio), std::round(bars_rect.y() * ratio));
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(QRect{QPoint{}, bars_layout.size}, Qt::transparent);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        TrainerHud::PaintBarsPanel(painter, bars_layout, bars_graph, bars.FirstSlot(), bars_stats);
        painter.restore();
    }
}
