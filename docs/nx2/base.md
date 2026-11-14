# Base: origin/nx2 plus the harness kit and the trainer

A Base is origin/nx2 at one commit plus the harness kit and the trainer, and nothing else of
ours. Dev branches start from a Base and never merge newer upstream. New upstream work means a
new Base. An A/B is then always Base against Base plus one change. Names and flow are in
`personal-oot-lab.md` on `personal/oot-lab` (naming rules, 2026-11-09).

## This Base

The current Base is `base/Base-c1f8790839-20261114-0635`: origin/nx2 `c1f8790839` (#1733) plus
98 commits of ours:

- the 85 kit commits of `base/Base-87d84d513c-20261113-0635` (the harness kit, the trainer and
  the doc commit of the prep of 2026-11-13), in the same order,
- `hotkeys: Toggle Trainer Overlay on Ctrl+F1` (new, see "Hotkeys"),
- this commit,
- the 11 trainer bars commits of the same Base, in the same order.

The old Base's last commit (its base.md) is replaced by this commit. No kit commit is dropped:
upstream's new debug-server methods (`capture_bundle`, `trace_record`, `trace_stop`,
`trace_status`) do not do what a kit method does.

Kit commits that changed in the move (the others have the same added and removed lines as
before):

- `debugger: timed ("ms") and measured ("measure") debug-server input`: upstream gave
  `ReadFrameTimes` a `recent_limit` and a `longest_ms` key. The kit's `IntervalStats` (shared by
  `frame_times` and `measure`) takes both, so `measurement` also has `longest_ms` now.
- `debugger: input_record records the game's input once per present`: upstream removed
  `g_nx2_present_time_services` and writes the present times at each present (for bug bundles).
  #1696 moved the count and the time store into `Core::RecordGuestPresent()`, which also counts
  Switch 1 game composites. It returns the new count now, and the kit's present hook runs after
  it in vi2 229 only, as before. The npad sync and the recorder read `g_nx2_present_count`.
- `debugger: RecordInput and IsServiceRunning for the frontend`: `IsServiceRunning` read the
  removed counter. It reads a kit counter `running_services` now, which the debug service
  counts up and down (3 lines).
- `debugger: screenshot reports the guest present it captured`: upstream moved the screenshot to
  `CaptureScreenshot`, which returns a `Png` (also used by the bug bundle). `Png` has a
  `present` field for it.
- `trainer kit: fixed time control in each caller's unit; step_frames capture`: the "paused"
  refusal is at the top of the `screenshot` method, before `CaptureScreenshot`.
- `kit: input_sequence capture_frames saves screenshots at clip frames`: `CaptureScreenshot`
  encodes with the kit's `ScreenshotPng`, as the kit's `Screenshot` did. The PNG is the same.
- The hotkey array is 35, 36 and 37 long after `yuzu: Record Input (Per Present) hotkey`,
  `trainer overlay: input display window` and `trainer overlay: frame-time bars panel`.
  Upstream's "Report a Bug" and "Load/Remove Amiibo" made it 34, and git takes our size changes
  as upstream's own, so each move must set them by hand.
- `hotkeys: our entries at their row in the hotkey dialog`: our three rows are at their dialog
  rows again (19, 28 and 35), after upstream's new rows.
