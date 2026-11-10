# Debug server and MCP adapter

Agents debug games through a slow loop today: write env flags, ask the owner's PC to build a
package, wait for the owner to launch the game, press a hotkey and send the log back, then copy
dumps. Every new question costs a relaunch, and most cost a rebuild, because every
`EDEN_NX2_*` switch is read once at startup.

This document designs an opt-in debug server inside Lichii and an MCP adapter that agents run on
the PC, so an agent can ask a running game questions directly. Phase 1 is implemented on the
`feat/debug-mcp` branch; the later phases are proposals with the hook points they need.

Line numbers refer to `feat/debug-mcp` at the time of writing.

## Goals and constraints

- The owner's PC (RTX 3080, Windows) runs the game. Agents reach it through bash on a paired
  device: they can run programs and read files there, but cannot see the screen or press keys.
- Game files, keys and firmware never leave that PC, and the server must not give a way to read
  them.
- A normal run must not change: the server is off unless `EDEN_NX2_DEBUG_SERVER` is set, and
  while it is off the only cost is one relaxed atomic load per presented frame.
- No relaunch for a new question, and as few rebuilds as possible for a new tool.

## Architecture

```
agent ──stdio MCP──> tools/nx2/debug_mcp.py ──TCP 127.0.0.1, JSON lines──> Lichii
  (on the PC)          (Python, stdlib only)                               ├─ "DebugServer" thread
                                                                           │    tools: status, screenshot, log_*, ...
                                                                           └─ rasterizer thread
                                                                                takes requests at each frame end
```

### In-process server

`src/core/debugger/nx2_debug_server.cpp` is the transport. It listens on a loopback TCP port
with Boost.Asio, as the GDB stub does (`src/core/debugger/debugger.cpp:147`), and runs one
`io_context` on its own thread named `DebugServer`. Each line a client sends is one request,
`{"id":1,"method":"status","params":{}}`; each answer is one line,
`{"id":1,"result":{...}}` or `{"id":1,"error":"..."}`. The transport knows nothing about the
emulator: it maps method names to handlers. That keeps it unit-testable without a `System`.

`src/core/debugger/nx2_debug_tools.cpp` holds the tools. `Core::System::Impl::Load` starts the
service once a game has loaded (`src/core/core.cpp:476`), and `ShutdownMainProcess` stops it
before anything else (`src/core/core.cpp:481`). The member is declared last
(`src/core/core.cpp:610`), so it is also destroyed first. The tools therefore never see a
half-built or half-destroyed system, and the server works the same under the Qt frontend and
`eden-cli`.

Why not reuse the GDB stub? It speaks the GDB remote protocol and parks emulation on every stop
(`src/core/debugger/debugger.cpp:82`). It is the right tool for guest breakpoints and stays as it
is (`tools/nx2/gdbrsp.py` drives it), but it has no place for frames, shaders or logs. The new
server copies its threading model, not its protocol.

### Why JSON lines over TCP, with a separate adapter

- MCP's own transports are stdio, which needs the agent to launch the process (it cannot launch
  the emulator, which the owner starts), and Streamable HTTP, which needs sessions and
  server-sent events. Neither fits inside the emulator cheaply.
- A JSON-lines socket is about 200 lines of C++ with libraries Lichii already links (Boost.Asio,
  nlohmann::json, both already used by `core`). Anything can drive it: the adapter, a test,
  or `python -c`.
- The adapter owns everything that is MCP: tool descriptions, input schemas, image content, error
  wording. Those change often, and changing them needs no emulator rebuild. That is the point of
  the whole exercise.
- Loopback TCP works the same on Windows and Linux. Named pipes or Unix sockets would need two
  code paths.

### Threading

Three kinds of work, three places:

1. **Server thread.** Parses requests and runs handlers. Handlers that only read thread-safe
   state run here directly: status, log tail, log filter, and the guest memory reads
   (`read_memory`, `search_memory`; see "Guest and GPU memory" for why that is safe). Handlers run one at a time, so a slow
   request (a capture can take seconds) delays the next one. One agent at a time is the expected
   use.
2. **Rasterizer thread.** Everything that touches the texture cache, pipeline cache, scheduler or
   frame dump must run on the thread that owns them. With the GPU thread split
   (`src/video_core/gpu_split.h:60`), that is the back thread, which also runs
   `RendererVulkan::Composite` and therefore `rasterizer.TickFrame()`
   (`src/video_core/renderer_vulkan/renderer_vulkan.cpp:269`). Requests for it go through
   `src/video_core/nx2_debug_control.h`, a small mutex-guarded block with one atomic "something
   is pending" flag. `RasterizerVulkan::TickFrame` (`src/video_core/renderer_vulkan/vk_rasterizer.cpp:1905`)
   checks the flag and takes the requests at the end of each presented frame. Between frames no
   draw is half-recorded and no pipeline is in use by the caller, so dropping cache entries there
   is safe.
3. **Existing renderer hooks.** The screenshot uses `RendererBase::RequestScreenshot`
   (`src/video_core/renderer_base.cpp:38`), the path the Qt screenshot hotkey uses
   (`src/lichii/bootmanager.cpp:819`). It is already safe to call from another thread, and the
   renderer fills the buffer at the next present (`renderer_vulkan.cpp:350`).

The server thread waits for rasterizer-thread work with a timeout (5 s for screenshots and
rejections, 120 s by default for a capture). If the game is paused or loading, no frame is
presented and the tool fails with "no frame was presented within N s (paused or loading?)"
instead of hanging. A withdrawn capture request is removed from the queue. On shutdown the
service ends the session first, which wakes every waiter, then stops the `io_context`, so
shutdown waits at most about 50 ms for a handler.

Cost when unused: if `EDEN_NX2_DEBUG_SERVER` is unset, `MaybeStartService` returns at its first
`getenv`. No thread, no socket, no log ring. The per-frame check is one relaxed atomic load,
and each guest present stores a steady-clock timestamp in the 4096-entry ring that
`frame_times` and the bug bundle read. The runtime reject check in the lowering is one relaxed atomic load per lowered program. When the
server is on but idle, its thread sleeps in `epoll`/IOCP.

## Safety

- **Loopback only.** `ParseEndpoint` accepts `127.0.0.1`, `::1` and `localhost` and refuses
  every other host, `0.0.0.0` included. The server is reachable only from the PC itself, where
  the agent already has a shell.
- **Off by default.** It needs `EDEN_NX2_DEBUG_SERVER`, which the owner's shortcut sets for a
  debug build. Release packages are unchanged unless someone sets it.
