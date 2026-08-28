# MesenMCP

A headless **MCP (Model Context Protocol) server** around the Mesen emulation core —
built so AI agents can validate and debug NES, SNES and GBA games **without any
display server, audio device or .NET runtime**. One native binary, no SDL/X11/evdev
dependencies, driven over stdio.

```
mesen-mcp --rom game.nes --frames 300 --screenshot out.png   # quick CLI check
mesen-mcp                                                    # MCP server on stdio
```

49 tools covering the full debug/validate loop: load & drive the game, inspect and
patch memory, break/step/trace the CPU, look at the PPU (tilemaps, sprites, palette),
verify audio, script arbitrary assertions in Lua, and record/replay regression tests.

---

## Building

Requires a C++17 compiler (g++ or clang++) and python3 for the test tooling.
**No other dependencies** — no SDL2, no X11, no .NET SDK.

```
make                # -> bin/mesen-mcp
make test           # build + generate a test ROM + run the 97-check MCP smoke test
make clean
```

Options:

| Variable | Effect |
|---|---|
| `DEBUG=1` | unoptimized build with debug symbols |
| `SANITIZER=address` / `SANITIZER=thread` | sanitizer build (compile **and** link) |
| `CXX=clang++` / `CC=clang` | use Clang (usually faster code) |

Note: the makefile has no header dependency tracking — after editing a header,
`rm obj/<area>/*.o` (or `make clean`) to be safe.

The binary runs with no `$DISPLAY` — that is the intended environment.

### Cross-console e2e (optional)

`python3 Mcp/tests/e2e_test.py` runs the **entire 49-tool surface** against real
ROMs per console — NES, SNES and GBA zips plus a `gba-bios.bin` placed in
`Mcp/tests/` (160 checks). The test auto-creates the session home and installs the
BIOS where the core expects it (`<home>/Firmware/gba_bios.bin`).

## Using with an MCP client

```
mesen-mcp [--home <dir>] [--verbose]
```

Client config (Claude Desktop / any stdio MCP host):

```json
{
  "mcpServers": {
    "mesen": { "command": "/path/to/mesen-mcp" }
  }
}
```

- `--home <dir>` — session state dir (settings, battery RAM, savestates, captures,
  logs). Default: per-process temp dir. **Firmware files** (e.g. GBA's required
  16 KB `gba_bios.bin`) go in `<home>/Firmware/`.
- One ROM per process (the MCP model). `load_rom` replaces the current ROM.
- Protocol: stdio, newline-delimited JSON-RPC 2.0, MCP `2025-06-18`
  (negotiates `2024-11-05`/`2025-03-26`).

---

## Tool reference (49 tools)

### Session & lifecycle

| Tool | Description |
|---|---|
| `load_rom` | Load a ROM (plain file **or zip/7z archive** — first ROM entry is used) and start running. Options: `patch_path`, `region` (auto/ntsc/pal/dendy), `deterministic` (default true), `speed` (max/realtime) |
| `unload_rom` | Stop and unload (battery not saved — throwaway session by design) |
| `reset` | Soft reset or `mode:"power_cycle"` |
| `pause` / `resume` | Pause/resume (pause waits for the stop to land) |
| `set_speed` | `max` (unthrottled, default) or `realtime` |
| `run_frames` | Advance N frames (60 = 1s of game time), timeout + partial results |
| `screenshot` | Current frame as PNG (MCP image content, optional `save_path`) |
| `get_status` | rom_loaded, paused, frame count, fps, region, sha1/crc32, video size |

### CPU / memory inspection

