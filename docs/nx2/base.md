# Base: origin/nx2 plus the harness kit and the trainer

A Base is origin/nx2 at one commit plus the harness kit and the trainer, and nothing else of
ours. Dev branches start from a Base and never merge newer upstream. New upstream work means a
new Base. An A/B is then always Base against Base plus one change. Names and flow are in
`personal-oot-lab.md` on `personal/oot-lab` (naming rules, 2026-11-09).

## This Base

The Base cut from this prep is `base/Base-977dd8ab43-20261113-0635` (name to be confirmed at the
06:35 step): origin/nx2 `977dd8ab43` (#1552) plus 85 commits of ours. They are the 84 commits of
`base/Base-db7a53c1e2-20261112-0635` below its doc commit, in the same order, and this commit.
The two Bases differ by 55 upstream commits (first parent) and by the rename below.

## Upstream's rename (#1541, #1544, #1551, #1552, #1553)

Upstream replaced the names it inherited with Lichii names. The naming table and the codemod
are in `docs/nx2/rename.md`. What a test run or a build sees:

- The Qt frontend is `src/lichii`, the CLI is `src/lichii_cmd`. The CMake targets are `lichii`
  and `lichii-cmd`. The old targets are gone, so a script that builds the old GUI target fails
  on this Base. The output files do not change: `lichii.exe`, `eden-cli.exe`, `lichii.pdb`.
- The CMake options are `LICHII_*`. `CMakeModules/LegacyOptions.cmake` carries an old option
  name from the command line or the cache to the new one, with a deprecation message.
- Config files, config keys, hotkey names, the data folder, the shader cache magic and the log
  lines `Lichii Version:` and `Booting game:` do not change.
- The startup-check variables of the Qt frontend are now `LICHII_IS_CHILD` and
  `LICHII_DO_STARTUP_CHECKS`.

What it did to the kit, commit by commit:

- The five trainer overlay files are in `src/lichii/render/`, their test is
  `src/tests/lichii/trainer_input.cpp`, and their includes are `"lichii/..."`. Three commits had a
  "file location" conflict for these files. Git moved them, and the codemod fixed the includes
  in the same commits.
- The kit's edits to the Qt frontend, the CLI and their CMake files moved with git's rename
  detection, with no conflict.
- The added and removed lines of each commit are the same as before, except for the renamed
  paths and includes, and one line of the doc commit of the first Base of 2026-11-11, which
  now names no other emulator (the codemod made it name Lichii).
- `rename.py check` lists 3 pending names in kit code: the page size and page mask constants
  of `Core::Memory` in `nx2_debug_tools.cpp`. Upstream has not renamed these library names yet
  (an upstream branch for the library names is not merged). The rest of the pending list is
  upstream's.

## Upstream since db7a53c1e2

55 commits (first parent). These change what a test run sees:

- #1459: a deleted buffer is cleared from the bindings of every channel. The lab's H56 (white
  blinks in the Sacred Forest Meadow grass) names it.
- #1485: Ocarina light-table shadow maps through descriptor arrays (the dark water grid).
- #1508: NVIDIA DLSS Frame Generation for Ocarina as a frame generation type. #1493: frame
  generation runs with HDR.
- #1441, #1460, #1479, #1515, #1516, #1518, #1519, #1520, #1521, #1539: buffer, query, fast draw
  and shader path changes with no setting. Time this Base against the old one before an A/B
  across the two.
- #1450: guest threads are suspended before power-off (no Userspace PANIC at a stop). #1453:
  Windows writes a minidump and logs a `Host crash` line on an unhandled exception or abort().
- #1478: a dialog before a Switch 2 boot on a PC with under 16 GB of RAM
  (`show_low_ram_warning`, `[UI]`).
- #1536: recommended settings reset and restore per-game overrides. #1505: no cached pipeline
  build at boot after a boot load that did not finish.
