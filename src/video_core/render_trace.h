// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"
#include "common/nx2_live_switch.h"

namespace VideoCommon::RenderTrace {

inline bool Enabled() {
    return Common::Nx2Live::g_render_trace();
}

inline bool SampleFrame(u64 frame) {
    return Enabled() && frame % 600 == 0;
}

} // namespace VideoCommon::RenderTrace