- Conflicts in lists only: `src/common/CMakeLists.txt`, `src/core/CMakeLists.txt` and
  `src/tests/CMakeLists.txt` (both sides stay), the tool names in `tools/nx2/test_debug_mcp.py`
  (upstream's `capture_bundle` and trace tools added).
- Bars commit `trainer overlay: review round 1 fixes`: `OnEmulationStopped` stops upstream's
  bug report capture and undoes the session speed changes (#1687), then resets the bars.

## Hotkeys

Upstream's "Report a Bug" (#1647) and our "Toggle Trainer Overlay" had the same default,
Ctrl+F12. The owner decided on 2026-11-13: Report a Bug keeps Ctrl+F12, the trainer overlay
moves to Ctrl+F1. No other default key or controller hotkey is used twice on this Base. Upstream's
other new hotkey is "Load/Remove Amiibo" on F2 (#1716). A profile that saved the old Ctrl+F12 as
a custom key for the overlay keeps it.

## Upstream since b55de0c1b2

55 commits (first parent). These change what a test run sees:

- #1728: the stuck-stop exit, the atomic config and play-time saves and the CRT fast-fail dumps
  of the fork PRs 1 and 2 are now upstream. A stop that stalls ends Lichii after it saves the
  settings, with no thread terminate.
- #1733: HostTiming sleeps until a margin before the next one-shot event and spins the rest
  (Windows timer lateness). This changes guest timing and pacing: time this Base against the
  old one, and read the TAS results of the old Base apart.
- #1685: no end-of-load driver cache save where reads race builds. Check the H58-S2 warm start
  again on this Base (main PC).
- #1736 (command list pages copied again after DMA copies and clears), #1731 (texture
  collector), #1700 (host1x syncpoint actions outside the guard), #1081 (fast-draw verification
  gate), #1677 (Intel barriers), #1732 (mesh fallback on RADV RDNA2): GPU path changes with no
  setting.
- #1687: "Limit Speed Percent" stays as the user set it after a game closes.
- #1696: Switch 1 games count frames for frame-timed input (`g_nx2_present_count`).
- Diagnostics, off unless set: `EDEN_NX2_TC_TRACE` (#1130), `EDEN_NX2_VULKAN_CACHE_FLUSH`
  (#963), sampled GPU frames by source line (#1723), JIT block names in the Windows sampler
  (#1724), trace replay checks (#1656).
- #1704 (Mario Kart World 60 FPS mode), #1734 and #1737 (solid NSZ and NCZ), #1395 (status bar
  chip colours), #1603 (commit in About), #1716 (Amiibo hotkey): no known effect on an Ocarina
  test run.

The lowering stamp is the same as on b55de0c1b2, so it differs from the Base of 2026-11-13
(next section).

## Upstream from 87d84d513c to b55de0c1b2

67 commits (first parent). These change what a test run sees:

- The shader lowering sources changed (#1181, #1008), so the lowering stamp changed. The first
  boot of each profile on this Base lowers its cached programs again, as at the owner start of
  2026-11-13 (20.7 min to the first present). Warn the owner before he starts this Base.
- #1639: a fixed frame-fence wait in vi2 suspends the guest service thread on a kernel event,
  so its core can run other work. #1641: draw state is reused on the sole GPU thread. #1651
  reverts #1486 (list-fed grid macro calls): it caused white flashing in Ocarina. #1534, #995,
  #1105, #1155, #1218, #1568, #1619, #1621: buffer, pipeline build and lock changes with no
  setting. Time this Base against the old one before an A/B across the two.
- Ocarina: #1659 (camera from the guest buffer binding), #1672 (NaN and Inf removed from the
  DLSS colour input), #1137 (1.0 output passes in No Sharpening), #863 (points and lines through
  the mesh fallback: the foliage shadows; it acts on the AMD runs of the main PC).
  #1679, #1663, #1660 and #1180 act only with FPS Unlock.
- #1406 and #1407: the pipeline binary store stays on for AMD's own driver, and a session keeps
  what it used. #1653: RADV keeps its own pipeline cache.
- #1417: a close during the boot shader load no longer hangs, and the next boot in the same
  window works.
- #1647: the one-press bug bundle (Help > Report a Bug, `capture_bundle`;
  `docs/nx2/bug-bundle.md`). The present times are now written at every present, also with no
  debug server.
- #1648 and #1655: GPU trace record and headless replay (`trace_record`, `EDEN_NX2_GPU_TRACE`,
  `eden-cli --replay`; `docs/nx2/gpu-trace.md`). Off unless asked for.
- #1626: the default log names what still builds on the GPU thread mid-game.
- #1298 and #1676 (audio sysmix), #1396 (motion), #1401 (game list artwork per program id),
  #1301 (NVIDIA disk cache on Linux), #1558 and #1615 (Windows loading screen): no known effect
  on an Ocarina test run.

The upstream changes from `977dd8ab43` to `87d84d513c` and the rename are in the
`docs/nx2/base.md` of `base/Base-87d84d513c-20261113-0635`. The rename facts that a script needs
are in the next section.

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
- On the night prep, `rename.py check` listed 3 pending names in kit code, the page size and
  page mask constants of `Core::Memory`. #1550 renamed them, and the move renamed the kit's
  3 lines in the same commit. `rename.py check --strict` passes on this Base.

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
- Input display overlay, hotkey Ctrl+F1, bottom-left. It does not overlap upstream's frame
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
  output sizes. Native (1) is DLAA at the render size. Since #1589, in Ocarina, Balanced,
  Performance and Ultra Performance also lower the game's render scale.
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
  tools/nx2/devbox/linux-build.sh base/Base-c1f8790839-20261114-0635 base-c1f8790839
```

Windows packages use the main-PC recipe in `docs/nx2/devbox-build.md` on `personal/oot-lab`, with
`build-nopgo.cmd` (`build-profile.cmd` without `-fprofile-use`). Use PGO only when the owner asks.
On this Base the recipe must build the target `lichii`, not the old GUI target.