| Tool | Description |
|---|---|
| `get_registers` | **One-shot view:** registers + status flags decoded by name (NES `n/v/d/i/z/c`, SNES `n/v/m/x/d/i/z/c`, GB `z/n/h/c`, GBA N/Z/C/V) + CPU cycle count (GBA: master clock instead) + PPU scanline/dot/frame + master clock |
| `get_cpu_state` | Registers per console: 6502 (NES), 65816 incl. K/DBR/D (SNES), ARM7TDMI incl. CPSR (GBA); sub-CPUs via `cpu` (e.g. `spc`) |
| `get_ppu_state` | Scanline/cycle, frame count, rendering flags, scroll, vblank (NES), BG mode (SNES/GBA) |
| `get_memory_size` | Size of a region; invalid names list what exists |
| `read_memory` | Bytes as hex (+ array for short reads); addresses accept `49152`, `"0xC000"` or `"$C000"` |
| `write_memory` | From an int array or hex string (`"DE AD"`); debugger write (no register side effects) |
| `search_memory` | Find a value (little-endian, 1/2/4 bytes) or hex pattern; returns addresses |
| `disassemble` | Instructions at an address (default: current PC) |
| `evaluate_expression` | Mesen expression language: `[$2002] & $80`, `A + X`, `PC == $C000` |

**Memory types** (console-prefixed names are stripped; `get_memory_size` lists them
all for the loaded console): NES `cpu` (full 64K bus), `internal_ram` (2K, mirrored
at $0000-$1FFF), `work_ram` (cart WRAM $6000+, when present), `prg_rom`, `chr_rom`/
`chr_ram`, `nametable_ram`, `sprite_ram` (OAM), `palette_ram`; SNES adds
`snes_prg_rom`, `work_ram`, `video_ram`, `cg_ram`, `spc_ram`, …; GBA has
`int_work_ram`, `ext_work_ram`, `video_ram`, `palette_ram`, …

### Debugging

| Tool | Description |
|---|---|
| `set_breakpoint` | By **address**, by **label** (from a `.dbg` file - also accepted directly in `address`), or by **source line** (`file`+`line`); execute/read/write (combinable), ranges, optional expression `condition` |
| `remove_breakpoint` | By `id`, or `all:true` |
| `wait_for_breakpoint` | Block until a break (timeout_ms); returns which bp hit + CPU state |
| `step` | `instruction` / `over` / `out` / `cycle` / `scanline` / `frame` |
| `continue` | Resume after a break |
| `get_callstack` | Current call chain |
| `trace` | In-memory instruction trace (format/condition filters, optional `run_frames`) |
| `trace_to_file` | Unlimited-length trace logging to a file (start/stop/auto_stop) |
| `get_debugger_status` | Debugger active/stopped, breakpoint list, available cpus & memory types |
| `load_dbg_file` | Load a ca65/cc65 `.dbg` file: imports labels (usable in expressions/conditions/disassembly) + source-line mappings |
| `find_labels` | Find symbols by name (substring or exact): CPU address, PRG offset, file:line, size, RAM/code |
| `list_source_files` | Source files in the loaded `.dbg` with their code line counts |

The debugger starts lazily on the first debug tool call and stays alive for the
session (it survives ROM reloads).

### Input, time travel & scripting

| Tool | Description |
|---|---|
| `set_controller` | Virtual pads: buttons by name (`up/down/left/right/start/select/b/a` + `x/y/l/r` on SNES, `l/r` on GBA); `hold_frames` for exact-length presses |
| `release_controller` | Release a port |
| `save_state` / `load_state` | Exact CPU/RAM/PPU snapshots (files under `<home>/savestates`) |
| `run_lua_script` | Mesen's Lua API: `emu.read(addr, emu.memType.nesMemory)`, `emu.write`, `emu.getMemorySize`, `emu.addEventCallback`, `emu.createSavestate`, HUD drawing…; output captured; resident scripts via `auto_stop:false` |
| `get_lua_script_log` / `stop_lua_script` | Manage resident scripts |

### Validation & media

| Tool | Description |
|---|---|
| `get_cdl_stats` | Code/data logger coverage: how much of PRG was executed (test-coverage proxy) + function count |
| `record_rom_test` / `stop_rom_test_record` | Record the session (inputs + per-frame hashes) as a `.mntest` file |
| `run_rom_test` | Replay a `.mntest` in an **isolated background emulator** — regression-testing a build without touching your session |
| `set_cheats` | Game Genie / PAR codes (family auto-detected); `[]` clears |
| `get_palette` | Palette RAM: raw + decoded RGB (bg + sprites) |
| `get_tilemap` | Background tilemap as PNG + metadata (scroll, mirroring) — includes off-screen areas |
| `get_tiles` | Tileset/CHR as PNG (palette-selectable or grayscale) |
| `get_sprites` | Decoded OAM table (pos/size/tile/palette/visibility) + screen preview PNG |
| `get_audio_summary` | Per-channel RMS/peak + clipping samples (window + totals) — "is the music playing / too loud?" |
| `capture_wav` | The actual mixed audio output as WAV |
| `capture_gif` | Gameplay clip as GIF |

