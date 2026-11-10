# Trainer kit: time control and live switches

The trainer kit adds debug-server methods that a practice panel (like a speedrun trainer) needs:
pause and step the game, slow it down (to 1/100, nearly frozen), and flip diagnostic switches while it
runs. It is on `explore/trainer-kit` (a Base plus these commits). It joins the harness kit only
after a test-PC A/B of the Base against Base + kit shows no speed change.

The kit sits on the `input_record` work (explore/h48-present-record); debug-mcp.md describes
that and the rest of the server. This file stays apart so the kit can be taken or left as one
layer.

## Upstream survey (2026-11-09)

| Need | Found | Used |
|---|---|---|
| pause, resume, frame advance | Eglantine 13d34459cd "Add a built-in MCP server for AI agents" (branch `fix/oot-map-change`, not on `nx2`): `Host::Pause`/`Resume`, Qt calls `OnPauseGame`/`OnStartGame` with a blocking queued call; `frame_advance` resumes, counts presents, pauses | Same pattern. The Qt call is queued, not blocking (see "Run control"). Its MCP framework is not ported: our server has its own transport. |
| live GPU profile | Eglantine e7498b8f41: `g_enabled` becomes an atomic with a generation counter | Not ported: here the profile also picks the method path at engine creation (see "Left out") |
| speed limit | Already on the Base (`speed_limit`, from Frizti ee1619ae70) | Unchanged |
| live switch registry, time scale, web panel | Nothing on origin, upstream branches or PRs of either repository | New |

Lichii PR #1152 (open) adds a second Ocarina cave at `0x3C46280` and grows the timer state to
`0xA0` bytes. The time control places its code and data after both.

## Methods

All need a running game except `switches`.

**`pause`** `{timeout_s?}`, **`resume`** `{timeout_s?}`. They return `{paused, changed, presents}`.
The frontend does the change; the method waits until `System::IsPaused` agrees (default 5 s).

**`step_frames`** `{frames, capture?, timeout_s?}`, 1 to 600 frames. It pauses if the game runs,
resumes, waits until the guest present count grew by `frames`, and pauses again on every path
(also on a timeout or a lost client). It returns `{requested, advanced, complete, paused,
presents}`. It is present-granular: the pause lands a present or two late (`advanced` can exceed
`requested`), and the GPU drains a frame behind the CPU.

`capture: true` (or the `capture_frame` params, such as `{sampled: true}`) captures the frame of
the last stepped present. A capture needs two presents. At the end of a frame,
`RasterizerVulkan::TickFrame` takes the request and arms the frame dump, which then records the
NEXT frame and finishes at that frame's end. So the call steps at least two presents. It steps all
but the last two, pauses, waits 100 ms for the GPU to finish the frames queued before the pause,
requests the capture while the game is paused, then steps the last two. `{frames: 1, capture:
true}` therefore advances two presents and captures the frame the game stops on. The reply adds
`capture` (as `capture_frame` answers) and `capture_present`. If the step does not present within
the timeout, the request is withdrawn and the call fails, so no later frame is captured.