- **Writes only under the dump directory.** `EDEN_NX2_DEBUG_DIR`, by default
  `<log dir>/debug-server`. Clients never send paths. The server names every file itself:
  `capture-<session>-<n>/`, `screenshot-<session>-<n>.png`.
- **Nothing the server returns comes from game files, keys or firmware.** It returns rendered
  pixels, log lines, frame-dump manifests and shader hashes, which the owner already sends today.
  The memory tools read only the application's guest virtual memory and GPU virtual memory,
  through their page tables, and never write either. They cannot reach host memory, the key area
  or the file system: an address resolves only through a page the guest has mapped. The adapter's `capture_image` reads only PNGs inside
  capture directories that it created in this session, and checks for path escapes.
- **Logs.** `log_tail` returns lines that already go to `eden_log.txt`.
- **No authentication.** Any local process may connect, the same as with the GDB stub. That is
  acceptable on the owner's single-user PC with the switch off by default. If it ever runs on a
  shared machine, add a token file in the dump directory that the adapter reads.

## Tool surface, by how much each cuts the loop

"Today" is the cost of answering the question without the server.

| Tool | Today | With the server | Phase |
|---|---|---|---|
| `status` | Ask the owner what is on screen, read the log header | One call: title, program id, build, paused, presents, fps, renderer, rejected hashes | 1 |
| `frame_times` | Frame trace and manual interval analysis | Guest-present interval statistics and VSync histogram over a timed window | 1 |
| `screenshot` | Owner takes and sends a screenshot | PNG of the next presented frame, as MCP image content | 1 |
| `log_tail`, `log_filter` | Relaunch with a new filter, owner sends the log | Read new lines since the last call, change the filter live | 1 |
| `capture_frame` | Relaunch with `EDEN_NX2_FRAME_DUMP`, owner touches `capture.now`, agent copies the dump | Capture the next frame now, sampled textures and bound buffers optional, get the manifest summary back; `capture_image` returns any pass PNG | 1 |
| `capture_bundle` | Owner zips logs, settings and screenshots by hand | The Report a Bug bundle: logs, settings, screenshot, frame times and a frame capture with shaders, in one zip | 1 |
| `reject_shaders` | Rebuild the env, relaunch, replay to the spot, once per bisection step | Reject or restore hashes live; bisection becomes a loop of reject, screenshot | 1 |
| `pause`, `resume`, `step_frames` | Not possible | Freeze on the broken frame, capture it, step one present | 2 |
| `input`, `input_sequence` (buttons, sticks, frame-timed sequences) | Owner plays to the spot | Agent walks to the spot itself and replays a repro (done) | 2 |
| `shaders` (inventory) | Shader dump and offline sweep | List loaded programs: key, hash, stage, address, lowered or rejected with the error | 2 |
| `switches` (live `EDEN_NX2_*`) | Relaunch per switch | Toggle the observer switches live (classification below) | 2 |
| `read_memory`, `search_memory`, `read_gpu_memory` | GDB stub session or log prints plus a rebuild | Read a range by address, find known values in the heap (done) | 3 |
| `dump_texture`, `dump_buffer` | Frame dump of the whole frame | One resource by GPU address, as PNG plus raw | 3 |
| `gpu_timings` | `EDEN_NX2_GPU_PROFILE` relaunch, zones only | Per-pass GPU time of one frame | 3 |
| Capture options (pass range, shader filter) | Not possible | Smaller captures, for example only passes 40 to 60 | 3 |
| Save and load state | Not possible | Not possible: see "Save states" | none |

### Phase 1 (implemented)

**`status`** `{fps_window_ms?}`. It reads the title and program id that `Load` saw, the build from
`Common::g_build_fullname` and `g_scm_rev`, and `System::IsPaused`. It measures fps from
`Core::g_nx2_present_count` (`src/core/core.cpp:78`) over a short window. It does not call
`GetAndResetPerfStats`, which would reset the Qt status bar's counters.

**`frame_times`** `{seconds?, refresh_hz?}` waits for `seconds` (default 5, > 0, at most 60),
interruptible on shutdown like `status`. It measures intervals between guest presents inside
that window, not display scanout or GPU execution time. `frames` counts those intervals;
`mean_ms`, `fps` (1000 / mean), `p50_ms`, `p90_ms`, `p99_ms`, `min_ms`, `max_ms` describe them.
Percentiles interpolate between sorted samples. `vsync_buckets` gives fractions (0 to 1) under
keys `1`, `2`, `3`, `4+`, using boundaries 1.5, 2.5 and 3.5 refresh intervals (`refresh_hz`
defaults to 60 and must be positive). `stutters` counts intervals strictly above twice the
median. `intervals_ms` contains the last up to 120 intervals in chronological order, rounded
to 0.1 ms. With fewer than two presents, statistics are zero and the interval list is empty.
Only available samples from the 4096-timestamp ring are used; overflow sets `truncated: true`.
For example: `{"method":"frame_times","params":{"seconds":5,"refresh_hz":60}}`.

**`screenshot`** `{height?, save?, timeout_s?}`. It requests a 16:9 layout from
`Layout::DefaultFrameLayout`, and the renderer blits the presented framebuffers into it. The
BGRA result is turned into RGB and encoded as PNG with stb, which `common` already compiles. The
pixel buffer is owned by the completion callback, so a request that times out cannot leave the
renderer writing into freed memory. If the Qt screenshot hotkey has a request pending, the tool
fails instead of taking over the hotkey's request. The answer has `present`: the guest present
count of the frame it captured. Each guest present queues one composite
(`GPU::RequestComposite`, called from nx2:vi2 229 right after the count). The GPU impl takes the
count there and stores it in `g_nx2_composite_present` before `renderer->Composite`. The
screenshot wrapper (`RendererBase::RequestScreenshot`) runs inside that composite, copies the value
before it starts the callback thread, and the tool reads it after the callback
(`RendererBase::ScreenshotPresent`). A Qt hotkey screenshot that completes between the two would
overwrite it; the debug server itself is serial. It is 0 for games that never count presents.

**`features`** `{}` answers `{features: [...], kit}`: what this build supports beyond origin.
An older exe ignores parameters it does not know (`input_sequence {start_present: N}` then starts
at the next present), so a client reads `features` first. An exe without the method answers
"unknown method": it is older than the list. The names are `input_ms` (`ms` steps), `measure`,
`input_record`, `record_mode` (`mode` any or sample), `start_present`, `landed_present`,
`screenshot_present`, `luma_watch` and `npad_sync`. `kit` is 2 (since `npad_sync`) and rises
when the contract of a listed feature changes. `status` stays as origin's.