---

## Source-level debugging (ca65 / cc65)

Build your ROM with debug info and MesenMCP can map source code to addresses:

```
ca65 -g -t nes main.s
ld65 -C nes.cfg -o game.nes --dbgfile game.dbg main.o ...nes.lib
# (with the cl65 driver: cl65 -t nes -g -Wl,--dbgfile,game.dbg ...)
```

Then in a session: `load_rom` → `load_dbg_file` → break on code, not hex:

- `set_breakpoint {"label": "update_hud"}` — by symbol (spans the label's size)
- `set_breakpoint {"file": "main.s", "line": 39}` — by source line (all code the line generated; `end_line` extends)
- `find_labels {"query": "oam"}` — where is a symbol? RAM labels included (`shadow_oam @ $0200`)
- Labels work in expressions/conditions (`evaluate_expression "update_hud"`, breakpoint `condition: "counter == 5"`)
- `disassemble` rows are annotated with `source_file`/`source_line`

A fixture ROM + .dbg built exactly this way lives in `Mcp/tests/dbg-rom.*` and is
covered by the smoke test.

## Determinism

By default sessions are reproducible: zeroed power-on RAM, no run-ahead, no frame
skipping (mirroring Mesen's own CI runner). `load_rom` accepts
`deterministic:false` to test against randomized power-on RAM. Emulation-internal
timing is exact regardless of host speed (`max` mode just removes the throttling).

## Testing

| Command | What it does |
|---|---|
| `make test` | Generates `Mcp/tests/red.nes` (hand-assembled mapper-0 ROM with a signature in zero page and a pulse tone) + runs the 97-check MCP smoke test |
| `python3 Mcp/tests/e2e_test.py` | 160-check cross-console e2e over all 49 tools (needs the ROM zips + BIOS in `Mcp/tests/`) |
| `bin/mesen-mcp --rom ... --frames N --screenshot out.png` | P0 one-shot CLI: load, run, screenshot, exit |

## Known issues & limitations

- **GBA reset deadlock** (core): `reset`/`power_cycle` while a GBA game idles in a
  BIOS halt state *with the debugger active* can wedge the emulation thread. The
  `reset` tool detects the stall (watchdog) and reports a clear error — restart the
  session. Doing lifecycle operations before the first debug tool call always works.
- **GBA CDL** (`get_cdl_stats`) stays empty headless; NES/SNES work.
- Debugger-thread writes to NES APU registers don't produce audible output — drive
  audio through game code (real ROMs like the bundled test ROM work fine).
- GBA requires a user-supplied `gba_bios.bin` (copyrighted, cannot ship) in
  `<home>/Firmware/`.

## Repository layout

| Path | Purpose |
|---|---|
| `Mcp/` | Headless front-end: `McpServer` (stdio JSON-RPC), `EmuSession` (+ Debug/Input/Media tool impls), `ToolRegistry`, `HeadlessRenderer`, `VirtualInputProvider`, `WavCaptureDevice`, `NotificationBridge` |
| `Core/`, `Utilities/`, `Lua/`, `SevenZip/` | Mesen core — nearly unchanged (see `MCP_PLAN.md` for the exact core diff surface) |
| `Mcp/tests/` | Test ROM generator, MCP smoke test, cross-console e2e |
| `MCP_PLAN.md` | Full project plan, phase notes, known issues, roadmap |

## History & roadmap

P0 headless proof → P1 MCP server (Tier-0) → P2 debugger/inspection → P3
input/validation → P4 PPU+audio+trace tools → cross-console e2e (160 checks
against real games). Remaining roadmap items (determinism/stress suites, docs
generation, HTTP transport, labels) are tracked in [MCP_PLAN.md](MCP_PLAN.md).

## License

GPLv3, as Mesen. See [LICENSE](LICENSE).
