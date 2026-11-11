// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <optional>

#include <QImage>
#include <QTimer>
#include <QWidget>

#include "lichii/render/trainer_hud.h"

namespace Core {
class System;
}
class GRenderWindow;

/// The trainer overlay (docs/nx2/trainer-kit.md, "Trainer overlay"): a click-through window over
/// the bottom-left of the game image with the input the game is given. MainWindow creates it on
/// the first hotkey press and toggles it with SetWanted. It reads the pad on a 60 Hz GUI timer
/// that runs only while it is wanted, and repaints only when what it shows changed. Later panels
/// (frame-time bars) go above the input panel in this window.
class TrainerOverlay : public QWidget {
    Q_OBJECT

public:
    /// hidden() is true while the overlay must not show: no game runs, or the game is in
    /// exclusive fullscreen, where a tool window cannot show over the swapchain.
    TrainerOverlay(QWidget* parent, GRenderWindow* game_view, Core::System& system,
                   std::function<bool()> hidden);

    void SetWanted(bool wanted);
    [[nodiscard]] bool Wanted() const {
        return wanted;
    }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void Tick();
    /// Moves the window over the game image; false when it must not show.
    bool Place();
    void ReadInput();

    GRenderWindow* game_view;
    Core::System& system;
    std::function<bool()> hidden;
    QTimer timer;
    bool wanted{};

    TrainerHud::InputFrame frame{};
    TrainerHud::StickTrail left_trail;
    TrainerHud::StickTrail right_trail;
    u64 last_present{};
    /// The stick positions at the last present.
    TrainerHud::TrailPoint last_left{};
    TrainerHud::TrailPoint last_right{};
    /// The recording's first present, read once when a recording starts.
    std::optional<u64> record_start;
    /// The base layer of the input panel at the window's size in device pixels.
    QImage base;
};