**`luma_watch`** `{presents?, cancel?, wait_s?}` finds a load's fade-in exactly, after the fact.
`{presents: N}` (1 to 3600) arms the next N composites: each one renders a 256x144 copy of the
frame through the screenshot path (`RenderLumaSample` next to `RenderScreenshot` in both
renderers) and keeps `[present, mean luma]`, with the composite's guest present (as `screenshot`)
and Rec. 601 luma from 0 to 255. The watch disarms itself after N samples. It answers at once:
`{armed: N, present}`, where `present` is the present count now; composites can run a frame or
two behind it. `{}` reads `{samples: [[present, luma], ...], remaining, done}`; `wait_s` (0 to 120)
waits for `done` first. `{cancel: true}` disarms. Arming again replaces the samples, and the end
of the session disarms.

While armed it stalls the GPU thread at every composite: `RenderToBuffer` ends with
`scheduler.Finish()`, so each present waits for the GPU and costs some ms more. Use it only outside
measured input, around a load and its fade-in. With the game's FPS add-ons off, the game advances
one step per present, so a slower present there changes no game state. Disarmed, the cost is one
relaxed atomic load per composite. Composites of a minimized Vulkan window take no sample
(`RendererVulkan::Composite` returns before the screenshot when the window is not shown), so the
watch waits until the window is shown again. Typical use: arm before the walk into a loading zone,
read with `wait_s` after the fade-in, take the first present after black (mean luma below 8)
whose next samples are not black either, then `input_sequence` with `start_present` set from
it. `cancel` as soon as the anchor is found keeps the stalls short.

**`log_tail`** `{after?, limit?, contains?}`. A `RingBackend` in `src/common/logging.cpp:308`
keeps the last 20,000 formatted lines. It is an ordinary backend that `FmtLogMessageImpl` already
fans out to (`logging.cpp:488`), and it does nothing while its capacity is 0, which is the
default. The service turns it on at boot. A call without `after` returns the newest lines.
`next` pages forward, so an agent can poll "everything since my last look".

**`log_filter`** `{filter}`. It parses the filter with `Filter::ParseFilterString` and applies
it with `SetGlobalFilter` (`logging.cpp:449`), the call the Qt settings dialog makes. Lines
already filtered out were never formatted, so the new filter applies only to later lines.

**`capture_frame`** `{sampled?, buffers?, timeout_s?, manifest_lines?, index?}`. It reuses
`FrameDump`. `FrameDump::Arm` (`vk_frame_dump.cpp:268`) points the next capture at a fresh directory and resets
the per-capture counters, so `EDEN_NX2_FRAME_DUMP` is not needed. When to capture is decided by
`FrameDumpSchedule`, which has no Vulkan state and is unit-tested. It handles both the env
triggers and the debug server's requests. It also sets `RecordingSampledTextures`, which the
graphics and compute pipelines check before they keep the textures each draw and dispatch
samples. The pipelines used to read `EDEN_NX2_FRAME_DUMP_SAMPLED` once at startup, so a sampled
request at runtime found no sampled textures to read back. Now the pipelines keep them only during
a sampled capture, under either trigger. With `buffers: true` the capture also records the uniform
and storage buffers the buffer cache bound for every draw and dispatch (`kind: buffer`, with stage,
binding, GPU address and size; bytes deduplicated in `buffer_<hash>.bin.zst`, copied on the GPU
when the GPU wrote the range) and each dispatch's group counts (`kind: launch`). This is the
buffer part of fix/oot-fog's frame dump (1ad14bebb8). `EDEN_NX2_FRAME_DUMP_SAMPLED` turns on both
textures and buffers. At runtime they are separate, so diffing a constant buffer such as a fog
`c5` across two captures does not pay for every texture readback. `FrameDump::TickFrame`
(`vk_frame_dump.cpp:849`) starts the capture exactly as `capture.now` would. The writer thread
reports completion after it writes the manifest summary (`WriterLoop`, `vk_frame_dump.cpp:710`). The tool returns the directory, the
frame and summary lines, counts per entry kind, the first entries, and the `frame_dump_index.py`
command. With `index: true`, the adapter runs that command itself. The adapter-only
`capture_image {directory, name}` returns one pass PNG as image content.

**`sample`** `{threads?, seconds?, max_chars?}`. The in-process sampling profiler
(`src/common/nx2_sampler.cpp`, also `EDEN_NX2_SAMPLE` at startup) on demand: it pauses the named
threads (default `GPU,GPUBack`) about 1000 times a second for `seconds` (default 10) and returns
the self, self-and-caller and inclusive counts per function. The report and its folded stacks go
to `samples-<session>-<n>.txt` in the dump directory. Windows only; the threads run slower while
sampled, so read fps from a run without it. A stack ends at its first frame in generated code,
shown as `(jit)`: a guest core thread (`CPUCore_*`) shows `(jit)` while it runs guest code and
the host functions above `(jit)` while the JIT calls out.

**`speed_limit`** `{on?}`. Lifts (`false`) or restores (`true`) the speed limit live and returns
the `previous` and current `on` state, so a test script can boot through the logos and menus
unlimited. Without `on` it only reports the state. Use it through `--call`; the MCP adapter does
not list it.

**`host_timing`** `{stats?}`. Turns the per-second `HostTiming wait` log line
(`EDEN_NX2_HOSTTIMING_STATS`) on or off and returns the `previous` and current `stats` state.
Read the lines with `log_tail` (`contains: "HostTiming wait"`).