- #1462: merge of upstream Eglantine nx2 `6be10b5c12`.
- New traces, off unless set: `EDEN_NX2_BUFFER_TRACE=1` (#1530).

## What the kit adds

Debug-server parameters and methods, in `src/core/debugger/nx2_debug_tools.cpp`:

- `input` and `input_sequence` steps can give `ms` instead of `frames`. Timed steps follow host
  time. The response then has `clock: "host_time"` and `ms`.
- `measure: true` adds `measurement`: the present intervals of the held input.
- `input_record` records the game's input once per present, as steps `input_sequence` replays
  (H48). Each step has every button the npad updater gave the game during that present
  (`HID::g_npad_read_hook`) and the stick the game reads at the present's reply
  (`g_nx2_present_reply_hook`). It counts presents with `g_nx2_present_hook`. Mode `any` is the
  only mode since kit 3; mode `sample` is refused. It reads player 1 and
  the handheld controller. The limits are 6000 steps per call and 3840 measured presents. The
  Qt hotkey Record Input (Per Present), default Ctrl+F8, starts and stops it and saves the steps
  to `<user dir>/tas/present-record-<n>.json`. After the stop, the status bar shows that path
  for 10 s.
- `start_present` on `input` and `input_sequence` puts step 0 at that present. Frame-step calls
  with `start_present` or `measure` answer `landed_present`, the present step 0 really landed on.
  `screenshot` answers `present`, the guest present it captured.
- `features` lists what the build supports (`kit` 3), so a client can tell an older exe apart.
- Npad sync, always on since kit 3: one npad update at each present's reply while a frame-step
  call or a recording runs (`HID::UpdateNpadNow`), so the game reads the present's step. The
  `npad_sync` key is refused as an unknown key, and `stale_presents` is gone.
- Kit stall log (`KIT STALL` Error lines from a watchdog while the kit is active), the kit line
  at server start, and the kill switch `EDEN_NX2_KIT_HOOKS=0` (debug-mcp.md, "If the kit hangs").
- `luma_watch` keeps `[present, mean luma]` for the next N composites, to find a fade-in
  exactly. It stalls the GPU thread per present while armed (`RenderLumaSample` in both
  renderers, state in `video_core/nx2_debug_control`).

The trainer, in the Base since 2026-11-12 ([trainer-kit.md](trainer-kit.md) and debug-mcp.md):

- Run control: `pause`, `resume`, `step_frames`, `time_control` (half speed and up, Ocarina only,
  with `EDEN_NX2_OOT_TIME_CONTROL=1` at boot) and `switches` (live diagnostic switches).
- Input display overlay, hotkey Ctrl+F12, bottom-left. It does not overlap upstream's frame
  stats overlay (Ctrl+V, top-left).
- Recorder pre-roll (`pre_roll`, `record_anchor`): input before Ctrl+F8 is kept.
- `capture_frames`: screenshots at clip frames. Since kit 3 the shot is the target present.
- `scan_memory` and `watch`: find a game value and read it at each present.
- `notify`: one line in the Lichii status bar.

These are described in [debug-mcp.md](debug-mcp.md) under "Input". Without `ms`, `measure` or
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

Since #1365 a profile that has not seen the current "What's New in Lichii" page gets it as a
modal dialog at start. The game runs under it, but a harness close then times out. Every test
profile needs these lines in `qt-config.ini`:

```ini
[UI]
show_whats_new\default=false
show_whats_new=false
whats_new_version\default=false
whats_new_version=1
```

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
  and it asks again at the next start. The answer is kept in `%APPDATA%\lichii.migration`.
  Copy leaves `%APPDATA%\eden` as it is. `eden-cli` refuses to start until the GUI has had the
  answer.
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
  tools/nx2/devbox/linux-build.sh base-prep/20261113 base-prep-20261113
```

Windows packages use the main-PC recipe in `docs/nx2/devbox-build.md` on `personal/oot-lab`, with
`build-nopgo.cmd` (`build-profile.cmd` without `-fprofile-use`). Use PGO only when the owner asks.
On this Base the recipe must build the target `lichii`, not the old GUI target.
