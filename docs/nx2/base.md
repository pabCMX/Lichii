# Base: origin/nx2 plus the harness kit

A Base is origin/nx2 at one commit plus the harness kit, and nothing else of ours. Dev branches
start from a Base and never merge newer upstream. New upstream work means a new Base. An A/B is
then always Base against Base plus one change. Names and flow are in `personal-oot-lab.md` on
`personal/oot-lab` (naming rules, 2026-11-09).

## This Base

The Base of 2026-11-11 is `base/Base-e63d0a04df-20261111-0635`: origin/nx2 `e63d0a04df` (#1334)
plus the kit, and from this Base on also the trainer kit (the commits after this one). It was
prepared on origin/nx2 `17baed716d` and moved onto `e63d0a04df` at the 06:35 US Eastern cut with
no conflict. The move gives the kit commits new hashes, so the table names them by their commit in
the Base of 2026-11-10 (`base/Base-bcb37c49f7-20261110-0635`).

| Commit in the old Base | What it adds |
| --- | --- |
| `4660c43470`, `90966ac662` | Timed `"ms"` input steps, `"measure": true`, and the test that input without them answers as origin does |
| `82a20b6605`, `8d1e49261a` | This page and the `input_record` part of debug-mcp.md |
| `0f6f867628..c7cf221821` | Kit 2: the recorder and the rest of the list below |
| `8e55d172c9` (`explore/kit-recorder-fixes`) | After Ctrl+F8 stops a recording, the status bar shows the saved path for 10 s |
| this commit | This page names this Base |

The two doc commits of the old Base that named its upstream (`7a5b0a076a`, `4e97edeaa2`) are not
picked. This commit puts their text, updated, in place.

Two kit commits needed a conflict fix on `17baed716d`, both in `#include` lines only:

- `44d11eaba5` (hotkey), `src/lichii/main_window.cpp`: #1264 removed `common/game_presets.h`. The
  kit keeps its `core/debugger/nx2_debug_tools.h` line, and `game_presets.h` stays out.
- `fef13892c0` (screenshot present), `src/video_core/renderer_base.cpp`: #1249 added
  `common/settings.h`. Both `common/settings.h` and the kit's `core/core.h` stay.

Upstream since the old Base (`bcb37c49f7`), 41 commits. These change what a test run sees:

- #1253, own data folder: see "Data folder" below.
- #1264: Lichii never asks for the recommended settings on its own. See "What the harness uses".
- #1249: DLSS quality and model presets. The default model is now M, before it was K. See "DLSS".
- #1210: on Windows the texture cache collector thresholds also follow the system commit limit
  (RAM plus page files). A PC with a small page file collects earlier than on the old Base.
- #796: the status bar and the overlay show generated frames next to the FPS while frame
  generation runs.
- #850 (sysmix queue sized to the host callback), #1245 and #1246 (Vulkan upload and dispatch
  barriers), #1231, #1236, #959, #784, #853 (buffer, texture and memory fixes): engine changes
  with no setting. Compare speed only Base against Base + one change, as always.

- #1313: a new built-in Ocarina add-on, "Texture Pool x2 (built-in)". It doubles the first pool of
  the game's graphics allocator, against a freeze in the Castle Town guard house with Max
  Resolution. It is on unless a config lists it in `disabled_addons`, so every test run on this
  Base has it. Its 12-byte routine is at main+0x3C462C0. The time-control routine of the trainer
  kit is at main+0x3C46300, so they do not overlap.
- #1334: graphics pipelines unused for 9000 frames are no longer destroyed. They stay until the
  built pipelines pass a quarter of the RAM. A place visited again builds fewer shaders. The new
  setting "Build all cached shaders before game start" is off by default.
- #1329: no submit when nothing was recorded since the last one (about 4,600 empty submits per
  second in Castle Town, +6% FPS by its author). #1095: the split back thread writes 3D state
  registers in one call. #1327: plain loads for LDAR, LDAPR and LDAXR on x86 (+1.5 to 2 FPS in
  Kokiri by its author). All three are speed changes with no setting.
