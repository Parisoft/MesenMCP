# MesenMCP — Headless MCP Server Plan

**Goal:** Turn this Mesen fork into a headless **MCP (Model Context Protocol) server** that AI agents
(e.g. Arena Agents) can use to validate and debug NES games under development — with **no SDL, no X11,
no window, no .NET runtime**.

This is a plan only — no code has been changed.

---

## 1. What the code review found

### 1.1 Architecture today

| Directory | Role | Depends on SDL/X11? |
|---|---|---|
| `Core/` | Emulation cores (NES, SNES, GB, GBA, PCE, SMS, WS) + debugger + Lua | **No** |
| `Utilities/` | PNG encode (`PNGHelper`), sockets, archives, miniz/spng, … | **No** |
| `Lua/` | Vendored Lua sources (used by the script engine) | **No** |
| `SevenZip/` | 7z archive reader | **No** |
| `Sdl/` | `SdlRenderer`, `SdlSoundManager` — the **only** SDL code in the repo (4 files) | **Yes** |
| `Linux/` | Desktop input: `LinuxKeyManager`, `LinuxMouseManager` (X11 `XOpenDisplay`), `LinuxGameController` (evdev) | **Yes** (X11/evdev) |
| `InteropDLL/` | Flat C API (`MesenCore.so`) consumed by the .NET UI | Only via `Sdl/` includes |
| `UI/` | .NET 10 **Avalonia** GUI (creates the window whose XID is handed to SDL) | Indirectly |

### 1.2 Key facts that make this easy

1. **SDL is already isolated.** A grep for SDL includes across the whole tree matches only
   `Sdl/SdlRenderer.*` and `Sdl/SdlSoundManager.*`. The core never touches SDL. SDL reaches the build
   through:
   - the makefile: `SDL2INC := $(shell sdl2-config --cflags)` is added to **global** `CXXFLAGS`, and
     `$(SDL2LIB)` / `-lX11` / libevdev are linked into `MesenCore.so`;
   - `InteropDLL/EmuApiWrapper.cpp` / `HistoryApiWrapper.cpp`, which `#include "Sdl/…"` on Linux/macOS.
2. **The core is already null-safe headless.** `VideoRenderer` and `SoundMixer` check for a null
   `IRenderingDevice` / `IAudioDevice` before every call (`Core/Shared/Video/VideoRenderer.cpp`,
   `Core/Shared/Audio/SoundMixer.cpp`).
3. **Headless operation is already proven.** `InteropDLL/TestApiWrapper.cpp → RunCiTests()` /
   `RunRecordedTest(filename, inBackground=true)` create a standalone `Emulator`, set the
   `EmulationFlags::TestMode` flag, load a ROM and run it to completion **with no renderer, no audio
   device, no key manager** — Mesen's own CI runs its whole `.mntest` ROM test suite this way, on
   multiple threads.
4. **A software (non-SDL) renderer already exists**: `Core/Shared/Video/SoftwareRenderer` implements
   `IRenderingDevice` with no SDL. A render device self-registers in one line
   (`_emu->GetVideoRenderer()->RegisterRenderingDevice(this)`), which is the pattern our
   headless renderer will copy.
5. **The debugger is complete and C++-native** (`Core/Debugger/`): breakpoints with a full
   **expression evaluator** (NES dialect included), `Step(cpuType, count, StepType::{Step, StepOut,
   StepOver, CpuCycleStep, PpuStep, PpuScanline, PpuFrame, …})`, disassembler + search, memory
   dumper/get/set per `MemoryType`, trace logger, profiler, callstack, code-data logger, labels,
   frozen addresses, PPU tile/tilemap tools, and **input overrides**
   (`Debugger::SetInputOverrides(index, DebugControllerState)`).
6. **A rich Lua scripting API already exists** (`Core/Debugger/LuaApi.cpp`, `ScriptManager`,
   `ScriptHost`) — the same API the GUI script window uses. `ScriptManager::LoadScript(name, path,
   content, scriptId)` can load a script from a **string**, which maps perfectly onto an MCP tool.
7. **Useful utilities are already there**: `Utilities/PNGHelper::WritePNG()` (spng/miniz) for
   screenshots, `Utilities/Socket` for optional TCP transport, `AviRecorder`/`GifRecorder` for
   video capture, `WaveRecorder` for WAV capture, and vendored single-header libs
   (`magic_enum.hpp`) — precedent for vendoring `nlohmann/json`.
8. **`PlatformUtilities` is a no-op on Linux** (screensaver/timer tweaks are Windows-only), so
   `Emulator::Run()`'s platform calls are safe headless.
9. **Notifications** (`INotificationListener`, `ConsoleNotificationType`) already emit
   `GameLoaded`, `CodeBreak`, `EmulationStopped`, `PpuFrameDone`, `GameLoadFailed`, … — ready to be
   forwarded as MCP notifications.
10. **Input injection has three existing paths**: (a) an `IInputProvider` registered with the
    emulator (what `MesenMovie` uses — see `MesenMovie::SetInput()`), (b) the debugger's
    `SetInputOverrides` (8 ports), (c) raw key events via `IKeyManager` (needs a real keymap —
    avoid). We will use (a) as primary and (b) as override.

