# Base: origin/nx2 plus the harness kit and the trainer

A Base is origin/nx2 at one commit plus the harness kit and the trainer, and nothing else of
ours. Dev branches start from a Base and never merge newer upstream. New upstream work means a
new Base. An A/B is then always Base against Base plus one change. Names and flow are in
`personal-oot-lab.md` on `personal/oot-lab` (naming rules, 2026-11-09).

## This Base

The current Base is `base/Base-33701f0e75-20261116-0710`: origin/nx2 `33701f0e75` (#1979) plus
99 commits of ours, the 99 of `base/Base-efddc1301c-20261115-0749` in the same order, with this
commit in place of that Base's base.md commit (the 87th).

- 97 picked with no conflict and have the same added and removed lines as before.
- `time control: forget the block address at System shutdown` picked with no conflict. Only its
  context changed: #1932 now resets the general channel before `kernel.Shutdown()`. Our reset is
  still the line after `kernel.Shutdown()` in `ShutdownMainProcess`.
- The gpu_sites fix had a conflict (next section).

No commit of ours was dropped. The hotkeys are the same (see "Hotkeys").

## The gpu_sites fix

`vk_scheduler: time GPU sites only with EDEN_DKB_GPU_SITES=1`, picked from 542b05d850 (fork
branch `fix/dkb-gpu-sites-variable`, upstream PR #1811, still open on 2026-11-16). Since #1723,
`EDEN_DKB_FRAME_TRACE=1` also wrote a timestamp before each recorded command of the frame it
samples once a second, and reset a 32,769-query pool for it. In heavy Ocarina scenes that frame
took about 106 ms instead of about 55 ms, every 1.13 s. On this Base the site markers and their
query pool also need `EDEN_DKB_GPU_SITES=1`. With that variable unset, the frame trace is the same
as before #1723, plus upstream's pass timer. When upstream merges #1811, drop this commit at the
next Base step.

Upstream's #1907 added a render pass timer (`DKB gpu_pass`, a 4096-query pool) to the frame
trace, and made that pool in the same block as the site pool. Git merged the code with no
conflict, so the pass pool came under our gate. In this commit the pass pool is made with the
work pool, under `EDEN_DKB_FRAME_TRACE=1` only, as upstream has it. Only the site pool needs
`EDEN_DKB_GPU_SITES=1`. The pass report counts its draws and dispatches through the work pool,
not through the site markers, so it works without the site markers. AGENTS.md has both texts:
upstream's `gpu_pass` text and our `EDEN_DKB_GPU_SITES` note.

## Upstream from efddc1301c to 33701f0e75

72 commits (first parent). These change what a test run sees.

Mesh shaders, on the main PC (AMD RX 6800 XT, RDNA2, AMD's Windows driver):

- #1937: a new setting, Mesh Shaders (Graphics, Extensions, also per game). In `qt-config.ini`
  it is `[Renderer]` `mesh_shaders`: 0 Automatic (the default), 1 Native, 2 Compute Fallback.
  Automatic now takes the compute fallback on RDNA2 with AMD's driver too (before: with RADV
  only). So on the main PC the default is now the fallback, the same as
  `EDEN_NX2_FORCE_MESH_FALLBACK=1`. To run native mesh shaders there, set `mesh_shaders=1`. The
  environment variable still wins: it turns native mesh off before the setting is read. On
  NVIDIA (the HTPC), Automatic is Native, as before. Log lines: `AMD RDNA2: task/mesh draws use
  the compute fallback, ...`, `Mesh Shaders setting: native task/mesh draws on AMD RDNA2`,
  `Mesh Shaders setting: task/mesh draws use the compute fallback`.
- #1940: the fallback now draws mesh programs with per-primitive outputs. It skipped them
  before: 2 of Ocarina's 280 mesh setups in the survey of #1934. The image of a fallback run can
  change where those two draw. A pipeline that the fallback still cannot take logs `Mesh
  emulation skipped for pipeline <hash>: <reason>`. Not tested on a GPU by its author.
- #1938: a device loss now ends the process. `Device::ReportLoss` is `[[noreturn]]`: the first
  thread logs `Device loss occurred!` and the `Device fault` lines, then `Closing Lichii: the
  GPU driver lost the Vulkan device`, flushes the log and calls `TerminateProcess` (Windows) or
  `std::_Exit`. Other threads that see the loss wait. The 15 s wait is only for Nsight Aftermath
  now. `MasterSemaphore::Refresh` and `Wait` also call `ReportLoss` on `VK_ERROR_DEVICE_LOST`.
  A device loss leaves no `Host crash` line and no minidump, and the exit code is 1.

Online play:

- #1979: Nextendo online play is in the build. Before, a compile-time switch kept it off. Now it
  follows the setting `[Services]` `nextendo_enabled` (Configure, Network), default `false`. With
  it off, nothing new runs: no session keep-alive (every 2 min), no status call at game boot, no
  host redirect. With it on and an account signed in, Lichii calls nextendo.network at start,
  every 2 min and at each game boot. A lab profile pins it off:

  ```ini
  [Services]
  nextendo_enabled\default=false
  nextendo_enabled=false
  ```

  #1910 adds Help, Check for Updates. Our Windows recipe builds with
  `-DENABLE_UPDATE_CHECKER=OFF`, so there is no update call at start.

Image and shaders in Ocarina:

- #1875: the No sharpening bypass copies the texture that the output pass samples, not the
  output of one of three tonemap shaders. A frame that no replacement finishes runs the network
  again. It acts only with `oot_anti_aliasing` set to No sharpening.
- #1941: in the same bypass window, an unused AA dispatch (native key `9387db798781f081`) is
  skipped. It logs the first skip.
- #1871: the 3/4 upscale pass, the 1440p output pass and the scene depth reader are found by
  native program key, not by shader hash. FSR and DLSS no longer turn off when a lowering change
  gives these passes new hashes.
- #1848 and #1853: the routing of global memory accesses among candidate buffers. On the NVIDIA
  proprietary driver every access selects among all buffers again, against a GPU hang on Linux
  (Xid 109) in programs `f98012dc9a4465ff` and `e78efceda31f97a8`. #1853 is for Intel compile
  times. Both change SPIR-V.
- #1899: guest storage buffers are capped at the per-stage limit.
- #1901: nx2_gsv command 903, which Ocarina sends after its intro movie, is a stub now.

Guest timing and the GPU split:

- #1953: the split's back checks owed words with no lock (about 2 % back thread time in
  Ocarina, +1 to +2 % FPS in its PR on an RTX 5080).
- #1850: the split front values QMD reductions over an exact base. QMD schedulers have their own
  table.
- #1908: KeplerCompute's upload list no longer grows with native launches. Ocarina launches
  compute natively, so the list grew 24 bytes per upload, about 768 MiB after 90 minutes. A
  memory fix for long sessions.
- #1845: bad-overlap images are collected at the collector mode's age. In the Skulltula House
  this held 16.8 GB of images on a 24 GB card at 5 FPS (#1755).
- #1933: `DMAClear` checks its whole range for buffers (it checked a quarter).
- #1936: the Null audio sink plays in real time. The lab mutes audio (`audio_muted=true`), which
  is not the Null sink, so no change for lab runs. #1847: the UI volume is no longer copied into
  the sink at boot.
- #1957 (a terminated thread closes outside the scheduler lock), #1952 (the cheat engine stops
  first), #1932 (kernel objects close before the kernel shuts down), #1928: stop paths.

Memory and failure paths (no change while there is memory):

- #1870, #1931, #1963, #1906: staging and buffer allocations free memory and retry, not abort.

Diagnostics:

- #1894: native compute dispatch time is split into GPU profile zones (`nx2_profile.h`).
- #1907: the frame trace's `DKB gpu_pass` (see "The gpu_sites fix").
- #1976, #1865: texture cache traces. #1943: the frame dump keeps sampled textures from filling
  its budget. #1886: `tools/nx2/cache_records.py`. #1891: `EDEN_NX2_GUEST_THREAD_STATS`,
  `EDEN_NX2_IDLE_STATS` and `EDEN_NX2_FS_READ_TRACE` are read after `nx2_switches.txt`, so the
  file can turn them on.

No known effect on an Ocarina test run: crypto, loader and file_sys checks (#1876 to #1883,
#1916 to #1918, #1920, #1962, #1964, #1968), #1889 (Donkey Kong Bananza), #1862 (MoltenVK), #1929
(OpenGL), #1927 (macOS sticks), #1895, #1904, #1967 (Qt), #1912, #1935 (add-on content, CLI),
#1948, #1919, #1851 (services), #1861 (dynarmic arm64), #1846 (XDG paths), #1842 and #1844 (CA
bundle), #1854 and #1926 (release jobs), #1923 (fuzz), #1852, #1954, #1959, #1969 (tests and
warnings).

