# Compiling MesenMCP

Linux (and macOS) with a C++17 compiler:

```
make          # -> bin/mesen-mcp
make test     # headless smoke test (generates Mcp/tests/red.nes, runs 300 frames, writes a PNG)
```

There are no external dependencies beyond the C++ standard library and pthreads -
no SDL2, no X11, no .NET SDK. python3 is only needed for `make test` (test ROM
generator).

Options:

- `DEBUG=1 make` - unoptimized build with debug symbols
- `SANITIZER=address make` / `SANITIZER=thread` - sanitizer builds
- `CXX=clang++ make` - use Clang instead of g++ (usually produces faster code)

The binary runs without any display server (`DISPLAY` unset is fine and is the
intended environment).