**`capture_bundle`** `{description?, size_limit_mib?, timeout_s?}` (`index?` in the adapter). It
writes the bug-report bundle that Help → Report a Bug writes ([bug-bundle.md](bug-bundle.md)):
logs, system and settings, screenshot, the last 10 s of frame times, and the next frame captured
with its shaders, SPIR-V and pipeline keys, zipped under `size_limit_mib` (default 19, 1 to 100)
into `<log dir>/bug-reports`. It waits up to `timeout_s` (default 90) for the frame capture and
returns `path`, `bytes`, `dropped`, `frame_error`, `manifest` (the bundle's `bundle.json`) and
`index_command`. In the adapter, `index` runs `bundle_index.py` on the zip and returns its
`index.html`.

**`reject_shaders`** `{hashes?, reject?, timeout_s?}`. This is `EDEN_NX2_REJECT_HASH` at
runtime: the same keys (native program key or shader hash) and the same effect. Pipeline
invalidation turned out to be tractable:

1. The lowering checks the runtime list right after the env list
   (`src/video_core/shader_environment.cpp:642` and the block after it). A rejected program
   throws the same `Shader::RuntimeError`, which the existing rejection path handles: the draw is
   skipped, the error is logged once, and the address is remembered.
2. `ShaderCache::RefreshDebugRejections` (`src/video_core/shader_cache.cpp:67`) runs on the
   rasterizer thread. It finds cached `ShaderInfo`s of lowered native programs whose
   `unique_hash` or `native_program_key` is on the list. It drops them through the same
   `InvalidatePagesInRegion` and `RemovePendingShaders` path (`shader_cache.cpp:369`, `:390`)
   that guest writes to shader memory take. For native code `unique_hash` is the shader hash
   (`GenericEnvironment::Analyze` returns `NativeShaderHash(code)`).
3. It sets `Dirty::Shaders` on every active channel. `RefreshStages` returns early while that
   flag is clear (`shader_cache.cpp:120`), and its `shader_infos` would still point at the
   dropped entries. With the flag set, the next draw looks the program up again, lowers it again,
   and hits the rejection. Compute looks up its program on every dispatch
   (`ComputeShader`, `shader_cache.cpp:254`), so it needs no flag.
4. After an un-reject, every remembered rejection (`rejected_native`) is cleared, so programs
   rejected earlier are lowered again. Programs that really fail are rejected again and logged
   once. The lowered result is identical, so the old pipeline still sits in the pipeline cache
   under the same key and is reused: restoring costs nothing.

The tool waits until the rasterizer applies the change and reports `applied`. A bisection is a
loop: reject half of the candidates, screenshot, compare. Today each step is a rebuild of the
environment plus a relaunch plus a replay.

### Guest and GPU memory (implemented)

All three are read only. Reads return at most 64 KiB. Addresses are hex strings (`"0x8012345000"`)
or JSON numbers, and results give them back as hex strings.

**`read_memory`** `{address, size, as?}` reads the application process's guest virtual memory.
`as` is `hex` (default, one string), `u32`, `u64` (hex strings, since JSON numbers lose bits
past 2^53) or `f32` (`"nan"`, `"inf"` and `"-inf"` for non-finite values). If any page of the
range is unmapped, the call fails and names the first unmapped address.

**`search_memory`** `{pattern, start?, end?, max_hits?, regions?, budget_ms?}` finds a byte
pattern (hex pairs in memory order, so little endian: `1.0f` is `0000803f`; up to 256 bytes) in
mapped guest memory. `regions` is `heap` (default, the heap region from the process page table)
or `all` (the whole address space); `start` and `end` narrow it. Matches may cross page
boundaries but not unmapped gaps. It returns at most `max_hits` (default 64, at most 256)
addresses. It stops after `budget_ms` (default 2000, at most 10000) and then returns
`complete: false` and `next`. Pass `next` as `start` to continue without missing or repeating a
match. The intended use is to find a known float tuple, such as the fog layer constants a
capture shows in `c5`, and from there the time-of-day or dynamic-resolution state. A heap scan
copies every mapped page once, so it keeps one host core busy for up to the budget: run it once,
not in a loop, while the owner plays.

**`read_gpu_memory`** `{gpu_address, size, as?, timeout_s?}` reads GPU virtual memory, for
example query report and semaphore buffers that the game reads back. It runs on the rasterizer
thread at the next frame end, like a capture, so it fails with "no frame was presented" while
the game is paused or loading. It tries the bound channel's address space first, then every
other address space the rasterizer knows, and reads from the first one that maps the whole range
(`address_space` in the result is `Tegra::MemoryManager::GetID`). Otherwise it names the first
unmapped address in the bound channel's address space.

How the reads are safe:

- **Guest memory, on the server thread.** `ReadGuestMemory` (`nx2_debug_tools.cpp`) resolves
  each page with `Memory::GetPointerSilent` (`src/core/memory.cpp:1073`). That is the lock-free
  lookup the guest CPU cores use: one relaxed atomic load of the page entry. Through
  `SparseLargeVector::GetOrDefault` (`src/common/sparse_large_vector.h:91`), an entry that was
  never committed reads as unmapped, so the lookup never commits the sparse table. It never logs,
  and it never calls `HandleRasterizerDownload`. `ReadBlock` would do all three, and its GPU
  flush may only run on the rasterizer thread. A mapped entry always points into the
  `DeviceMemory` backing, which stays mapped and readable for the whole boot (`SEC_COMMIT` on
  Windows, `src/common/host_memory.cpp:153`; a shared memfd mapping elsewhere). If the game
  unmaps a page during the copy, the read returns stale bytes instead of faulting.
  `GetPointerSilent` masks the top 16 bits, so addresses at or above 2^48 count as unmapped
  instead of aliasing low memory. The service stops before the process is destroyed, so the
  page table outlives every read.
- **What a guest read can show.** Guest threads keep running, so a value can change, or tear,
  while it is copied. Read it twice when that matters. On pages the GPU caches track
  (`RasterizerCachedMemory`), the read returns the CPU copy: GPU writes that are not written back
  yet are missing. `capture_frame` with `buffers` shows the GPU side.
- **GPU memory, on the rasterizer thread.** Each `Tegra::MemoryManager` belongs to an nvdrv
  address space that the GPU thread uses for every draw. The server thread cannot know whether an
  address space is still alive, so it does not touch them. The request goes through
  `nx2_debug_control.h` like a capture. `TickFrame` collects the bound `gpu_memory` and the
  `address_spaces` under `config_mutex`, then calls `IsFullyMappedRange` and `ReadBlockUnsafe`
  (`src/video_core/memory_manager.cpp:742`, `:474`), the reads the GPU itself makes. It does not
  flush the GPU caches, so the result is what guest memory holds, not data that is still only in
  GPU buffers.

### Input (implemented)

**`input`** `{buttons?, left_stick?, right_stick?, frames, timeout_s?, measure?}` holds the buttons and
sticks for `frames` presented frames and then releases them. It returns when they are done.
**`input_sequence`** `{steps: [{buttons?, left_stick?, right_stick?, frames}, ...], timeout_s?, measure?}`
runs the steps back to back, at most 6000 steps and 36000 frames per call. A step with no
buttons and no sticks injects nothing, so it works as a pause between moves.

- **Buttons** are `A B X Y L R ZL ZR Plus Minus Up Down Left Right StickL StickR` (the
  `NpadButton` names). Home and Capture are not offered: they would leave the game. Unknown
  names and unknown step keys are refused, so a typo fails instead of doing nothing.
- **Sticks** are `[x, y]` in -1..1, up is +y, scaled by `HID_JOYSTICK_MAX`.

Where it hooks in: `EmulatedController::SetInputInjector` installs a function that
`GetNpadButtons` and `GetSticks` call while it is set
(`src/hid_core/frontend/emulated_controller.cpp`). The npad service reads those two getters on
every update (`src/hid_core/resources/npad/npad.cpp:449`). Injected buttons are ORed into the
owner's, and an injected stick replaces the owner's stick and its `StickL*` or `StickR*`
direction bits (threshold half of full scale, like a real stick). It changes no mapping, profile
or setting, so it works with whatever the owner has mapped and leaves nothing behind. The input
goes to player 1, or to the handheld controller when player 1 is not connected; the result names
which. When no injector is installed, each getter costs one atomic load.

Timing is by presents, not wall time. Switch 1 advances the counter when a newly acquired,
visible game buffer is composed; overlay-only updates and cached-buffer repeats do not count.
Switch 2 advances it on vi2 present calls. Generated display frames do not count.
`RunInput` starts the script at `g_nx2_present_count + 1`,
the next present. The injector looks up the active step from the present count at every read
(`InputAt`), so a step changes at the first npad update after its present: the npad updates
every 1 ms (`src/hid_core/resource_manager.cpp:48`). A slow frame stretches the step in time but
not in frames. Games that integrate movement over time can therefore follow different paths
when replay frame times differ.

Steps may give `ms` instead of `frames` (one unit per call, at most 600000 ms in total). Timed
steps follow host `steady_clock` time from the moment the injector is installed, for games that
advance by elapsed time: with present-counted steps such a game moves further when frames are
slower (OOT moved Link visibly less in a run whose 90 held presents took 4.33 s instead of
4.93 s). A timed result reports `clock: "host_time"`, `ms`, and as `frames` the presents that
began inside the window except the last; `measure` then covers those presents (2 to 3840),
otherwise the measurement is marked incomplete. Timed input still quantizes to the game's own
input reads, so it narrows run-to-run drift but is not a frame-exact replay.
Host time continues during pauses and hangs; completion of a timed call does not prove the
game advanced. Reject incomplete measurements and failed scene checkpoints.
For timed calls, `timeout_s` must exceed the requested duration. Measured timed calls return
`head_ms`, `tail_ms`, and `window_ms` when their present snapshot is complete. The head and tail
are the time before the first and after the last measured present; they are excluded from
`intervals_ms` and its FPS/quantiles. `complete` describes the present snapshot, not full-window
coverage. Include these edges when assessing stalls, and report interval coverage separately
from elapsed-window throughput. A window with too few or too many presents returns incomplete
measurement statistics, not evidence of smooth playback.

**`start_present`** (frame steps only, `input` and `input_sequence`) puts step 0 at exactly
that present instead of the next one. The call installs the injector at once with
`start = start_present`; the injector answers by the present count and gives nothing before
`start` (`InputAt`), so the npad reads of that present already get step 0. No extra work runs on
the present path. The call fails with "start_present N has passed" when the game is already at N
or later, and refuses more than 36000 presents of lead. If present N is counted while the
injector is being installed, the call fails with "present advanced while installing input" and
releases the input. The default `timeout_s` counts the lead too. The adapter does not know the
lead, so with `start_present` and no `timeout_s` it waits up to 900 s; a client with its own
socket timeout must add the lead.

Frame-step calls with `start_present` or `measure` also answer `landed_present`: the first
present whose whole count had the injector. These calls refuse a start present that was counted
while the injector was being installed, so `landed_present == first_present` whenever they
succeed. A harness proves a landing error of 0 by checking `landed_present == start_present`. The
plain reply stays as origin's.

The same calls answer `stale_presents`: the offsets from `first_present` whose newest npad entry
at the present's reply was written before the present was counted. The game reads the newest
entry of the npad LIFO, not the injector. The npad updater writes that entry on its own 1 ms
core-timing event, which in multicore runs on the timer thread by host time
(`core_timing.cpp`), not in step with the game. The game resumes and reads after the reply of
present c (`g_nx2_present_reply_hook`). When the time from count to reply is shorter than the
wait for the next update, the newest entry still holds step c - 1, and the game gets the old
input for one more frame. With the stamps on (only while a frame-step call or a recording in mode
`"any"` runs), `ResourceManager::UpdateNpad` stores the host time of each update's start
(`HID::g_npad_update_ns`) once its entries are written, so an update still running counts as not
done. The reply hook compares it with the present's count time in `g_nx2_present_times_ns`. An
update that began just before the count but read after it is counted as stale too, so the list is
an upper bound.

**`npad_sync`** (default true, any frame-step `input` or `input_sequence`) removes that race:
at the reply of each covered present, the reply hook runs one npad update itself
(`HID::UpdateNpadNow`, i.e. `ResourceManager::UpdateNpad`) before the game resumes. It takes the
npad lock the 1 ms update takes, and each entry gets the next sampling number, so the game sees
one more normal entry, the newest, with the present's step. The cost is one npad update per
present (microseconds), only while a call runs. Side effects: the turbo-button phase advances one
extra step per present. The stale check runs before the update, so `stale_presents` still shows
where the race would have hit. `"npad_sync": false` turns the update off for an A/B; calls with
`measure` or `start_present` echo `npad_sync`.

Release: the injector returns nothing from the end of the last step on, so the input ends on its
exact frame even before the handler removes it. The handler removes it on every exit path
(`SCOPE_EXIT`): when the steps are done; on timeout (default `frames / 20 + 10` or
`ms / 1000 + 10` seconds, at most 900); when the game shuts down; and when the client disconnects. The
transport gives every handler a `ClientGone` probe, a non-blocking peek at the request's socket
(`nx2_debug_server.cpp`), which the wait polls every 2 ms. The adapter waits as long as the
emulator's default timeout, so it does not drop a long sequence early.

With `measure: true`, input calls are limited to 3840 presents and additionally return
`measurement.supported: "held-input-v1"`. For `first_present = start` and `frames = total`,
only timestamps `start..start+total-1` are used; the response waits for counter
`>= start+total`. There are exactly `total-1` intervals (zero for one present).
`intervals_ms` contains every interval at full precision, with statistics, inclusive
`first_present`/`last_present`, `expected_intervals`, `covered_presents`, snapshot counter
bounds, `complete` and `truncated`. Invalid/pending/overwritten timestamps produce incomplete
coverage and an empty interval list; consumers must reject them. For frame-counted input, a
present advancing while the injector is being installed rejects the measured call and releases input.
The default nonmeasuring response is unchanged. Debug dispatch remains serial: no parallel
`frame_times` request is needed. Screenshots taken after responses lie outside the measurement.
Multiple chunks omit their cross-chunk intervals; pooled quantiles are not a continuous
wall-time route measurement. These are host guest-present timestamps, not GPU execution times.

Use it when the owner has put the controller down: injected buttons add to whatever the owner
presses. For an A/B test, run the same `input_sequence` from the same spot under each variant
(for example with a `reject_shaders` change in between), then compare screenshots or captures.

### Input recording (implemented)

**`input_record`** `{action: "start" | "status" | "stop", mode?}` records the input the game is given
at each present, so that `input_sequence` can replay it present for present. The frontend TAS
recorder steps once per frame shown in the window (`GRenderWindow::OnFrameDisplayed`), not once
per game present, so its clips drift when replayed with `input_sequence` (H48).

| Action | Result |
| --- | --- |
| `start` | `{recording: true, start_present, controller, mode, npad_sync}`. `mode` is `"any"` (default) or `"sample"`, see below. `npad_sync` (default true, mode `"any"` only) runs one npad update at each present's reply, as replays do. Fails while a recording runs. |
| `status` | `{recording: false, controllers}`, or `{recording: true, start_present, presents, steps, controller, mode, truncated, controllers}`, where `steps` is a count. `controllers` has one entry for player 1 and one for the handheld controller: `{npad, connected, style, input}`, where `style` is the `NpadStyleIndex` and `input` is true when it gives buttons or a stick now. |
| `stop` | `{steps: [...], presents, start_present, end_present, controller, mode, npad_sync, truncated}`, and in mode `"any"` also `steps_at_count` and `blocked_us: {presents, mean, max}`. Fails when nothing records. The count still running is closed with what it had so far. |

- `controller` is the controller a replay goes to: `player1`, or `handheld` when player 1 is
  not connected, the same choice `input` makes, made once at `start`. The recording itself reads
  both (see below).
- `start_present` is `g_nx2_present_count + 1` at `start`, the next present.
  `end_present = start_present + presents - 1`. With `presents: 0`, `steps` is empty.
- `steps` are frame steps in the `input_sequence` format, run-length encoded. They sum to
  `presents` frames. Each step has `left_stick` and `right_stick`, also when centred, and
  `buttons` when any of the 16 input buttons are held. Stick values have five decimals, which
  `ParseInputStep` rounds back to the exact recorded value.
- Pass `steps` to **`input_sequence`** (not `input`, which takes one step). The step at offset k
  of the recording plays at offset k of the replay: the game gets the recorded input of present
  `start_present + k` at present `first_present + k` of the replay.
- Replay with `measure: true`. Without it, a present that lands while the injector is installed
  gets the owner's input instead of step 0. With it, that call fails with "present advanced
  while installing input" and releases the input; start the replay again from the save.

Where it samples: `g_nx2_present_hook` is set only while recording. nx2:vi2 229
(`src/core/hle/service/nx2_gsv.h`) calls it right after it counts present c, on the present
thread. It reads `GetNpadButtons` and `GetSticks` of player 1 and of the handheld controller,
each while it is connected: the getters the npad updater reads every 1 ms. The buttons are ORed
and each stick keeps the larger deflection. Both are read because they have separate bindings
(`players[0]` and `players[8]`) and the npad updater gives the game every connected npad
(`npad.cpp`, `RequestPadStateUpdate`): in handheld mode the owner's pad can drive the handheld
npad while player 1 is connected and idle. The first live clips (v1.0.0a-20261109-10) read player
1 only and lost the owner's input after 400 to 1900 presents. The injector also answers them by
the present count, so for a replay the input read at present c is exactly what the game gets
during count c.

Mode `"any"` (the default, and the hotkey's) also holds every button the game was given at any
time during count c, from present c to present c + 1. The npad updater writes an entry for the
game every 1 ms (`resource_manager.cpp`, `npad_update_ns`) into a 17-entry LIFO, about 28 entries
per present at 35 FPS, and the owner's sword taps (Y) are often one present long or shorter. A
sample at the start of the count alone missed such taps: a clip held 5 Y presses where the owner
swung at least 6 times. While mode `"any"` records, `RequestPadStateUpdate` (`npad.cpp`) calls
`HID::g_npad_read_hook` for each controller it reads for the game. The hook reads the same two
getters and ORs the buttons into count c, but only when the present count is c before and after
its read: a read that may belong to count c + 1 is left out, so scripted input of the next count
never leaks into this one. Count c is closed into the recording at present c + 1, or at `stop`.
Sticks in mode `"any"` are the value at the REPLY of present c. The game is blocked inside
nx2:vi2 229 from the count until the reply (the previous frame's fence, the composite, vblank
pacing; `PresentClientBuffer` in `nx2_gsv.h`), and it reads its next input only after the reply.
A sample when the count goes up is therefore early by that whole time, and a moving stick then
replays older than what the game read: long clips ended a stable 4 to 10 px off the owner's end
point. `g_nx2_present_reply_hook` runs just before the reply; a count whose reply did not come
(the last one at `stop`) keeps its start value. The replay holds one stick value per count, so
the reply value is what a game that reads input at the start of its frame gets. The largest
deflection would turn a short flick into a whole present at full tilt, and the last read of the
count is late by almost a whole frame of game logic.

With `npad_sync` the recorder's reply sample is the values that npad update wrote (the npad read
hook passes them), so a step holds exactly the newest entry the game read next, and a replay with `npad_sync` gives the game
the same entry: record and replay describe the same thing. A recording with `npad_sync: false`
(and every recording made before kit 2) can differ from what the game read by up to 1 ms of
input plus the timer thread's lateness.

A harness replays `steps`. `steps_at_count` is TEMPORARY: the same presents and buttons with the
stick at the count (the phase before 2026-11-10), for one A/B replay against `steps`. It is
dropped after that A/B. Both lists keep the step limit; `truncated` is true when either reached
it, and only then can their frame sums differ. `blocked_us` is the time from count to reply over
the presents that had a reply, in microseconds: the stick phase error of a recording made before
this change. Mode `"sample"` has neither: it takes the stick at the count, as before.

Trade-off: if the game reads only the latest entry once per frame, mode `"any"` can add a press
the game never saw (a press that began and ended between two of its reads). Which entries a game
reads is not visible to the emulator: it reads the LIFO with its own code in shared memory, with
no service call. Mode `"sample"` is the earlier behaviour: the input at the moment of present c
only, so a press that starts later during count c shows from present c + 1. Use both on one
fight clip to compare. When not recording, the cost is one atomic load per present and one per
npad update.

What is kept: the 16 input buttons and both sticks, after turbo and after any injected input.
Stick direction bits (`StickL*`, `StickR*`) are not kept: the replay derives them from the
recorded sticks, with the same half-scale threshold. Other buttons (Home, Capture, SL, SR) are
not kept.

Replay notes:

- The replay sets both sticks at every present, so a recorded clip replaces the owner's sticks.
  Injected buttons are still ORed into the owner's, so leave the pad alone during a replay.
- The game gets the same input at the same present offsets. It reaches the same positions only
  if each present advances the game by the same step, for example with the game's FPS add-ons
  off. A game that moves by elapsed time drifts with frame times (see "Input").
- One call replays at most 6000 steps and 36000 frames. A recording stops adding presents when
  either limit is reached and reports `truncated: true`; `stop` then returns what it has. 6000
  steps cover 100 s of a stick that moves at every present at 60 presents/s. A 6000-step
  request with both sticks and two buttons per step is about 0.7 MB of the 1 MB request limit.
  Steps that hold many buttons at once are longer: check the request size before a long replay.
- `measure: true` covers at most 3840 presents (64 s at 60 presents/s): the present-time ring
  has 4096 slots, less 256 of margin.

Hotkey: **Record Input (Per Present)** (default Ctrl+F8, Configure > Hotkeys) starts a
recording, and pressed again stops it and saves the `stop` reply to the TAS folder
(`<user dir>/tas/present-record-<n>.json`, the first free n). It works only while a game runs with
`EDEN_NX2_DEBUG_SERVER` set. The status bar shows `REC INPUT <presents> presents` while any
recording runs, also one the debug server started. After the stop it shows `Input recording of
<n> presents saved to <path>` for 10 s, at the left of the status bar (the label's tooltip has the
whole path when the bar cuts it). Until 2026-11-10 this text did not show: it went to
`QStatusBar::showMessage`, whose space the status bar buttons cover. The
hotkey and the debug server use the same recording: a `stop` from either one ends it. Leaving the
game discards a running recording, so stop it first. A controller combo can be set for it in
Configure > Hotkeys. Controller hotkeys read player 1 only (`MainWindow::LinkActionShortcut`), and
every button of the combo except Home and Capture also goes to the game and into the recording.

Threading: the hook, the handler and shutdown share one mutex. A recording is not tied to a
client connection, so `start` and `stop` can come from different calls. A client that leaves
mid-recording leaves it running until the limits, and the next `stop` ends it. When the game
shuts down, `Service` discards the recording after the server thread has stopped, so the
present path no longer reads the controller.

### Phase 2: control and input (about 4 to 5 days)

- **pause, resume, step_frames.** The core has `System::Pause` and `Run` (`src/core/core.cpp:279`).
  The frontends own the run state, though: Qt's `EmuThread` decides when to call `Run`, so a
  server that called `Pause` directly would desynchronise the UI. The frontend registers
  pause and resume callbacks with the service, and Qt implements them with the same slot its
  pause button uses. `step_frames {n}` resumes, waits until `g_nx2_present_count` advances by
  `n`, then pauses. That is present-granular, not exact, because the GPU drains a frame or two
  behind the CPU. Use a capture to get an exact frame.
- **input** and **input_sequence** are done; see "Input".
- **shaders.** An inventory from `ShaderCache::lookup_cache` (key, hash, size, address) and
  `rejected_native` (address plus the exception message), copied on the rasterizer thread like
  the reject change. Each entry carries its stage and whether it was lowered, rejected by the
  lowering, or rejected by a hash.
- **switches.** A registry that replaces the `static const bool x = std::getenv(...)` pattern for
  switches that can change safely. It keeps the env var as the initial value, and the getter
  becomes a relaxed atomic load.

Live switch classification, from the switches in `src/`:

| Class | Switches | Live? |
|---|---|---|
| Observers (trace and log only) | `RENDER_TRACE`, `IPC_TRACE`, `IPC_SUMMARY`, `VDEC_TRACE`, `GPU_PROFILE`, `DLSS_CONTRACT_TRACE`, `QUERY_TRACE`, `METHOD_TRACE`, `SVC_TRACE`, `MEM_TRACE`, `PULLER_TRACE`, `QMD_TRACE`, `CB24_TRACE`, `RT_TRACE`, `TC_LOG`, `MME_LOG`, `WINDOW_TRACE`, `FPS_LOG`, `PB_CHECK`, `SPLIT_CHECK`, `EDEN_DKB_FRAME_TRACE` | Yes. Flip at a frame end. Some keep per-frame counters (`render_trace_frame`) that must reset when turned on. `GPU_PROFILE` is an `inline const bool` (`src/video_core/nx2_profile.h:74`) read by every zone; it becomes an atomic. |
| Dumps | `SHADER_DUMP`, `VDEC_DUMP`, `TEXTURE_DUMP`, `PB_DUMP`, `MME_DUMP` | Yes, as on and off only, writing into a subdirectory of the debug dump directory. Clients never pass the path. |
| Behaviour, cached | `REJECT`, `REJECT_HASH` (done), `EXPERIMENTAL_GEOMETRY`, `NO_MESH`, `NO_TENSOR`, `SKIP_VS`, `UNDEF_ZERO`, `FORCE_LOD0` | With invalidation. The first group changes lowering, so it needs the reject path above for every native program. `FORCE_LOD0` changes samplers, so it needs a sampler cache flush. |
| Behaviour, A/B races | `PB_SNAPSHOT`, `FETCH_DRAIN`, `FLUSH_DRAWS`, `SYNC_SUBMIT`, `FENCE_FLUSH_US`, `IDLE_SPIN_US` | Probably, at a frame end. They change timing, not state, but the point of an A/B race test is a clean run, so a relaunch is often still the honest answer. |
| Not live | `SPLIT` (thread topology set at boot, `gpu_split.cpp:89`), `CNTFRQ` (guest-visible clock rate, set in `Load`), `VRAM_LIMIT_MIB` (device creation), `SPARSE_*` and `PRIVATE_DEVICE_RANGES` (decided when memory is mapped), `EMPTY_SYSTEM_DATA`, `AUTO_STUB` (service setup), `MME_JIT` (compiled macros), `DLSS_REPLACEMENT` and `DLSS_PRESET` (pipelines and models built at first use), `FRAMES_AHEAD`, `SWAP_INTERVAL` and `VSYNC_HZ` (pacing state in `nx2_gsv.h`) | No. Changing them mid-run either has no effect or leaves state built under the old value. |

### Phase 3: memory, resources and timing (about 1 to 1.5 weeks)

- **read_memory**, **search_memory** and **read_gpu_memory** are done; see "Guest and GPU memory".
- **dump_texture** `{gpu_address}` and **dump_buffer** `{gpu_address, size}`. Look the address
  up in the texture cache on the rasterizer thread and reuse `FrameDump::Record`'s readback,
  decode and PNG path for one image.
- **gpu_timings**. Bracket each frame-dump pass with timestamp queries. The scheduler already
  writes timestamps into a query pool for its trace (`src/video_core/renderer_vulkan/vk_scheduler.h:95`).
  Return pass index, draw range, shaders and GPU microseconds for one frame. This extends
  `EDEN_NX2_GPU_PROFILE`, which only splits CPU time on the GPU thread into zones.
- **capture options**: `pass_range` and `shader` filters in `FrameDump::BeginPass` and
  `CaptureDispatch`, so a sampled capture of a 300-pass frame does not write gigabytes.

### Save states

The core cannot save state. There is no serialisation of the kernel, services, the dynarmic and
NCE CPU state, or the GPU caches, and Eden and yuzu never had it. The `saveState` in
`src/lichii/main_window.cpp` is Qt window geometry. Adding save states is a project of its own,
not a tool. The practical substitute is a reproduction script: phase 2 input sequences, recorded
once (`EDEN_NX2_KEY_RECORD` already records keys in `eden-cli`) and replayed after boot.

## If the kit hangs

The lab harness kit (`input_record`, `start_present`, `npad_sync`, `luma_watch`, the hotkey) adds
locks and hooks to the present path, the npad update and the GPU thread. Each of its blocking
points leaves a breadcrumb (`common/nx2_kit_stall.h`): the thread, the lock, the operation, and
whether it waits for the lock or holds it, since when. The points are the recorder mutex, the
replay sync mutex, the injector mutex (`EmulatedController::Injected` and `SetInputInjector`),
the forced npad update (`UpdateNpadNow`, which waits for and then holds the npad mutex), and the
luma sample's GPU readback. The present hook, the reply hook, the npad read hook and the server's
input_record and input calls reach them through these locks. Writing a breadcrumb is two relaxed
stores, and only these kit paths write them, so a game without a kit call pays nothing.

While a recording, an input call or a luma watch runs, a watchdog thread (`KitWatchdog`) checks
the breadcrumbs every 250 ms. A wait or hold longer than `EDEN_NX2_KIT_STALL_MS` (default 2000)
gives ONE Error line (Error lines flush the log), again every 10 s while it lasts:

    KIT STALL: <thread>#<id> has waited 2.3 s for recorder mutex in RecordNpadRead; held by
    <thread>#<id> in RecordPresent for 2.4 s; present count 51234; active: record injector

Thread names come from the OS (`pthread_getname_np`; on Windows only the id). The first stall
also calls `KitStall::g_first_stall_hook` when a frontend set it (to write a minidump); nothing
sets it on the Base. At start the server logs `Debug server kit 2, kit hooks on, features [...]`,
so every log names the kit the exe has.

`EDEN_NX2_KIT_HOOKS=0` is the kill switch for a clean A/B. The server then installs none of the
kit's hooks: `input_record`, `luma_watch`, `start_present` and an explicit `npad_sync: true` fail
with an error that names the variable, frame-step calls run without the reply hook (no
`npad_sync`, no `stale_presents`), the GPU does not store the composite present, and `features`
answers `{features: ["input_ms", "measure"], kit, kit_hooks: false}`. Plain `input` and
`input_sequence` still work: their injector is origin code. One relaunch with it tells whether a
hang or a slowdown comes from the kit.

What to send to the dev orchestrator: the build name, the log with every `KIT STALL` line and
the 50 lines before the first one, the `Debug server kit` line, the last harness call (method and
parameters), and a minidump if the hang stays (see "Host crash" in AGENTS.md).

## Running it on the PC

1. Build the branch as usual:
   `bash C:/lx/build-fix.sh feat/debug-mcp debug-mcp "Any - debug server" "EDEN_NX2_DEBUG_SERVER=127.0.0.1:6545"`.
   Optionally add `EDEN_NX2_DEBUG_DIR=C:/Users/Zortos/.capy/work/lichii-debug/debug-mcp/dumps`
   so dumps land next to the build.
2. The owner launches the game from the shortcut. The log shows
   `Debug server listening on 127.0.0.1:6545, dump dir ...`.
3. The agent runs the adapter with native Windows Python. The MSYS `python` in Git Bash rejects
   `C:/` paths, so call the Windows interpreter by its full path. From bash, `--call` runs one
   request, prints its JSON result, and saves any PNG to `--out`:

   ```
   PY=/c/Users/Zortos/AppData/Local/Programs/Python/Python314/python.exe
   $PY -I C:/lx/src/tools/nx2/debug_mcp.py --server 127.0.0.1:6545 --call '{"method":"status"}'
   $PY -I C:/lx/src/tools/nx2/debug_mcp.py --server 127.0.0.1:6545 \
       --call '{"method":"screenshot","params":{"height":1080}}' --out C:/lx/shot.png
   $PY -I C:/lx/src/tools/nx2/debug_mcp.py --server 127.0.0.1:6545 \
       --call '{"method":"reject_shaders","params":{"hashes":["3ddfb1d7621c4755"]}}'
   $PY -I C:/lx/src/tools/nx2/debug_mcp.py --server 127.0.0.1:6545 \
       --call '{"method":"input_sequence","params":{"steps":[{"right_stick":[1,0],"frames":60},{"frames":30}]}}'
   ```

   The request is `{"method": <tool>, "params": {...}}`, the same format the socket uses. Adapter-only
   tools work too (`capture_image`, and `index` on `capture_frame`). On failure, `--call` prints
   `{"error": ...}` and exits with status 1. Without `--call`, the script is a stdio MCP server for
   an MCP client.

   Hashes must be all 16 hex digits. A shorter hash is refused rather than zero-padded, because a
   padded hash matches nothing. Returned paths use forward slashes.

The adapter reconnects after the game restarts. A boot through a guest relaunch stops and starts
the server again on the same port.

## Phase 1 limits

- Vulkan only for `capture_frame`, `reject_shaders` and `read_gpu_memory`. The drain point is in
  `RasterizerVulkan`. OpenGL would need the same three lines in `RasterizerOpenGL::TickFrame`,
  plus a frame dump it does not have.
- A runtime capture uses up an `EDEN_NX2_FRAME_DUMP_AT` capture that has not fired yet. They share
  one `FrameDump`.
- One request at a time. A second client waits while a capture runs.
- `log_tail` sees lines logged after the game finished loading. The boot itself is in
  `eden_log.txt`.
- Runtime rejections last for one boot. Starting the next session clears them.