No lowering source changed (the stamp files are the same as at efddc1301c), so the stamp stays
`88561304c6618a81` and the first boot does not lower its cached programs again. The precache
marker inputs changed (#1940, #1848, #1853, `shader_environment.cpp`), so the precache runs once
more and the driver builds the changed pipelines again. No cpmfile.json, CMakeModules or
externals change.

## Upstream from 58e418db65 to efddc1301c

34 commits (first parent). These change what a test run sees.

The GPU split's front, always on, no setting:

- #1830 (Nx2ListIndirect): a GPU-written command list that only moves group counts through
  compute macro 0xe74 stays on the host GPU. Ocarina writes the grid of a later dispatch this
  way. The front no longer waits for the writer and the copy back ("list copy" back idle):
  the back dispatches from a slot of its own, indirectly. The A/B switch of the PR
  (`EDEN_NX2_LIST_INDIRECT=0`) was not merged. `EDEN_NX2_LIST_INDIRECT_CHECK=1` compares
  with the old read (and waits, so timing changes). `EDEN_NX2_GPU_PROFILE=1` adds its
  `list indirect` counts every 10 s.
- #1821: the front values QMD releases itself (FrontQmdView), so a greater-or-equal acquire on
  a word a QMD launch releases no longer drains the split. `EDEN_NX2_ACQUIRE_STATS` adds a
  `QMD view in 10 s` line, `EDEN_NX2_DRAIN_CHECK=1` checks the view against the back.
- Both change guest timing and the work order on the GPU. Time this Base against the old one
  before an A/B across the two, and compare end shots.

Shader output and the image:

- #1825: the constant fold of `BitFieldUExtract` no longer shifts a 32-bit value by 32. A
  32-bit field at bit 0 now folds to its base value. On x86 the old fold gave 0 there (the
  shift count wraps). An empty field folds to 0 as before. The fold runs when the SPIR-V is
  made, after a program comes from the store, so it acts at once with no new lowering.
- #1807: each native global memory access is routed among its own candidate buffers (the IR pass
  `global_memory_to_storage_buffer_pass.cpp` and the SPIR-V backend). It changes the SPIR-V of
  the Ocarina compute shader `331e607152ce78d5`. These files are not in the lowering stamp, so
  the stamp stays the same (below), but the pipelines built from the stored programs change.
