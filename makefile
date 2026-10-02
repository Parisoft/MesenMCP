#MesenMCP build - headless MCP server for the Mesen emulation core.
#
#Builds a single native binary (bin/mesen-mcp) from Core + Utilities + Lua + SevenZip + Mcp.
#No SDL, no X11, no evdev, no .NET - the binary runs in environments with no display server.
#
#Run "make" to build, "make test" to build + run the headless smoke test, "make clean" to clean.
#
#Useful variables:
#  DEBUG=1          - unoptimized build with debug info
#  SANITIZER=address|thread - build with a sanitizer (DEBUG=1 recommended)
#  CXX=/CC=         - compiler override (default: g++, clang++ also works)
#  SDKROOT=<path>   - macOS only: SDK to build against (default: xcrun --show-sdk-path)

CXX ?= g++
CC ?= gcc

UNAME_S := $(shell uname -s)

DEBUG ?= 0
SANITIZER ?=

OPT_FLAG := -O2
ifeq ($(DEBUG),1)
	OPT_FLAG := -O0 -g
endif

FLAGS :=
ifeq ($(SANITIZER),address)
	FLAGS += -fsanitize=address
else ifeq ($(SANITIZER),thread)
	FLAGS += -fsanitize=thread
endif

#macOS: Apple's /usr/bin/clang is a shim that finds the SDK on its own, but a standalone LLVM
#(e.g. Homebrew's llvm@15) may have no default sysroot, so it can't see the C library headers
#and fails with "'stdio.h' file not found" (libc++'s stdio.h does #include_next <stdio.h>).
#Always point the compiler AND the linker at the SDK explicitly (harmless with Apple's clang).
#An SDKROOT from the environment or the make command line wins over auto-detection.
#(The warning is wrapped in an assignment on purpose: a bare tab-indented $(warning) line would be
#parsed as a recipe line - "recipe commences before first target".)
ifeq ($(UNAME_S),Darwin)
	ifeq ($(strip $(SDKROOT)),)
		SDKROOT := $(shell xcrun --show-sdk-path 2>/dev/null)
	endif
	ifeq ($(strip $(SDKROOT)),)
		SDK_WARNING := $(warning macOS SDK not found - run "xcode-select --install" or set SDKROOT=<path to MacOSX.sdk>)
	else
		FLAGS += -isysroot "$(SDKROOT)"
	endif
endif

#Note: Lua is included as "Lua/lua.hpp" - the repository root must be on the include path.
INCLUDES := -I$(realpath ./) -I$(realpath ./Core) -I$(realpath ./Utilities) -I$(realpath ./Mcp)

CXXFLAGS := -std=c++17 -Wall $(OPT_FLAG) $(FLAGS) $(INCLUDES)
CFLAGS := -Wall $(OPT_FLAG) $(FLAGS) $(INCLUDES)
LDFLAGS := $(FLAGS) -pthread -rdynamic

OUTFILE := bin/mesen-mcp

CORESRC := $(shell find Core -name '*.cpp')
UTILSRC := $(shell find Utilities \( -name '*.cpp' -o -name '*.c' \))
LUASRC := $(shell find Lua -name '*.c')
SEVENZIPSRC := $(shell find SevenZip -name '*.c')
MCPSRC := $(shell find Mcp -name '*.cpp')

SRCS := $(MCPSRC) $(CORESRC) $(UTILSRC) $(LUASRC) $(SEVENZIPSRC)
OBJS := $(addprefix obj/,$(addsuffix .o,$(SRCS)))

.PHONY: all test clean

all: $(OUTFILE)

$(OUTFILE): $(OBJS)
	@mkdir -p bin
	$(CXX) $(LDFLAGS) -o $@ $(OBJS)

obj/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

obj/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

#Headless smoke tests:
#1. P0 CLI: generate a tiny test ROM, run it for 300 frames with no display
#   server and dump a PNG screenshot of the final frame.
#2. MCP stdio protocol: full client session against the server binary.
test: $(OUTFILE)
	python3 Mcp/tests/make_test_rom.py
	$(OUTFILE) --rom Mcp/tests/red.nes --frames 300 --screenshot Mcp/tests/red.png
	python3 Mcp/tests/mcp_smoke_test.py
	python3 Mcp/tests/input_test.py

clean:
	rm -rf obj bin