- #1335: device memory overallocation is allowed on AMD drivers (the main PC).
- #1333: read-only native compute bindings are sized from their size word again, not up to 1 GiB.
- #1326: native HDR output, setting `hdr_mode`, default Off.
- #1320 (less shader compile work), #852, #638, #889, #828, #637: no setting, no known effect on
  an Ocarina test run.

#1252 removes stub entries that real handlers already shadowed in `nx2_gsv.h`. The kit hooks in
`INx2Vi2Session::PresentClientBuffer` do not change.

## What the kit adds

Debug-server parameters and methods, in `src/core/debugger/nx2_debug_tools.cpp`:

- `input` and `input_sequence` steps can give `ms` instead of `frames`. Timed steps follow host
  time. The response then has `clock: "host_time"` and `ms`.
- `measure: true` adds `measurement`: the present intervals of the held input.
- `input_record` records the game's input once per present, as steps `input_sequence` replays
  (H48). Mode `any` (default) gives each step every button the npad updater gave the game
  during that present (`HID::g_npad_read_hook`) and the stick the game reads at the present's
  reply (`g_nx2_present_reply_hook`). Mode `sample` takes only the input at the start of each
  present's count. Both modes count presents with `g_nx2_present_hook`. It reads player 1 and
  the handheld controller. The limits are 6000 steps per call and 3840 measured presents. The
  Qt hotkey Record Input (Per Present), default Ctrl+F8, starts and stops it and saves the steps
  to `<user dir>/tas/present-record-<n>.json`. After the stop, the status bar shows that path
  for 10 s.
- `start_present` on `input` and `input_sequence` puts step 0 at that present. Frame-step calls
  with `start_present` or `measure` answer `landed_present`, the present step 0 really landed on.
  `screenshot` answers `present`, the guest present it captured.
- `features` lists what the build supports (`kit` 2), so a client can tell an older exe apart.
- `npad_sync` (default on): one npad update at each present's reply while a frame-step call or a
  recording in mode `any` runs (`HID::UpdateNpadNow`), so the game reads the present's step;
  `stale_presents` names where the race would have hit.
- Kit stall log (`KIT STALL` Error lines from a watchdog while the kit is active), the kit line
  at server start, and the kill switch `EDEN_NX2_KIT_HOOKS=0` (debug-mcp.md, "If the kit hangs").
- `luma_watch` keeps `[present, mean luma]` for the next N composites, to find a fade-in
  exactly. It stalls the GPU thread per present while armed (`RenderLumaSample` in both
  renderers, state in `video_core/nx2_debug_control`).

Both are described in [debug-mcp.md](debug-mcp.md) under "Input". Without `ms`, `measure` or
`start_present`, nothing changes: the input injector (`EmulatedController::SetInputInjector`) is
the origin code, and the response has the origin keys. The test "Debug input answers as before
unless the client asks for ms or measure" in `src/tests/core/nx2_debug_server.cpp` checks this.
`screenshot` adds `present`, the guest present of the captured frame.

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

Since #1264, Lichii never asks for the recommended settings on its own: not at start, not after
a game stops and not after a failed boot. Only Help > Apply Recommended Settings... asks. #1264
also removed the Mario Kart World session preset question. The setting
`recommended_settings_prompt_shown` is gone. The harness lines for it in `qt-config.ini` are not
read any more and do no harm, so a harness copy can keep them or drop them:

```ini
[Miscellaneous]
recommended_settings_prompt_shown\default=false
recommended_settings_prompt_shown=true
```

An A/B config must still not take "Use recommended settings": it changes performance settings.

## Data folder (#1253)

The data folder is now `lichii`, not `eden`: `%APPDATA%\lichii` on Windows,
`$XDG_DATA_HOME/lichii` on Linux.

