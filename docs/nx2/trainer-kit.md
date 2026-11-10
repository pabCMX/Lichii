# Trainer kit: time control and live switches

The trainer kit adds debug-server methods that a practice panel (like a speedrun trainer) needs:
pause and step the game, slow it down or freeze its world, and flip diagnostic switches while it
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

**`step_frames`** `{frames, timeout_s?}`, 1 to 600 frames. It pauses if the game runs, resumes,
waits until the guest present count grew by `frames`, and pauses again on every path (also on a
timeout or a lost client). It returns `{requested, advanced, complete, paused, presents}`.
It is present-granular: the pause lands a present or two late (`advanced` can exceed
`requested`), and the GPU drains a frame behind the CPU. Use `capture_frame` for an exact frame.
While the game is paused, `screenshot` waits for a present and fails.

**`time_control`** `{mode?, value?}`. Ocarina only, and only when the game booted with
`EDEN_NX2_OOT_TIME_CONTROL=1`. Modes:

| mode | value | step the game gets each frame |
|---|---|---|
| `off` | none | its own (or the FPS add-on's) |
| `scale` | 0 to 4 | its own step times `value` |
| `fixed` | 0 to 6 | `value`, whatever the frame time |

Without `mode` it only reports. It returns `{address, mode, value?, previous}`. The step is in
1/60-s ticks: 1.0 is one 60 Hz tick, 6.0 is FPS Unlock's largest step (a 100 ms frame).
`fixed 1.0` with FPS Unlock gives the same step every frame, as with both add-ons off.
`fixed 0` is the frozen world that still renders, if the game accepts it.

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
         fmov s8, s3               ; fixed
done:    ret
magic:   .word 0x524E5254          ; "TRNR"
```

Assembled with `clang --target=aarch64-linux-gnu`. It clobbers x10 to x12, s3 and the flags,
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
   each, a `capture_frame` afterwards shows the expected frame.
5. `time_control {mode: "scale", value: 0.5}`: Link walks at half speed. `value: 2`: double.
   `off`: normal.
6. Safe minimum: `scale` 0.25, 0.1, 0.05, 0.02, 0.01, then `fixed` 0.5, 0.1, 0.01, 0. Hold each
   for 30 s while walking and opening the menu. Record the lowest value that neither crashes nor
   hangs, and whether `fixed 0` freezes the world while the camera still moves.
7. With FPS Unlock on, `fixed 1.0`: game speed follows the frame rate (slower below 60 fps), as
   with both add-ons off.
8. Note which things do not slow down with the step (audio, cutscene video, water ripples below
   1.0, UI animation).
9. Live switch: `switches {set: {ipc_summary: true}}`; the log shows the IPC summary every 5 s;
   `false` stops it. Then `ipc_trace` (one line per service command) on and off.
10. Speed: with the variable unset and no trainer calls, the Base + kit build matches the Base
    within run-to-run noise on the standard route (the ship condition).

## Open risks

- The hook's register analysis comes from the add-on documentation; the time control adds a
  stack push below `sp` at the hook. Compiled code keeps `sp` 16-byte aligned, and AArch64 has no
  red zone, so this is safe if the function follows the ABI. The live test confirms it.
- The block may be game-owned BSS. The method refuses a block that is not zero and has no magic,
  but a game write after the trainer set the magic is not detected.
- `step_frames` from the Qt frontend depends on the GUI thread running the queued slot. If the
  GUI thread is blocked (a modal dialog), pause times out with an error, not a hang.