- #1778: more Ocarina 1.0.0 output programs get the No sharpening bypass. It acts only with
  `oot_anti_aliasing` set to No sharpening on game version 1.0.0.
- #1812: a descriptor frame that runs out no longer submits in the middle of a draw. When the
  next frame's chunks are not free yet, that one draw or dispatch is skipped.
- #1796: the ASTC to BC transcode keeps its staging buffers until its copy runs (a race fix).

Guest timing:

- #1802: CMIF out buffers keep the guest bytes that a handler leaves alone (short IFile reads).
- #1817: GPU report timestamps in GPU ticks, for Mario Kart World only. Ocarina keeps
  nanoseconds (`UsesLegacyAmpereTimestamps` in `src/video_core/gpu_timestamp.h`).
- #1795: more early read-back downloads (256 unread writes, 512 pages, 8 MiB a frame). What a
  guest read finds does not change, but Ocarina's read-back reads wait for the GPU less often.
- #1803: at Loading Shaders the boot waits only for the pipelines of the last two sessions. Older
  ones build in the background, so a test run can meet more pipeline builds after the load.
- #1818: movie frames are not dropped for a client that holds more than 2 output images, or after
  the movie loops or jumps back.
- #1793: the Ocarina cloth and hair time step, only with FPS Unlock above 60 FPS.
- #1831: time zone rules give the repeated fall-back hour both times. It acts only in that
  hour.

