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
- `SDKROOT=/path/to/MacOSX.sdk make` - macOS only: build against a specific SDK (see below)

The binary runs without any display server (`DISPLAY` unset is fine and is the
intended environment).

## macOS

Install the Xcode Command Line Tools (`xcode-select --install`) and run `make`. The default
compiler (`g++`, which on macOS is Apple's clang) needs nothing else.

The makefile passes `-isysroot "$(xcrun --show-sdk-path)"` to both the compiler and the
linker. That is what lets a standalone LLVM, such as Homebrew's `llvm` / `llvm@15`, find the
system headers and libraries:

```
CXX=/opt/homebrew/opt/llvm@15/bin/clang++ make
```

Unlike Apple's clang, a standalone LLVM typically has no default SDK, so without the flag it fails with
`fatal error: 'stdio.h' file not found` (libc++'s `stdio.h` forwards to the SDK's with
`#include_next`). Set `SDKROOT=/path/to/MacOSX.sdk` to use a different SDK than the one
`xcrun` reports; an `SDKROOT` already exported in your shell is honoured too.