### 1.3 NES memory types available to tools (already implemented)

`NesMemory`, `NesPpuMemory`, `NesPrgRom`, `NesInternalRam`, `NesWorkRam`, `NesSaveRam`,
`NesNametableRam`, `NesMapperRam`, `NesSpriteRam`, `NesSecondarySpriteRam`, `NesPaletteRam`,
`NesChrRam`, `NesChrRom` (from `Core/Shared/MemoryType.h`).

---

## 2. Proposed architecture

```
                 ┌────────────────────────────────────────────────┐
 Agents  stdio   │  mesen-mcp  (new, C++ ~ small MCP front-end)   │
   ⇄     JSON-   │  ┌──────────────┐  ┌─────────────────────────┐ │
          RPC    │  │ MCP protocol │  │ Headless glue:          │ │
                │  │ dispatch     │  │  HeadlessRenderer       │ │
                │  │ + tool registry │  │  WavCaptureAudioDevice│ │
                │  └──────┬───────┘  │  VirtualInputProvider  │ │
                │         │          └─────────────────────────┘ │
                │  Core + Utilities + Lua + SevenZip (unchanged) │
                └────────────────────────────────────────────────┘
   Excluded from this target: Sdl/, Linux/, libevdev, -lX11, -lSDL2, InteropDLL, UI (Avalonia)
```

### 2.1 Deliverable shape

- New top-level `Mcp/` directory containing a single native executable **`mesen-mcp`**:
  an MCP server over **stdio** (newline-delimited JSON-RPC 2.0, MCP spec `2025-06-18`), one emulator
  instance per process (the normal MCP server model — one server per client session).
- The desktop GUI build (`make` → `MesenCore.so` + Avalonia UI) can keep working unchanged during
  development; pruning it is a separate decision (see §6).
- Implementation language: **C++17**, same toolchain/flags as the core. See §2.3 for the
  alternatives that were considered.

### 2.2 New components (all additive)

| Component | File(s) | Purpose |
|---|---|---|
| MCP server main + JSON-RPC loop | `Mcp/main.cpp`, `Mcp/McpServer.{h,cpp}` | stdio framing, `initialize`, `tools/list`, `tools/call`, `notifications`, logging |
| JSON | vendored `nlohmann/json.hpp` single header (same pattern as `Utilities/magic_enum.hpp`) | protocol + tool args/results |
| Tool registry | `Mcp/Tools.{h,cpp}` | tool schemas + handlers, each mapped to a core API (§4) |
| Headless renderer | `Mcp/HeadlessRenderer.{h,cpp}` | `IRenderingDevice` that stores the latest `RenderedFrame` under a lock; encodes PNG on demand |
| Audio capture sink (optional) | `Mcp/WavCaptureDevice.{h,cpp}` | `IAudioDevice` that keeps the last N seconds in RAM (ring buffer) so agents can *verify audio is playing*; or simply register no audio device (null-safe) in v1 |
| Virtual input | `Mcp/VirtualInputProvider.{h,cpp}` | `IInputProvider` (like `MesenMovie`) fed by MCP tool calls; per-port button state with frame-count TTL |
| Notification bridge | `Mcp/NotificationBridge.{h,cpp}` | `INotificationListener` → thread-safe queue → MCP notifications (`CodeBreak`, `EmulationStopped`, …) |
| Session/emu lifecycle | `Mcp/EmuSession.{h,cpp}` | owns `Emulator`, headless settings defaults, home-folder isolation, ROM list, safe shutdown |

### 2.3 Why C++ (and not C#/Python)

- **C# (reuse `UI/Interop/*.cs`)**: the interop wrappers are the largest existing API mapping, but
  they import Avalonia types (`WriteableBitmap` in `EmuApi.cs`, `DebugApi.cs`), so a headless server
  would drag Avalonia in or require refactoring; it also requires the .NET 10 runtime + a
  separately-shipped `MesenCore.so` (which today links SDL). Worse for sandboxes.
- **Python MCP SDK wrapping a CLI**: adds a second language and process layer; per-call process
  startup can't keep a live emulator session, so it would need a resident daemon anyway.
- **C++ native**: one self-contained binary, no runtime deps, direct access to `Debugger`,
  `MemoryDumper`, `ScriptManager`, locking primitives exactly as `InteropDLL/*Wrapper.cpp` already
  demonstrates the correct usage of. This is the recommended path.

### 2.4 MCP protocol details

- Transport: **stdio** (agents spawn `mesen-mcp`). Optional later: streamable HTTP / TCP using the
  existing `Utilities/Socket`.
- Capabilities: `tools` (+ `notifications` for breakpoint hits), `logging`.
- Tool results: text (JSON pretty-printed states, disassembly), plus MCP `image` content for
  screenshots/tilemap dumps (base64 PNG). Large memory dumps returned as hex/base64 text with
  pagination, or written to a file path and referenced.
- Every tool gets a **strict JSON schema** and a precise description — the descriptions are the
  agent's "documentation", so they must explain NES semantics (e.g. PPU vs CPU address spaces,
  `$2007` mirroring, nametable layout).