Host speed only, the same results:

- #1780 (Windows `VirtualProtect` without the placeholder lock first), #1820 (submapped ranges
  from the map-time runs), #1809 (occlusion query reads poll; a result missing 1 s after its
  submission is a device loss).
- #1789: a CPU write to a page that no image was ever registered on no longer takes the texture
  cache lock, which the split's back holds through each draw.
- #1823: large ASTC arrays decode in batches, on integrated GPUs only.
- #1808: the GPU split also runs at High GPU accuracy and with safe DMA reads. At Normal accuracy
  (the lab default) the command list read path is the same as before.
- #1788: the multiplayer IP ban list reads its saved key. No effect on a test run.
- No known effect on an Ocarina test run: #1805 (SDL gyro after a reconnect), #1764 (RomFS
  dump of a loose .nup), #1826 (stutter trace counts), #1794 (zero-length `GetSpan` at the
  end of the address space), #1828 (`HexStringToArray` on a short string), #1768 (NCZ
  section prefix), #1481 (MinGW only), and GCC and test fixes (#1759, #1760, #1761, #1762,
  #1827, #1833).

No lowering source changed (stamp `88561304c6618a81` at 58e418db65 and at efddc1301c), so the
first boot of a profile does not lower its cached programs again. The shader changes of #1807
and #1825 are outside the stamp files: the driver builds the changed pipelines again. No
cpmfile.json or Vulkan pipeline cache version change. #1481 changes CMakeModules for MinGW
builds only.

## The move to c1f8790839

The Base `base/Base-c1f8790839-20261114-0635` was origin/nx2 `c1f8790839` (#1733) plus 98
commits of ours:

- the 85 kit commits of `base/Base-87d84d513c-20261113-0635` (the harness kit, the trainer and
  the doc commit of the prep of 2026-11-13), in the same order,
- `hotkeys: Toggle Trainer Overlay on Ctrl+F1` (new, see "Hotkeys"),
- its base.md commit,
- the 11 trainer bars commits of the same Base, in the same order.

The base.md commit of the Base of 2026-11-13 was replaced. No kit commit was dropped:
upstream's new debug-server methods (`capture_bundle`, `trace_record`, `trace_stop`,
`trace_status`) do not do what a kit method does.

Kit commits that changed in that move (the others have the same added and removed lines as
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

## Upstream from c1f8790839 to 58e418db65

14 commits (first parent). These change what a test run sees:

- #1783: `GetSpan` checks the host offset of every page in the range, not the block ids of the
  first and last page (port of Eden #4675). Test this Base against the H56 slivers.
- #1779: frame generation counts the spacing of generated frames from the deadline. It acts only
  with frame generation on, which the lab configs leave off.
- #1744: on AMD's PAL driver the Ocarina FSR jitter offsets are in [0, 1), so the jittered scene
  draws keep their last column and row (the right and bottom edge ripple). It acts on the AMD
  runs.
- #1771: the file log is flushed once a second, so a hang or kill keeps the last lines.
- #1766 (a render target with no memory is left out, not bound as the null view), #1751
  (dynarmic logs a JIT fault that is not a fastmem miss and passes it on to the crash handler):
  crash paths.
- #1743 and #1745: Ocarina movie frame counts and Scarecrow's Song lengths, only with FPS Unlock.
- #1757 (NVIDIA only), #1742 (Intel Windows only), #1784 (NCA sections opened on first use),
  #1750 (Star Fox GameChat), #1753 (DLSS panel), #1747 (SDL controller warning): no known
  effect on an Ocarina test run.

No lowering source changed, so the lowering stamp is the same as on `c1f8790839`: the first boot
of a profile does not lower its cached programs again. No cpmfile.json or CMakeModules change.

## Upstream from b55de0c1b2 to c1f8790839

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
  tools/nx2/devbox/linux-build.sh base/Base-33701f0e75-20261116-0710 base-33701f0e75
```

Windows packages use the main-PC recipe in `docs/nx2/devbox-build.md` on `personal/oot-lab`, with
`build-nopgo.cmd` (`build-profile.cmd` without `-fprofile-use`). Use PGO only when the owner asks.
On this Base the recipe must build the target `lichii`, not the old GUI target.