- Portable mode still wins. If a folder `user` is next to `lichii.exe` (the harness
  `runtime\user`), all data and config are in it, as before.
- In portable mode Lichii does not look for an old profile, so no import dialog opens
  (`FrontendCommon::NeedsLegacyProfileImport` returns false when `IsPortableMode()`). Unattended
  boots of a harness copy are safe.
- The recorder file is `<TAS folder>/present-record-<n>.json`. The TAS folder is `tas_directory` in
  `qt-config.ini`; a harness copy sets it to `runtime/user/tas`. Without that key it is
  `<user dir>/tas`.
- The user copy of `nx2_switches.txt` is now `%APPDATA%\lichii\nx2_switches.txt`
  (`$XDG_CONFIG_HOME/lichii` on Linux), also in portable mode: this file never comes from the
  `user` folder. Lichii reads it after the copy next to the exe, and the environment wins over
  both. A test PC that keeps switches in `%APPDATA%\eden\nx2_switches.txt` loses them on this
  Base. Move the file, or set the switches in the environment.
- A non-portable install, on its first start, finds `%APPDATA%\eden` (or the folder of
  another emulator it can import from) and opens a modal "Migration" dialog: Import (Copy, the
  default, Move or Link) or Start fresh. Closing it stops Lichii with "Lichii startup stopped",
  and it asks again at the next start. The answer is kept in `%APPDATA%\lichii.migration`. Copy leaves
  `%APPDATA%\eden` as it is. `eden-cli` refuses to start until the GUI has had the answer.
- `tools/nx2/oot_harness/package.py` on `personal/oot-lab` still copies the owner profile from
  `%APPDATA%\eden`. After a Copy import, that folder no longer gets the owner's changes.

## DLSS (#1249)

DLSS stays off unless asked for. The switch is `nvidia_dlss` ("Use NVIDIA DLSS in place of a
game's DLSS"), default `false`, or `EDEN_NX2_DLSS_REPLACEMENT=dlss`. With neither, Ocarina at
handheld 1x uses its own upscaler, also on an NVIDIA card (`SelectOcarinaUpscaler`). The new
settings act only while DLSS runs:

- `nvidia_dlss_quality`, default Automatic (0). Automatic picks a mode that fits the render and
  output sizes. Native (1) is DLAA at the render size.
- `nvidia_dlss_preset`, default M (0). Before #1249 the model was K, and `EDEN_NX2_DLSS_PRESET`
  picked another one. That variable is no longer read.

Enum values in `qt-config.ini`: quality Automatic 0, Native 1, Quality 2, Balanced 3,
Performance 4, Ultra Performance 5; preset M 0, NVIDIA default 1, E 2, F 3, J 4, K 5, L 6.
A per-game file (`config/custom/<program id>.ini`) can override all three. To pin DLSS off
for an A/B, in `qt-config.ini` and in the per-game file if it has the key:

```ini
[Renderer]
nvidia_dlss\default=false
nvidia_dlss=false
```

To compare a DLSS run with an older Base (model K), also pin the mode and the model:

```ini
[Renderer]
nvidia_dlss_quality\default=false
nvidia_dlss_quality=0
nvidia_dlss_preset\default=false
nvidia_dlss_preset=5
```

The status bar and the overlay show `DLSS <mode> <preset>` while DLSS runs, so a screenshot shows
whether it ran.

## Build

Linux check, from a checkout that has `tools/nx2/devbox` (for example `~/dev/lichii` on
`personal/oot-lab`):

```sh
flock ~/dev/lichii-builds/.build.lock env JOBS=4 \
  tools/nx2/devbox/linux-build.sh base/Base-e63d0a04df-20261111-0635 base-e63d0a04df
```

Windows packages use the main-PC recipe in `docs/nx2/devbox-build.md` on `personal/oot-lab`, with
`build-nopgo.cmd` (`build-profile.cmd` without `-fprofile-use`). Use PGO only when the owner asks.
