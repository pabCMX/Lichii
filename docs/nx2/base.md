# Base: origin/nx2 plus the harness kit

A Base is origin/nx2 at one commit plus the harness kit, and nothing else of ours. Dev branches
start from a Base and never merge newer upstream. New upstream work means a new Base. An A/B is
then always Base against Base plus one change. Names and flow are in `personal-oot-lab.md` on
`personal/oot-lab` (naming rules, 2026-11-09).

## This Base

Branch `base/Base-0ab7736df9-20261109-0905`, cut 2026-11-09 09:05 US Eastern.

| Commit | What it adds |
| --- | --- |
| `0ab7736df9` | origin/nx2: shader cache v2 as the Vulkan pipeline cache (#1036) |
| `3cb6a1edf3` | Timed `"ms"` input steps and `"measure": true` (`held-input-v1`). Input part of lab commit `5f99b06008` |
| `6a64e2e5ad` | `CallTool` and a unit test: input without `ms` or `measure` answers as origin does |
| this file | This page |

## What the kit adds

Two debug-server parameters and one method, all in `src/core/debugger/nx2_debug_tools.cpp`:

- `input` and `input_sequence` steps can give `ms` instead of `frames`. Timed steps follow host
  time. The response then has `clock: "host_time"` and `ms`.
- `measure: true` adds `measurement`: the present intervals of the held input.
- `input_record` records the game's input once per present, as steps `input_sequence` replays
  (H48). It adds `g_nx2_present_hook` to the present path and raises the limits to 6000 steps
  per call and 3840 measured presents. The Qt hotkey Record Input (Per Present), default
  Ctrl+F8, starts and stops it and saves the steps to `<user dir>/tas/present-record-<n>.json`.

Both are described in [debug-mcp.md](debug-mcp.md) under "Input". Without `ms` or `measure`,
nothing changes: the input injector (`EmulatedController::SetInputInjector`) is the origin code,
and the response has the origin keys. The test "Debug input answers as before unless the client
asks for ms or measure" in `src/tests/core/nx2_debug_server.cpp` checks this.

## What the harness uses

The harness is not in the Base. It is in `tools/nx2/oot_harness` on `personal/oot-lab`
(`harness.py`, `nightly/*.py`). Run it from that checkout against a Base build.

| Method | Parameters the harness sends | From |
| --- | --- | --- |
| `status` | none, `fps_window_ms` | origin |
| `screenshot` | none, `height`, `timeout_s` | origin |
| `log_tail` | `limit`, `after` | origin |
| `sample` | `threads`, `seconds`, `max_chars` | origin |
| `input` | `buttons`, sticks, `frames`, `timeout_s` | origin |
| `input` | `measure` | kit |
| `input_sequence` | steps with `frames`, `timeout_s` | origin |
| `input_sequence` | steps with `ms`, `measure` | kit |

The launch flags in `harness.py` and `nightly/session.py` (`EDEN_NX2_DEBUG_SERVER`,
`EDEN_NX2_DEBUG_DIR`, `EDEN_NX2_PIPELINE_DIFF`, `EDEN_NX2_PRELOAD_ALL_PIPELINES`,
`EDEN_NX2_PIPELINE_BUILD_TRACE`, `EDEN_DKB_FRAME_TRACE`, `EDEN_NX2_GPU_PROFILE`) are all on origin.
`--audio-trace` sets `EDEN_NX2_AUDIO_TRACE`, a lab-only diagnostic. A Base ignores it, so
`analyze_audio.py` finds no lines.

## Build

Linux check, from a checkout that has `tools/nx2/devbox` (for example `~/dev/lichii` on
`personal/oot-lab`):

```sh
flock ~/dev/lichii-builds/.build.lock env JOBS=4 \
  tools/nx2/devbox/linux-build.sh base/Base-0ab7736df9-20261109-0905
```

Windows packages use the main-PC recipe in `docs/nx2/devbox-build.md` on `personal/oot-lab`, with
`build-nopgo.cmd` (`build-profile.cmd` without `-fprofile-use`). Use PGO only when the owner asks.
