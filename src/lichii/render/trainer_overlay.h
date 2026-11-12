// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <optional>

#include <QImage>
#include <QPointer>
#include <QScreen>
#include <QTimer>
#include <QWidget>

#include "core/perf_stats.h"
#include "lichii/render/trainer_hud.h"

namespace Core {
class System;
}
class GRenderWindow;

/// The trainer overlay (docs/nx2/trainer-kit.md, "Trainer overlay"): a click-through window over
/// the game image with two panels, each on its own hotkey, shown alone or together. The input
/// panel (the input the game is given) is at the bottom-left; the frame-time bars are centred
/// along the bottom, or above the input panel when the image is too narrow for both side by side.
/// MainWindow creates the window on the first hotkey press and toggles each panel with SetWanted.
/// A GUI timer runs only while a panel is wanted and can show; a panel that is off does no work.
/// Each panel repaints only when what it shows changed.
class TrainerOverlay : public QWidget {
    Q_OBJECT

public:
    enum class Panel { Input, Bars };

    /// hidden() is true while the overlay must not show: no game runs, or the game is in
    /// exclusive fullscreen, where a tool window cannot show over the swapchain.
    TrainerOverlay(QWidget* parent, GRenderWindow* game_view, Core::System& system,
                   std::function<bool()> hidden);

    void SetWanted(Panel panel, bool wanted);
    [[nodiscard]] bool Wanted(Panel panel) const {
        return panel == Panel::Input ? input_wanted : bars_wanted;
    }

    /// Starts the timer again if a panel is wanted and can show now. Called when a game has
    /// loaded and on show, hide and window state events of the game's windows.
    void Refresh();

    /// Empties the bars panel (bars, text, the history position). Called when a game stops and
    /// when one has loaded, so no game shows another's bars.
    void ResetBars();

signals:
    /// The REC state the input panel shows changed, so the status bar can show the same count at
    /// once.
    void RecordChanged(bool recording, u64 recorded, bool full);

protected:
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void Tick();
    /// Stops the timer and lets go of the frame time history.
    void Stop();
    /// Moves the window over the game image; false when it must not show.
    bool Place();
    /// The timer's interval for the panels that are wanted.
    void SetInterval();
    /// Follows the screen the game is on and its refresh rate changes, for SetInterval.
    void WatchScreen();
    void ClearBars();
    void ReadInput();
    /// Adds the presents since the last tick as bars; true when the panel must repaint.
    bool ReadBars();

    GRenderWindow* game_view;
    Core::System& system;
    std::function<bool()> hidden;
    QTimer timer;
    /// The top-level window whose events start the timer again.
    QWidget* watched_top{};
    QPointer<QScreen> watched_screen;
    bool input_wanted{};
    bool bars_wanted{};
    /// Each panel's place in this window, in its pixels.
    QRect input_rect;
    QRect bars_rect;

    TrainerHud::InputFrame frame{};
    TrainerHud::StickTrail left_trail;
    TrainerHud::StickTrail right_trail;
    u64 last_present{};
    /// The stick positions at the last present.
    TrainerHud::TrailPoint last_left{};
    TrainerHud::TrailPoint last_right{};
    /// The base layer of the input panel at its size in device pixels.
    QImage base;

    TrainerHud::FrameBars bars;
    TrainerHud::BarsLayout bars_layout{};
    /// The bars, one column per FrameBars slot (TrainerHud::PaintBarSlots).
    QImage bars_graph;
    /// The end of the newest present already in bars (TrainerHud::UpdateBars); none when empty.
    std::optional<Core::FrameTimeHistory::Clock::time_point> last_bar_end;
    /// The text line's stats, taken at most every BARS_TEXT_MS.
    TrainerHud::BarStats bars_stats{};
    s64 bars_stats_ms{};
    /// This overlay asked the perf stats to record the frame time history.
    bool history_on{};
};