---

## 3. Work items

### 3.1 Build system (makefile)

1. **Stop putting SDL flags on everything.** Today `CXXFLAGS` includes `$(SDL2INC)` globally and the
   shared-lib link pulls `$(SDL2LIB) $(X11LIB) $(LIBEVDEVLIB)`. Split into:
   - `COREFLAGS` (no SDL) used for `Core/`, `Utilities/`, `Lua/`, `SevenZip/`, `Mcp/`;
   - `SDLFLAGS` applied only to `Sdl/` (and the `MesenCore.so` link) for the existing GUI target.
2. **New target `mcp`**:
   ```
   mcp: $(MCPOBJ) $(COREOBJ) $(UTILOBJ) $(LUAOBJ) $(SEVENZIPOBJ)
        $(CXX) ... -o mesen-mcp ... -pthread    # no SDL, no X11, no evdev
   ```
   with its own object dir (e.g. `obj.$(MESENPLATFORM).mcp/`) so objects compiled without
   `$(SDL2INC)` don't collide with the GUI build's objects. `make HEADLESS=1` / `make mcp` both
   reasonable; default `all` can stay GUI for now.
3. **CI**: add a job to `.github/workflows/build.yml` (or a new `mcp.yml`) on ubuntu that builds
   **without installing SDL2/X11 dev packages** (that's the regression test for the dependency
   removal), then runs the smoke test from §5.
4. Note: `InteropDLL/EmuApiWrapper.cpp` keeps its `#include "Sdl/..."` — it is simply not part of
   the `mcp` target. The MCP server replicates the ~30 lines of initialization it needs
   (`FolderUtilities::SetHomeFolder`, `Emulator::Initialize`, settings) directly instead of going
   through InteropDLL.

### 3.2 Headless runtime glue

1. **Lifecycle** (`EmuSession`): create `Emulator`, `Initialize(false)` (no shortcut handler),
   never register a key/mouse manager; register `HeadlessRenderer` (and optionally
   `WavCaptureDevice`); `LoadRom(VirtualFile(path), {})` — which already spawns the emulation
   thread (`Emulator.cpp` line ~550).
