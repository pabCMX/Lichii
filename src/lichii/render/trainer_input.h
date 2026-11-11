// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cmath>
#include <cstddef>

#include "common/common_types.h"

/// The parts of the trainer overlay's input panel that need no Qt, so the unit tests use them.
namespace TrainerHud {

/// A full stick deflection on one axis as the game reads it (HID_JOYSTICK_MAX).
constexpr double STICK_MAX = 32767.0;

struct StickReading {
    /// Compass degrees, 0 to 359: 0 is up (the stick pushed forward), turning clockwise, so 90 is
    /// right and 270 is left. -1 when the stick is centred (magnitude under 0.005).
    int angle;
    /// The length over STICK_MAX: 0 centred, 1 at the rim. Above 1 when the source gives square
    /// corners, such as a keyboard diagonal without range limits.
    double magnitude;
};

/// Reads a stick as the npad gives it: x to the right, y up.
inline StickReading ReadStick(s32 x, s32 y) {
    const double magnitude = std::hypot(double(x), double(y)) / STICK_MAX;
    if (magnitude < 0.005) {
        return {.angle = -1, .magnitude = magnitude};
    }
    constexpr double DEGREES = 180.0 / 3.14159265358979323846;
    const int angle = int(std::lround(std::atan2(double(x), double(y)) * DEGREES) + 360) % 360;
    return {.angle = angle, .magnitude = magnitude};
}

/// One stick position the game was given, at a host time in milliseconds.
struct TrailPoint {
    s32 x;
    s32 y;
    s64 ms;
};

/// The last stick positions, one per present, for the trail that makes a flick visible. A full
/// ring overwrites its oldest point.
class StickTrail {
public:
    /// 0.5 s at 120 presents a second.
    static constexpr size_t CAPACITY = 64;

    void Push(TrailPoint point) {
        points[(head + count) % CAPACITY] = point;
        if (count < CAPACITY) {
            ++count;
        } else {
            head = (head + 1) % CAPACITY;
        }
    }

    /// Drops the points older than max_age_ms at now_ms.
    void Expire(s64 now_ms, s64 max_age_ms) {
        while (count != 0 && now_ms - points[head].ms > max_age_ms) {
            head = (head + 1) % CAPACITY;
            --count;
        }
    }

    void Clear() {
        head = count = 0;
    }

    [[nodiscard]] size_t Size() const {
        return count;
    }

    /// 0 is the oldest point.
    [[nodiscard]] const TrailPoint& operator[](size_t index) const {
        return points[(head + index) % CAPACITY];
    }

private:
    std::array<TrailPoint, CAPACITY> points{};
    size_t head{};
    size_t count{};
};

} // namespace TrainerHud