While the game is paused, `capture_frame` and `screenshot` fail at once ("paused: ... needs a
present"). Before, they waited 5 s for a present and left their request pending; a pending
screenshot then refused every later one. Use `step_frames {frames: 1, capture: true}` for the
paused frame.

**`time_control`** `{mode?, value?}`. Ocarina only, and only when the game booted with
`EDEN_NX2_OOT_TIME_CONTROL=1`. Modes:

| mode | value | step the game gets each frame |
|---|---|---|
| `off` | none | its own (or the FPS add-on's) |
| `scale` | 0.01 to 4 | its own step times `value` |
| `fixed` | 0.01 to 6 | `value / 60` s of game time, whatever the frame time |

Without `mode` it only reports. It returns `{address, mode, value?, previous}`.

Units. The game's step is not a plain time. It is in ticks of `s1` Hz, where `s1` is 30 or 60 and
differs between callers: the original is `s1 / s0` (`s0` the frame-rate field), and FPS Unlock
returns seconds × `s1`. `scale` multiplies whatever the game computed, so every caller keeps its
ratio. `fixed N` gives each caller `N × s1 / 60`, that is N/60 s of game time per frame in its own
unit. `fixed 1`, `2`, `3` and `6` are the steps FPS Unlock gives at 60, 30, 20 and 10 FPS; they
are the steps the game already takes in real play.

The first live test (2026-11-10, v1.0.0a-20261109-11) found the first version of `fixed` broken:
it gave every caller the same N, ignoring `s1`. With `s1` = 60, fixed 0.5 was half speed; with
`s1` = 30, 0.5 was the normal step. Systems timed on different bases then ran at different
relative speeds: presents fell to 14 per second, then the log showed unmapped reads. `scale`
worked down to 0.01.

No true freeze. A step of 0 hangs the game: with `fixed 0` no frame was presented, and `off` did
not bring presents back. Both modes therefore refuse values below 0.01 (the smallest live-tested
scale). The panel's "Freeze" preset is `scale 0.01`, 100 times slower: the world nearly stands
still and the game still renders. `off` always returns to the game's own step. A game that already
hung cannot be recovered by any mode.

**Benchmark mode: `fixed 2`.** Every frame advances the game by 1/30 s, as in real play at 30
FPS, whatever the host's speed: a slow host renders the same game frames, only slower. With both
FPS add-ons off, a recorded clip then has the same game load per frame on every host as in the
owner's real play at 30 FPS, and replays are not affected by the host's frame rate. If the live
ladder below passes, this is the recommended setting for benchmark recordings and runs. A clip
recorded at `fixed 2` must be replayed at `fixed 2`: set the mode and value before the load whose
anchor the clip starts from, and keep it until the clip ends. In another mode the same inputs
land on other game frames.

**`switches`** `{set?: {name: bool, ...}}`. Lists the live switches as
`{name, env, class, on}`; `set` flips them first. An unknown name or a value that is not a
boolean fails the whole call before anything changes.

## Time control

### Patch

With `EDEN_NX2_OOT_TIME_CONTROL` set at boot, `PatchManager::PatchNSO` calls
`FileSys::OotTimeControl::Apply` after the built-in patches
(`src/core/file_sys/oot_time_control.cpp`). It writes two records:

- `main+0x6E6CC4`: `bl 0x3C46300` (`8F 7D D5 94`), in place of the original `fdiv s8, s1, s0`
  or the add-ons' `bl 0x3C46200`.
- `main+0x3C46300`: a 96-byte routine, in the executable padding between the end of text
  (`0x3C461D0`) and read-only data (`0x3C47000`).

```
3C46300  ldr  w11, 0x3C46200      ; first word of the add-on routine; zero padding if none
         cbz  w11, plain
         str  x30, [sp, #-16]!
         bl   0x3C46200            ; the add-on leaves its step in s8
         ldr  x30, [sp], #16
         b    scale
plain:   fdiv s8, s1, s0           ; the original instruction
scale:   adrp x10, 0x4E9F000
         add  x10, x10, #0xF00     ; control block, main+0x4E9FF00
         ldr  w11, [x10]
         ldr  w12, magic
         cmp  w11, w12
         b.ne done                 ; no magic: leave the step alone
         ldr  w11, [x10, #4]       ; mode
         ldr  s3, [x10, #8]        ; value
         cmp  w11, #1
         b.ne 1f
         fmul s8, s8, s3           ; scale
         ret
1:       cmp  w11, #2
         b.ne done
         ldr  s2, inv60            ; fixed: value * s1 / 60, in the caller's s1-Hz units
         fmul s3, s3, s1
         fmul s8, s3, s2
done:    ret
magic:   .word 0x524E5254          ; "TRNR"
inv60:   .float 0.016666668
```

Assembled with `clang --target=aarch64-linux-gnu` (108 bytes). It reads s1 and keeps it. It clobbers x10 to x12, s2, s3 and the flags,
which the add-on routines already clobber at this hook (`docs/nx2/ocarina-dynamic-fps.md`), and
x30, which the hook's `bl` already clobbers. It saves x30 on the stack around the nested call.
Because it checks the add-on routine at run time, it works with FPS Unlock, Dynamic FPS, or
neither, and changes neither add-on's bytes.

Guards, as for the built-ins. Any failure skips the whole time control, with a log line
`Skipping time control: ...`:

- title `0400486024DEC000`, module `main`, build id `E0F3CCFAF5...3348`;
- the original word at the hook is `fdiv s8, s1, s0`, and the patched word is that or the
  add-on call (a user mod on the hook wins);
- the cave is zero-filled executable padding; the 16-byte control block is zero-filled BSS.

On success the log shows `Applying time control to main (around the step add-on | around the
game's own step; control block main+0x4e9ff00)`. The NSO loader then records the block's guest
address (`OotTimeControl::Publish`); the next boot without the patch clears it.

### Control block

`main+0x4E9FF00`: `u32 magic, u32 mode, f32 value, u32 reserved`. The method writes the value,
then the mode, then the magic. Before it writes, it reads the block: if it is not zero and has
no magic, the game uses that memory, and the method refuses. The BSS checks show that the block
is mapped and starts at zero, not that the game never uses it. The live checklist reads it
during play.

The method is the only write. There is no general memory-write method.

### Unknowns

- The safe minimum step. The game may divide by its step or skip work at small steps. The live
  test must find the lowest `scale` and `fixed` value that do not crash or hang.
- Below a step of 1, the water ripples stop (PR #1152: the ripple simulation runs
  `trunc(step)` times). Slow motion shows still ripples until #1152 lands.
- Other clocks. Audio, video playback and some animations may use their own timers and not slow
  down with the step.

## Run control

The frontend registers a `RunControl` with `Core::Nx2Debug::SetRunControl` and clears it before
it is destroyed:

- Qt (`MainWindow`): queues `OnPauseGame` or `OnStartGame`, the slots of the pause button, so the
  menus and play-time counter stay in step. It is a queued call, not a blocking one: when a game
  shuts down, the GUI thread stops the server and waits for its handler, so a handler that waited
  on the GUI thread would deadlock.
- eden-cli: calls `System::Run` and `System::Pause`.

## Live switches

`Common::Nx2Live::Switch` (`src/common/nx2_live_switch.h`). The environment gives the start value at static initialization; `VideoCore::CreateGPU` reads it
again at each boot, after `nx2_switches.txt`, which also drops live changes from the last boot.

| name | variable | notes |
|---|---|---|
| `render_trace` | `EDEN_NX2_RENDER_TRACE` | Most lines are at Debug level: set `log_filter` too. `render_trace_frame` counts frames always, so sampling works from the first frame. The "Render trace enabled" boot line only appears with the variable. |
| `ipc_trace` | `EDEN_NX2_IPC_TRACE` | Read once per command, so a command's two log lines agree. |
| `ipc_summary` | `EDEN_NX2_IPC_SUMMARY` | Counts from the moment it is on; the first summary comes 5 s after the first command. |
| `vdec_trace` | `EDEN_NX2_VDEC_TRACE` | |

Left out of the first set, with the reason:

| Switch | Why not now |
|---|---|
| `EDEN_NX2_GPU_PROFILE` | Not an observer here. `Maxwell3D` sets `fast_maxwell3d` from it at construction (`maxwell_3d.cpp:65`), which picks the method path, and the split sets `g_acquire_counting` from it at start (`gpu_split.cpp:122`). Turned on live, it would miss method counters and back-thread zones; turned off live, the slow method path would stay and skew a speed A/B. Making it live means refreshing both on their threads. |
| `EDEN_DKB_FRAME_TRACE` | Threads register with its clock when they start (`dkb_cpu_trace.cpp`); threads that started before it was on would be missing. |
| Other observers (`QUERY_TRACE`, `METHOD_TRACE`, `SVC_TRACE`, `MEM_TRACE`, `PULLER_TRACE`, `QMD_TRACE`, `CB24_TRACE`, `RT_TRACE`, `TC_LOG`, `MME_LOG`, `WINDOW_TRACE`, `FPS_LOG`, `PB_CHECK`, `SPLIT_CHECK`) | Safe by class, not needed for the trainer yet. `PB_CHECK` is also a `DmaPusher` member set at construction, and `TC_LOG` sets `debug_stats` in the texture cache constructor. Each is a one-line change to a `Switch` when needed. |
| Dumps (`SHADER_DUMP`, `VDEC_DUMP`, `TEXTURE_DUMP`, `PB_DUMP`, `MME_DUMP`) | Each reads a directory path at several sites (six for `SHADER_DUMP`). A live version needs a path under the debug dump directory that the server sets. `capture_frame` already covers live frame dumps. |
| Behaviour, A/B races (`PB_SNAPSHOT`, `FETCH_DRAIN`, `FLUSH_DRAWS`, `SYNC_SUBMIT`, `FENCE_FLUSH_US`, `IDLE_SPIN_US`) | The table says "probably, at a frame end". They change timing, and an A/B race test wants a clean run from boot, so a relaunch is the honest test. Not added. |
| Behaviour, cached; not live | Need invalidation or are set at boot; see the debug-mcp.md table. |

### Cost on the default path

A switch read is one relaxed atomic load. On x86 and ARM that is a plain load. GCC 14 and
clang 19 at `-O2` (`if (flag) Log();`):

| Pattern | GCC 14 | clang 19 |
|---|---|---|
| Before: function-local `static const bool` (IPC, VDEC, render trace) | `call` to the getter, which tests the guard and loads the value | load and test the guard byte, then compare the value |
| Plain global bool, for comparison | `cmpb $0, g(%rip)` | `cmpb $1, g(%rip)` |
| After: relaxed `std::atomic<bool>` | `movzbl g(%rip)` + `testb` | `movzbl g(%rip)` + `testb` |

So each converted read is cheaper than before (no guard), and as cheap as a plain global. The
render trace is read per draw, the IPC switches per service command. The time control adds nothing when its variable is
unset. The test-PC A/B of Base against Base + kit is the measurement that decides.

## Tests

`[oot_time_control]` (`src/tests/core/oot_time_control.cpp`): off unless asked; installs around
the game's step or the add-on's; refuses changed bytes, a user mod, a used cave or block, short
text, early read-only data, short BSS and a truncated module, all without changing the output.
It runs the patched bytes in dynarmic: the step for no add-on and for a stand-in add-on, scaled,
fixed (0 included), off, an unknown mode and a block without magic, and checks that x30, sp and
every other register are unchanged.

`[nx2_trainer]` (`src/tests/core/nx2_trainer_tools.cpp`): `switches` lists, flips together and
refuses a bad entry before any change; `pause`, `resume` and `step_frames` against a fake
frontend, including no registered control, a step from a running game, a step that times out
and still pauses, a client that leaves before the resume runs (a pause is queued after it), and
bad frame counts; `time_control` parameter checks and the "not installed"
error.

The existing `[patch_manager]` tests show that the default path (variable unset) keeps both
add-ons byte-identical.

## Live acceptance checklist

For a test orchestrator, on the Windows build of `explore/trainer-kit`, with
`EDEN_NX2_DEBUG_SERVER=127.0.0.1:6545` and `EDEN_NX2_OOT_TIME_CONTROL=1`:

1. Log: `Applying time control to main (...)` once per boot, and no `Skipping time control`.
2. Before any `time_control` call, `read_memory {address: <address from time_control>, size: 16}`
   at the title screen and after 5 minutes of play: all zero. A non-zero value means the game uses
   the block: stop and report.
3. `pause`: the Qt menu shows paused, `status.paused` is true. `resume`: both clear.
4. `step_frames {frames: 1}` ten times: each `advanced` is 1 to 3, the game is paused after
   each. Then `step_frames {frames: 1, capture: true}` five times: each advances 2 presents,
   answers with a `capture` whose files exist, and its frame is the one the game is paused on (the
   next `screenshot` after a resume is not needed: compare with the window). A plain `capture_frame` or
   `screenshot` while paused fails at once with "paused: ... needs a present".
5. `time_control {mode: "scale", value: 0.5}`: Link walks at half speed. `value: 2`: double.
   `off`: normal.
6. Whole fixed steps FIRST, from a fresh boot, both FPS add-ons off: `fixed 1`, `fixed 2`,
   `fixed 3`, each held 30 s while walking and turning the camera, then `off`. After each hold
   and after `off`, search the log for `Unmapped` lines (`log_tail {contains: "Unmapped"}`): there
   must be none. Record presents/s per hold. Expected: `fixed 1` as the game's own speed at 60
   FPS, `fixed 2` as real play at 30 FPS, `fixed 3` as at 20 FPS. Any unmapped line ends the
   ladder; that boot proves nothing more.
7. Then fractional steps, each from a fresh boot after a clean whole-step ladder: `fixed 0.5`,
   `0.1`, `0.01`, then `scale` 0.25, 0.1, 0.01, each 30 s with walking and camera, then `off`, with
   the same `Unmapped` check. `fixed 0` and anything below 0.01 must be refused by the server.
8. With FPS Unlock on, `fixed 2`: game speed follows the frame rate (game time 1/30 s per frame),
   as with both add-ons off.
9. Benchmark mode: record a clip with `fixed 2` set before the save load, replay it 3 times with
   `fixed 2` set before the load anchor. It must reach the owner's end point as a clip recorded
   and replayed in `off` does.
10. Note which things do not slow down with the step (audio, cutscene video, water ripples below
   1.0, UI animation).
11. Live switch: `switches {set: {ipc_summary: true}}`; the log shows the IPC summary every 5 s;
   `false` stops it. Then `ipc_trace` (one line per service command) on and off.
12. Speed: with the variable unset and no trainer calls, the Base + kit build matches the Base
    within run-to-run noise on the standard route (the ship condition).

## Open risks

- The hook's register analysis comes from the add-on documentation; the time control adds a
  stack push below `sp` at the hook. Compiled code keeps `sp` 16-byte aligned, and AArch64 has no
  red zone, so this is safe if the function follows the ABI. The live test confirms it.
- The block may be game-owned BSS. The method refuses a block that is not zero and has no magic,
  but a game write after the trainer set the magic is not detected.
- `step_frames` from the Qt frontend depends on the GUI thread running the queued slot. If the
  GUI thread is blocked (a modal dialog), pause times out with an error, not a hang.