2. **Settings defaults for agents** (`EmuSettings`):
   - `EmulationFlags::TestMode` (as CI does: suppresses battery/prompt side effects),
     plus `MaximumSpeed` toggle for "fast-forward" tools; real-time 60 fps when speed=1.
   - Disable rewind/run-ahead/netplay (determinism), pick region explicitly (or auto),
     `VideoFilter` default ( screenshots use the decoder output as today's CI mode does).
   - Controller port config via `EmuSettings::SetInputConfig` (`InputConfig`): standard
     controllers on ports 1–2 by default, zapper/etc. configurable per request.
   - `FolderUtilities::SetHomeFolder(<workspace temp dir>)` so battery RAM, save states, Mesen
     settings and CDL files never touch (or collide in) the user's real `~/.config/Mesen2`.
3. **Frame stepping / "run N frames"**: with the debugger initialized
   (`Emulator::InitDebugger()`), `Debugger::Step(CpuType::Nes, N, StepType::PpuFrame)` while
   paused gives exact frame advance; unthrottled bulk running uses `MaximumSpeed` + poll
   `Emulator::GetFrameCount()` (the `RunTest()` pattern) with a timeout.
4. **Screenshots**: `HeadlessRenderer::UpdateFrame(RenderedFrame&)` keeps the newest frame
   (width/height/buffer/FrameNumber) under a lock; `screenshot` tool copies it and calls
   `PNGHelper::WritePNG()`. Return as MCP image content and/or save to a path. 256×240 native
   resolution by default.
5. **Input**: `VirtualInputProvider::SetInput(BaseControlDevice*)` writes the currently requested
   buttons for that port (mirror `MesenMovie::SetInput`). MCP tool state = per-port button mask +
   "hold for N frames" semantics implemented by counting `PpuFrameDone` notifications. Config-free
   fallback: `Debugger::SetInputOverrides` for force-held inputs while debugging.
6. **Audio** (v2): either no `IAudioDevice` (safe) or `WavCaptureDevice` ring buffer +
   `get_audio_summary` (RMS/peak per channel, last N frames) and `capture_wav` (existing
   `WaveRecorder`) so agents can assert "music is actually playing / not silent / not clipping".

### 3.3 MCP protocol layer

1. Newline-delimited JSON-RPC 2.0 on stdin/stdout (stderr → `logging/message` notifications +
   file log; **nothing else may write to stdout** — `MessageManager` must be routed to stderr
   (it already supports the `OutputToStdout` flag; keep it off) since stdout is the protocol
   channel).
2. Handlers: `initialize` (declare tools + notifications), `notifications/initialized`,
   `tools/list` (from the registry), `tools/call`, `ping`; unknown methods → standard error.
3. **Notification bridge**: `INotificationListener` enqueues `{CodeBreak → "emu/breakpoint",
   EmulationStopped → "emu/stopped", GameLoaded → "emu/loaded", …}`; the I/O thread drains the
   queue between requests (and after each `tools/call` completes).
4. **Cancellation/timeout discipline**: every tool that waits on the emulation thread
   (run_frames, wait_for_breakpoint…) takes `timeout_ms` (default ~2–10 s), polls with
   `std::condition_variable`, and returns a structured partial result on timeout — an agent must
   never deadlock the session.

### 3.4 Threading & locking model (critical correctness piece)

Follow the exact conventions `InteropDLL/*Wrapper.cpp` already established:

- Emulation runs on its own thread (`Emulator::Run`); all cross-thread state access goes through
  `Emulator::AcquireLock()` / `EmulatorLock` (RAII).
- Debugger access via `Emulator::GetDebugger(autoInit)` → `DebuggerRequest` (the
  `WrapDebuggerCall` pattern in `DebugApiWrapper.cpp`); debugger-lifetime edge cases (game
  unloaded mid-call) are already handled there — copy that code.
- Operations that require execution to be stopped (registers, callstack, memory while paused)
  use `DebugBreakHelper` semantics as the GUI does.
- One MCP `tools/call` at a time (serialize dispatch with a mutex); notifications are async.

### 3.5 Tool catalog (the agent-facing API)

Names are console-agnostic where free (core supports more than NES later), but v1 semantics are
NES-focused (`CpuType::Nes`, memory types from §1.3).

**Tier 0 — session & baseline (MVP)**
| Tool | Backing API |
|---|---|
| `load_rom(path, patch?, region?, settings?)` | `Emulator::LoadRom` + session defaults |
| `unload_rom` / `reset(power_cycle?)` | `Emulator::Stop` / `Reset` / `PowerCycle` |
| `pause` / `resume` / `set_speed(0..∞ \| "realtime")` | `Emulator::Pause/Resume`, `EmulationFlags::MaximumSpeed` |
| `get_status` | `GetRomInfo`, `GetFrameCount`, `GetFps`, region, `IsPaused`, checksums/SHA1 (`GetHash`) |
| `run_frames(n, timeout_ms)` | debugger `Step(PpuFrame)` or `MaximumSpeed` + frame-count poll (§3.2.3) |
| `screenshot(scale?)` | `HeadlessRenderer` + `PNGHelper::WritePNG` → image content |

**Tier 1 — inspection (debugging basics)**
| Tool | Backing API |
|---|---|
| `get_cpu_state` / `get_ppu_state` | `Debugger::GetCpuState/GetPpuState` (`NesCpuState`, `NesPpuState` structs → JSON: PC/A/X/Y/SP/P, cycle, sprite0, VBlank flags …) |
| `read_memory(type, start, length)` / `write_memory` / `get_memory_size(type)` | `Debugger::GetMemoryValues/SetMemoryValues/GetMemorySize` (also `Emulator::GetMemory`) |
| `search_memory(type, value_or_pattern, compare)` | fetch range + scan (small new helper) |
| `disassemble(address, count)` / `search_disassembly(text)` | `Disassembler::GetDisassemblyOutput`, `DisassemblySearch` |
| `evaluate_expression(expr)` | `ExpressionEvaluator` (NES dialect: reads `$addr`, symbols, `A`, `PC`, arithmetic) — also powers watch expressions |

**Tier 2 — breakpoints & stepping**
| Tool | Backing API |
|---|---|
| `set_breakpoint(address\|expr, type: r/w/x, condition?, cpu?)` / `list` / `remove` | `Debugger::SetBreakpoints` (full `Breakpoint` array semantics) |
| `step(type: instruction\|over\|out\|cycle\|scanline\|frame, count)` | `Debugger::Step(CpuType, count, StepType)` |
| `continue_` (resume until break) | `Debugger::Run()` + async `CodeBreak` notification with stop reason (which bp, cpu state snapshot) |
| `get_callstack` | `CallstackManager` |
| `trace(rows\|enable_to_file)` | `SetTraceOptions`, `GetExecutionTrace`, `StartLogTraceToFile` |
| `freeze_address(type, start, end, on)` | `FrozenAddressManager` via `UpdateFrozenAddresses` |

**Tier 3 — PPU/graphics inspection (very useful for NES homebrew)**
| Tool | Backing API |
|---|---|
| `get_tilemap(nametable, as_png?)` / `get_tiles(page)` | `PpuTools::GetTileView` / tilemap helpers (already exported in `DebugApiWrapper`) |
| `get_palette(background\|sprites)` | `NesPaletteRam` read + palette decode |
| `get_sprites()` (OAM list: pos/tile/attrs) | `NesSpriteRam` + OAM decode |

**Tier 4 — validation & automation (the "CI for your ROM" tier)**
| Tool | Backing API |
|---|---|
| `set_controller(port, buttons, hold_frames?)` / `press_sequence([{buttons, frames}])` | `VirtualInputProvider` (§3.2.5), sequence via frame notifications |
| `save_state(slot\|path)` / `load_state` | `SaveStateManager` / `Emulator::Serialize/Deserialize` (store under session temp dir) |
| `run_lua_script(code, timeout_ms)` | `ScriptManager::LoadScript(name, path, content, id)` + script output/log capture — unlocks Mesen's whole Lua API (`emu.read`, callbacks, savestates, drawing) for arbitrary test logic |
| `get_cdl_stats(memory_type)` (code/data coverage %, function list) | `CdlManager`/`GetCdlStatistics` — *test-coverage numbers for ROM code*, great for validation reports |
| `get_profile_data` / `get_memory_access_counts` | `Profiler`, `MemoryAccessCounter` (find never-read VRAM, hot routines) |
| `run_rom_test(.mntest path)` | `RecordedRomTest::Run` (existing format; also lets devs record tests in the GUI and replay them headlessly) |
| `capture_gif/avi(frames)` / `capture_wav(frames)` | existing `GifRecorder`/`AviRecorder`/`WaveRecorder` — bug-report attachments |
| `set_cheats` | `CheatManager` (Game Genie / PAR codes to bypass levels when testing later stages) |

Documentation for agents ships as each tool's `description` + `inputSchema`; plus a
`docs/MCP_TOOLS.md` cheat sheet (typical flows: "boot → screenshot → set bp on NMI → verify init
code ran", "controller sequence + RAM assertion", "coverage report after test run").

---

## 4. Repository / file-level change list (summary)

**Added**
- `Mcp/` — `main.cpp`, `McpServer`, `Tools`, `EmuSession`, `HeadlessRenderer`,
  `WavCaptureDevice` (opt), `VirtualInputProvider`, `NotificationBridge`
- `Utilities/nlohmann/json.hpp` (vendored single header)
- `docs/MCP_TOOLS.md`, `.github/workflows/mcp.yml` (build + smoke test)
- `MCP_PLAN.md` (this document)

**Modified**
- `makefile` — split SDL flags out of global `CXXFLAGS`; add `mcp` target + object dir; skip
  `Sdl/`, `Linux/`, `libevdev`, `InteropDLL` for it
- `.github/workflows/build.yml` — (only if desired) note that GUI job stays as-is

**Unchanged** — `Core/`, `Utilities/` (except vendored header), `Lua/`, `SevenZip/`, the desktop
GUI. Any core edits should be limited to: (a) making `sdl2-config` absence non-fatal, and
(b) at most small accessors if a needed getter is private (none identified so far — the debugger
surface is already sufficient).

---

## 5. Testing strategy

1. **Dependency gate**: CI job builds `mesen-mcp` in a container **without** SDL2/X11 dev packages —
   build failure = SDL leaked back in.
2. **Protocol conformance smoke test** (python, CI): spawn `mesen-mcp`, do `initialize` →
   `tools/list` → `load_rom(test rom)` → `run_frames(60)` → `screenshot` → assert non-empty PNG and
   expected memory (e.g. `read_memory(NesWorkRam, 0, 16)`).
3. **Emulation golden tests**: run existing `.mntest` suites via `run_rom_test` (Mesen's own test
   corpus) — headless runner already exists, so this is plumbing only.
4. **Debugger tests**: `set_breakpoint($0800` exec`)` + `step` + PC assertion; watch expression
   evaluation; NMI break on a test ROM with known frame cadence.
5. **Determinism test**: same ROM + savestate + input sequence twice → identical screenshots
   (hash) and RAM dumps.

---

## 6. Open decisions (need your call before implementation)

> **Decision log (2026-08-26):**
> 1. **GUI: DROP.** The server is agent-only. `UI/`, `Sdl/`, `Linux/`, `Windows/`, `MacOS/`,
>    `InteropDLL/`, `PGOHelper/` get removed; build = `Core/` + `Utilities/` + `Lua/` +
>    `SevenZip/` → single native binary, no .NET/SDL/X11/evdev deps. Mitigation for the lost
>    `.mntest` recording UI: expose `RecordedRomTest::Record()` as an MCP tool (core-side,
>    headless-capable) so agents can author ROM tests themselves.
> 2. **Consoles: NES + SNES + GBA for v1.** All three have full debugger/expression-evaluator
>    support. Prerequisite: **GBA requires `gba_bios.bin`** (16 KB, cannot be shipped —
>    copyrighted). `MissingFirmware` must surface as a structured tool error naming the expected
>    file/size. (NES: no firmware except FDS `disksys.rom`; SNES: none for typical homebrew.)
>    Tools stay console-parameterized (`cpu`, `memory_type`); per-console enum docs.
> 3. **Transport: recommendation = stdio for v1** (matches one-ROM-per-process, zero network
>    surface, works in any sandbox), with the protocol loop isolated behind a
>    `read_message()/write_message()` interface so an authenticated `--http` (Streamable HTTP)
>    mode can be added later for a shared lab server. Awaiting final confirmation.
> 4. **One ROM per process: confirmed.** `load_rom` replaces the current ROM; no session IDs in
>    tool schemas.
> 5. **Licensing: GPLv3, fine for internal use.** New files carry GPLv3 headers; nlohmann/json is
>    MIT (GPL-compatible, matches vendored-header precedent). Revisit if ever embedded in a
>    proprietary host.

1. **Keep or drop the desktop GUI?**
   - *Keep (recommended initially)*: MCP is an additive target; upstream merges stay easy.
   - *Drop*: delete `Sdl/`, `Linux/`, `InteropDLL/` SDL includes, `UI/` — repo becomes headless-only;
     simpler, but loses the GUI debugger and recording tools devs use to create `.mntest`/movies.
2. **Consoles in scope for v1**: NES-only tool semantics first (as requested), generic core already
   supports SNES/GB/PCE/SMS/GBA/WS — tools are designed console-parameterized anyway.
3. **Transport**: stdio only for v1? (Streamable HTTP via `Utilities/Socket` is a later add.)
4. **Session model**: one ROM per process is the MCP norm — confirmed OK? (Multi-ROM could be added
   later via per-ROM session ids, the `RunRecordedTest(inBackground)` pattern supports it.)
5. **Licensing**: repo is GPLv3; the MCP server links the core so it stays GPLv3 — fine for internal
   agent tooling, worth noting if this ever ships inside a proprietary host.

---

## 7. Risks & mitigations

| Risk | Mitigation |
|---|---|
| Emulation thread vs MCP thread races | Reuse `InteropDLL`'s proven lock patterns verbatim (§3.4); serialize tool calls |
| Long-running `run_frames` blocks the protocol loop | Timeouts + polling + partial results; document that agents should poll `get_status` |
| stdout pollution (breaks JSON-RPC) | Audit all logging paths (`MessageManager`, `RunCiTests`' `std::cout`) → stderr; assert in tests |
| Mapper/homebrew edge cases (unsupported mapper, training ROM) | `GameLoadFailed` notification → structured tool error with the mapper number; homebrew iNES headers usually fine |
| FDS/arcade firmware prompts headless | `MissingFirmware` notification mapped to a clear tool error (NES FDS only) |
| Savestate/config written to user's real Mesen dir | `SetHomeFolder(session tmp)` enforced at startup (§3.2.2) |
| Big memory dumps blowing up context windows | Pagination + hex format + file-path option; screenshots 256×240 PNG (~small) |
| `nlohmann/json` vendoring concerns | Single header, well-known, matches existing vendored-header precedent; swap-able |

---

## 8. Suggested milestones

| Phase | Contents | Rough size |
|---|---|---|
| **P0 — headless proof ✅ (done)** | makefile split + `mcp` target; `main.cpp` that loads a ROM headless, runs 300 frames, dumps a PNG, exits. No MCP yet. Validates "no SDL/X11" claim. | done |
| **P1 — MCP skeleton ✅ (done)** | JSON-RPC loop, `initialize`/`tools/list`/`tools/call`; Tier 0 tools (load/pause/resume/run_frames/screenshot/get_status + unload/reset/set_speed); CI job + smoke test | done |
| **P2 — inspection ✅ (done)** | Tier 1–2 tools (memory, disasm, cpu/ppu state, breakpoints, step, trace, expressions, break notifications) | done |
| **P3 — input & validation ✅ (done)** | Tier 3–4 (controllers, savestates, Lua, CDL coverage, ROM tests, captures) | done |
| **P4 — hardening** | determinism tests, docs, timeouts everywhere, optionally prune GUI / add HTTP transport | ~2–3 days |

### P0 findings (implementation notes for P1+)

P0 shipped with the GUI removal (972 files deleted: `UI/`, `Sdl/`, `Linux/`, `Windows/`,
`MacOS/`, `InteropDLL/`, `PGOHelper/`, solution/vcxproj files, GUI CI workflows), a new
SDL-free makefile, `Mcp/HeadlessRenderer` + `Mcp/main.cpp` (CLI front-end), a test ROM
generator and a CI workflow. Validated with public NES test ROMs (blargg full_palette,
ppu_vbl_nmi) with no display server; `ldd` shows no SDL/X11/Wayland.

Two headless-only gotchas discovered and fixed — **P1 must carry both into the MCP session layer**:

1. **The core expects the front-end to supply the NES palette.** `NesConfig::UserPalette`
   is zero-initialized and nothing in `Core/` ever populates it (the old .NET GUI pushed
   it through its config on startup). With a zero palette, `NesDefaultVideoFilter` decodes
   every pixel as black even though the PPU is rendering correctly. Fix: seed it via
   `NesDefaultVideoFilter::GetBuiltInPalette()` (added in P0) before `LoadRom`, with
   `IsFullColorPalette = true`.
2. **Deterministic settings are mandatory.** Defaults are deliberately non-deterministic:
   `RamPowerOnState` is *random* (a Mesen feature to catch homebrew bugs — real test ROMs
   crashed into random RAM), and `RunAheadFrames` defaults to 1 (hidden second emulation
   instance; drops/duplicates frames and confuses frame-accurate tooling). P1 mirrors
   `RecordedRomTest::UpdateSettings()` (zeroed RAM, run-ahead off, no frame skipping,
   sprite limit intact), and sets `EmulationFlags::MaximumSpeed` **after** `LoadRom`
   (settings are re-applied during load, which resets flags set earlier).
   These become `mesen-mcp` session options (`--deterministic` on by default; agents
   debugging RAM-initialization bugs can opt into random RAM).
3. Also observed (unresolved, queued for P1): `Emulator::StopDebugger()` called while the
   emulation thread runs at `MaximumSpeed` can deadlock — the P1 debugger lifecycle must
   stop/pause emulation before releasing the debugger (the GUI always kept the debugger
   alive across ROM changes instead of re-initializing it mid-run).

### P1 implementation notes

- `Mcp/McpServer` — stdio JSON-RPC 2.0 loop (newline-delimited, MCP `2025-06-18` with
  version negotiation for `2024-11-05`/`2025-03-26`), `initialize`/`ping`/`tools/list`/
  `tools/call`, `-32700`/`-32600`/`-32601`/`-32602`/`-32002` error codes, rejects requests
  before `initialize`, exits cleanly on stdin EOF. Tool failures are MCP `isError` content,
  protocol failures are JSON-RPC errors.
- `Mcp/EmuSession` — owns the `Emulator` + `HeadlessRenderer` for the process; implements
  the P0 lessons (palette seeding, deterministic settings, speed flag after `LoadRom`).
  Determinism is a `load_rom` argument (`deterministic`, default true; agents can opt into
  random power-on RAM). Region/patch/speed are `load_rom` arguments.
- `Mcp/ToolRegistry` — tool schemas + agent-facing descriptions (the descriptions are the
  primary docs LLM clients see). 9 Tier-0 tools shipped.
- **stdout discipline**: the core's stray stdout writes were rerouted to
  `MessageManager` (`Debugger::Log`, `PNGHelper` error path); emu2413's debug printf is
  behind `OPLL_DEBUG=0` (compiled out). Core logging to stdout is enabled only in the P0
  CLI mode, never in MCP mode.
- `Mcp/tests/mcp_smoke_test.py` — 39-check end-to-end MCP client (handshake, tool flows,
  PNG pixel verification, error paths, graceful shutdown); wired into `make test` + CI.
- Concurrency: single-threaded dispatch (one `tools/call` at a time); `run_frames` polls
  with a timeout and returns partial results — an agent session can never deadlock on it.
- Not carried into P1 (still open for P2): the `StopDebugger` deadlock above (no debugger
  use in P1), notifications (breakpoint hits), memory/CPU inspection tools.

### P2 implementation notes

- **Debugger lifecycle**: the debugger is initialized lazily by the first debug tool call
  via `Emulator::InitDebugger()` (which locks the emulation like the GUI does) and then
  **kept alive for the session** — `StopDebugger()` is never called (the P0 deadlock), and
  the debugger survives ROM reloads on its own (`Emulator::InternalLoadRom` preserves it).
- `Mcp/NotificationBridge` — `INotificationListener` registered on the emulator's
  `NotificationManager`; copies `CodeBreak` events (`BreakEvent`) under a mutex with a
  condition variable. `wait_for_breakpoint` blocks on it — polling beats push notifications
  for most MCP clients.
- **16 new tools** (25 total): `get_cpu_state`, `get_ppu_state` (NES/SNES/GBA serialization
  incl. sub-CPUs), `get_memory_size`, `read_memory`, `write_memory`, `search_memory`
  (value + hex-pattern, little-endian), `disassemble`, `evaluate_expression`,
  `set_breakpoint`/`remove_breakpoint` (session keeps the list, pushes it in full via
  `Debugger::SetBreakpoints` — its replace-all semantics), `step` (instruction/over/out/
  cycle/scanline/frame), `continue`, `wait_for_breakpoint`, `get_callstack`, `trace`
  (Mesen trace-format tags; default `[PC] [Disassembly]`), `get_debugger_status`.
- Core additions: `Breakpoint::Init(...)` (fields were private; the old GUI built the
  struct layout in C#) and the P1-era `NesDefaultVideoFilter::GetBuiltInPalette()`.
- Memory types: resolved by friendly names (`cpu`, `work_ram`, `prg_rom`, `oam`,
  `palette_ram`, ...) mapped through `magic_enum` over `MemoryType`; unavailable types are
  excluded and the error message lists what exists for the loaded console. Note: NES
  `work_ram` is Mesen's 8KB WRAM region; the 2KB internal RAM is `internal_ram`.
- Step semantics: from a stopped state, `step` waits for a **new** break event
  (`NotificationBridge` sequence), not `IsExecutionStopped()` (already true); `step out`
  from a non-subroutine context runs to the next popped stack address (upstream behavior).
- Expressions: `$xx` hex literals, `[addr]` memory reads, register names — documented in
  the tool description (`[$2002] & $80` etc.).
- **Debugger-instrumentation gotcha** (documented for future test-ROM authors): a tight
  `BIT $2002 / BPL` vblank poll can phase-lock into Mesen's vblank-flag read-suppression
  race under debugger instrumentation and never observe the flag. The bundled test ROM (v2)
  uses a cycle-counted delay loop instead of `$2002` polls, keeps a zero-page signature for
  memory tools, and calls a subroutine at `$C100` from its main loop as a
  reliably-hitting breakpoint target.
- `mcp_smoke_test.py` grew to 58 checks (P1 flow + full P2 debugger flow incl. breakpoint
  hit at `$C100`, step-out to `$C05A`, memory roundtrip, pattern search, trace rows).
- Validated against blargg's `full_palette.nes` (correct register/disasm/trace of its real
  code paths).


---

## Appendix A — precise code references backing this plan

- SDL boundary: `Sdl/SdlRenderer.{h,cpp}`, `Sdl/SdlSoundManager.{h,cpp}`; includes in
  `InteropDLL/EmuApiWrapper.cpp` (~lines 28–33) and `InteropDLL/HistoryApiWrapper.cpp`;
  makefile `SDL2INC/SDL2LIB/X11LIB` and the `MesenCore.so` link recipe.
- Headless precedent: `InteropDLL/TestApiWrapper.cpp` — `RunRecordedTest(inBackground=true)`
  (fresh `Emulator`, `Initialize(false)`, `TestMode`), `RunTest` (frame-count polling),
  `RunCiTests` (multi-threaded headless ROM tests).
- Null-safe sinks: `Core/Shared/Video/VideoRenderer.cpp` (`if(_renderer)` everywhere),
  `Core/Shared/Audio/SoundMixer.cpp` (`if(_audioDevice)`).
- Device registration pattern: `Sdl/SdlRenderer.cpp:19`,
  `Core/Shared/Video/SoftwareRenderer.cpp:12` — `_emu->GetVideoRenderer()->RegisterRenderingDevice(this)`.
- Emulator API: `Core/Shared/Emulator.h` (LoadRom/Pause/Resume/GetFrameCount/InitDebugger/
  GetDebugger/Serialize/…); emulation thread spawn in `Core/Shared/Emulator.cpp` (~line 550).
- Debugger: `Core/Debugger/Debugger.h` (`Step`, `SetBreakpoints`, `SetInputOverrides`,
  `GetCpuState`/`GetPpuState` via tools, `GetMemoryDumper`, `GetTraceLogger`, `PpuTools`);
  step types in `Core/Debugger/DebugTypes.h` (`StepType`); locking wrappers in
  `InteropDLL/DebugApiWrapper.cpp` (`WrapDebuggerCall`/`WithDebugger`).
- Input provider pattern: `Core/Shared/Interfaces/IInputProvider.h`,
  `Core/Shared/Movies/MesenMovie.cpp::SetInput`, `Core/Shared/BaseControlManager.cpp`.
- Scripting: `Core/Debugger/ScriptManager.h::LoadScript(name, path, content, id)`,
  `Core/Debugger/LuaApi.cpp`.
- Notifications: `Core/Shared/Interfaces/INotificationListener.h` (`CodeBreak`, `GameLoaded`, …).
- Screenshot: `Utilities/PNGHelper.h::WritePNG`; frame struct: `Core/Shared/RenderedFrame.h`.
- Memory types: `Core/Shared/MemoryType.h` (§1.3 list).
- Linux-only desktop deps to exclude: `Linux/LinuxMouseManager.cpp` (`XOpenDisplay`),
  `Linux/LinuxKeyManager.*`, `Linux/LinuxGameController.*`, `Linux/libevdev/`.

### P3 implementation notes

- `Mcp/VirtualInputProvider` — `IInputProvider` (the movie mechanism) injecting
  per-port button masks; `set_controller` accepts button names or a list string,
  with `hold_frames` semantics (held for exactly N frames, the tool advances them)
  or hold-until-changed. **Controllers must be plugged in explicitly**: headless
  sessions start with NO controllers (`ControllerConfig.Type` defaults to `None`;
  the GUI filled this from its config) — `load_rom` now plugs standard pads into
  ports 1-2 per console.
- **Lua `print()` was writing to stdout** — the single source of every mysterious
  "crash"/flaky stream corruption during P3 testing. `ScriptingContext` now
  redirects Lua's `print` to the per-script log (via `lua_getextraspace`), so
  script output lands in `run_lua_script`'s log where agents read it. Lua API
  notes: memory access needs an explicit type — `emu.read(addr, emu.memType.nesMemory)`.
- Memory semantics reminder baked into the tools: NES `internal_ram` = the 2KB
  console RAM (mirrored across $0000-$1FFF on the CPU bus); `work_ram` = the
  separate 8KB cartridge WRAM ($6000-$7FFF). Scripts writing CPU-bus addresses
  are read back via `internal_ram`/`cpu`.
- `run_rom_test` replays `.mntest` files in a **separate background emulator**
  (Mesen's CI pattern) — the current session survives untouched.
  `record_rom_test` + `stop_rom_test_record` record the current session (inputs +
  per-frame hashes); replay of a deterministic session passes. This is the
  regression-suite workflow: record known-good behavior, replay after changes.
- Savestates via `SaveStateManager` file API; cheats via `CheatManager::SetCheats`
  (code family guessed from format/console, address decoding is the core's job);
  GIF capture via `VideoRenderer::StartRecording(GIF)` + run frames + stop.
- CDL coverage accumulates only while the debugger exists — call any debug tool
  before `get_cdl_stats` matters (the tool lazily initializes it, then coverage
  grows from that point).
- Stepping: a small settle delay + retry loop makes `Debugger::Step` reliable at
  max speed; consecutive rapid steps within a tight loop still jitter by an
  instruction (inherent to the break/release race) — single steps are exact.
- 38 tools total; smoke test at 76 checks, stable across repeated runs.
