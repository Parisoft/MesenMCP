# MesenMCP

Headless [MCP](https://modelcontextprotocol.io/) server around the Mesen emulation core,
built so AI agents can validate and debug NES (and later SNES/GBA) homebrew without any
display server, audio device or .NET runtime.

```
mesen-mcp --rom game.nes --frames 300 --screenshot out.png
```

The build is a single native binary (`bin/mesen-mcp`) linked from the untouched
`Core/`, `Utilities/`, `Lua/` and `SevenZip/` trees plus the headless front-end in
`Mcp/`. It has **no SDL, X11, Wayland or evdev dependency** - video frames are captured
in memory by a headless rendering device and can be written as PNG screenshots; audio
and desktop input are simply not wired up.

## Status

- **P0 (done)** - headless proof of concept: load a ROM, run N frames at maximum
  speed, dump a PNG screenshot, exit - validated against public NES test ROMs
  with no `$DISPLAY` present.
- **P1 (done)** - MCP server over stdio (newline-delimited JSON-RPC 2.0, MCP
  `2025-06-18`) with Tier-0 tools: `load_rom` (regions, patches, deterministic
  mode), `unload_rom`, `reset`, `pause`/`resume`, `set_speed` (`max`/`realtime`),
  `run_frames` (frame-accurate advancement with timeout), `screenshot` (native
  resolution PNG as MCP image content) and `get_status`.
- **P2 (done)** - inspection & debugger tools: `get_cpu_state`/`get_ppu_state`
  (NES/SNES/GBA), `read_memory`/`write_memory`/`search_memory`/`get_memory_size`
  (friendly memory-type names, console-aware), `disassemble`,
  `evaluate_expression` (Mesen expressions, `[addr]` memory reads),
  `set_breakpoint`/`remove_breakpoint`/`wait_for_breakpoint` (execute/read/write,
  conditions, ranges), `step` (instruction/over/out/cycle/scanline/frame),
  `continue`, `get_callstack`, `trace` and `get_debugger_status` — 25 tools total,
  58-check smoke test.
- **P3 (next)** - input & validation: controller injection, savestates, Lua
  scripting, CDL coverage, `.mntest` replay, GIF/AVI/WAV capture.
  See [MCP_PLAN.md](MCP_PLAN.md) for the full roadmap.

### Using with an MCP client

```
mesen-mcp            # stdio transport; one ROM per process
```

Client config example (Claude Desktop / any stdio MCP host):

```json
{
  "mcpServers": {
    "mesen": { "command": "/path/to/mesen-mcp" }
  }
}
```

Optional arguments: `--home <dir>` (emulator state + firmware files such as
`gba_bios.bin`; default: per-process temp dir), `--verbose`.

## Building

Requires a C++17 compiler (g++ or clang++) and python3 for the test ROM generator:

```
make          # build bin/mesen-mcp
make test     # build + generate a test ROM and run the headless smoke test
make clean    # remove build outputs
```

`DEBUG=1 make` builds unoptimized with debug info; `SANITIZER=address make` adds ASan.

## Layout

| Path | Purpose |
|---|---|
| `Mcp/` | Headless front-end (renderer, CLI entry point; MCP server in P1) |
| `Core/` | Mesen emulation core (NES/SNES/GB/GBA/PCE/SMS/WS) - unchanged |
| `Utilities/`, `Lua/`, `SevenZip/` | Core support libraries - unchanged |

## License

GPLv3, as Mesen. See [LICENSE](LICENSE).
