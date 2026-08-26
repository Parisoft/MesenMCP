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

- **P0 (done)** - headless proof of concept CLI: load a ROM, run N frames at maximum
  speed, dump a PNG screenshot, exit. Verified against public NES test ROMs
  (blargg's full palette / vbl-nmi suites) with no `$DISPLAY` present.
- **P1 (next)** - the actual MCP server over stdio (JSON-RPC 2.0): load/pause/resume,
  run N frames, screenshot, memory read/write, CPU state, disassembly.
  See [MCP_PLAN.md](MCP_PLAN.md) for the full roadmap.

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
