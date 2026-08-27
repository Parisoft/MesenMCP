//MesenMCP - tool registry
#include "Mcp/ToolRegistry.h"

using json = nlohmann::json;

namespace
{
	json TextContent(const std::string& text)
	{
		return json{ {"type", "text"}, {"text", text} };
	}

	json ObjectSchema(const std::map<std::string, json>& properties,
		const std::vector<std::string>& required,
		const std::map<std::string, std::string>& propertyDescriptions)
	{
		json props = json::object();
		for(auto& [name, type] : properties) {
			json prop{ {"type", type} };
			auto desc = propertyDescriptions.find(name);
			if(desc != propertyDescriptions.end()) {
				prop["description"] = desc->second;
			}
			props[name] = prop;
		}
		json schema{
			{"type", "object"},
			{"properties", props},
			{"additionalProperties", false}
		};
		if(!required.empty()) {
			schema["required"] = required;
		}
		return schema;
	}

	json ToolCallResponse(const json& handlerResult)
	{
		//EmuSession returns either {"result": ...} or {"error": "..."}.
		json response;
		if(handlerResult.contains("error")) {
			response["isError"] = true;
			response["content"] = json::array({ TextContent(handlerResult["error"].get<std::string>()) });
		} else {
			response["isError"] = false;
			const json& result = handlerResult["result"];
			std::string text;
			if(result.is_string()) {
				text = result.get<std::string>();
			} else {
				//Screenshots return an image alongside the json summary
				if(result.contains("image_base64")) {
					response["content"].push_back(json{
						{"type", "image"},
						{"data", result["image_base64"]},
						{"mimeType", "image/png"}
					});
					json summary = result;
					summary.erase("image_base64");
					text = summary.dump(2);
				} else {
					text = result.dump(2);
				}
			}
			response["content"].push_back(TextContent(text));
		}
		return response;
	}
}

ToolRegistry::ToolRegistry(EmuSession& session) : _session(session)
{
	Register({
		"load_rom",
		"Load a ROM into the emulator session (replaces any ROM currently loaded) and start "
		"running it. Supported: NES (.nes/.fds/.nsf/.unif), SNES (.sfc/.smc), Game Boy (.gb), "
		"GBA (.gba - requires gba_bios.bin in the firmware folder), PCE, SMS, WonderSwan. "
		"Archives (.zip/.7z) containing a ROM are also supported. By default the emulator runs "
		"at maximum speed (no 60fps limiter) with deterministic settings (zeroed power-on RAM, "
		"no run-ahead) so runs are reproducible - use run_frames to advance a controlled number "
		"of frames and screenshot/pause to inspect state.",
		ObjectSchema(
			{
				{"path", "string"},
				{"patch_path", "string"},
				{"region", "string"},
				{"deterministic", "boolean"},
				{"speed", "string"}
			},
			{ "path" },
			{
				{"path", "absolute path to the ROM file"},
				{"patch_path", "optional IPS/BPS/UPS patch to apply on top of the ROM"},
				{"region", "force the console region: 'auto' (default), 'ntsc', 'pal' or 'dendy' (NES only)"},
				{"deterministic", "zero power-on RAM and disable non-deterministic features (default true). Set false to test the game against randomized power-on RAM."},
				{"speed", "'max' (default, run unthrottled) or 'realtime' (60fps wall clock)"}
			}),
		[this](const json& args) { return _session.LoadRom(args); }
	});

	Register({
		"unload_rom",
		"Stop the emulator and unload the current ROM (battery RAM is not saved - this is a "
		"throwaway session by design).",
		ObjectSchema({}, {}, {}),
		[this](const json&) { return _session.UnloadRom(); }
	});

	Register({
		"reset",
		"Reset the console. mode='reset' (soft reset, like pressing Reset) or "
		"mode='power_cycle' (full power cycle - RAM returns to its power-on state).",
		ObjectSchema(
			{ {"mode", "string"} },
			{},
			{ {"mode", "'reset' (default) or 'power_cycle'"} }),
		[this](const json& args) { return _session.Reset(args); }
	});

	Register({
		"pause",
		"Pause emulation (the current video frame is kept available for screenshots).",
		ObjectSchema({}, {}, {}),
		[this](const json&) { return _session.Pause(); }
	});

	Register({
		"resume",
		"Resume paused emulation.",
		ObjectSchema({}, {}, {}),
		[this](const json&) { return _session.Resume(); }
	});

	Register({
		"set_speed",
		"Set emulation speed: 'max' disables the frame limiter (default for automated runs), "
		"'realtime' runs at the console's native frame rate.",
		ObjectSchema(
			{ {"speed", "string"} },
			{ "speed" },
			{ {"speed", "'max' or 'realtime'"} }),
		[this](const json& args) { return _session.SetSpeed(args); }
	});

	Register({
		"run_frames",
		"Advance emulation by N video frames (e.g. 60 frames ~= 1 second of NES time). Returns "
		"once the frames have run or timeout_ms elapses (a partial result is returned, not an "
		"error - check the timed_out flag). Use this between inputs/screenshots to control "
		"game time deterministically. Requires emulation to be running (not paused).",
		ObjectSchema(
			{ {"frames", "integer"}, {"timeout_ms", "integer"} },
			{ "frames" },
			{
				{"frames", "number of video frames to advance (1 to 21600000; 60 frames = 1 second of NES time)"},
				{"timeout_ms", "how long to wait before returning a partial result (default 10000; at 'max' speed typical throughput is 500+ fps)"}
			}),
		[this](const json& args) { return _session.RunFrames(args); }
	});

	Register({
		"screenshot",
		"Capture the most recent video frame as a PNG image (native console resolution, e.g. "
		"256x240 for NES). Returned as an MCP image; optionally also saved to disk. If no frame "
		"exists yet, run some frames first.",
		ObjectSchema(
			{ {"save_path", "string"} },
			{},
			{ {"save_path", "optional absolute path to additionally write the PNG file to"} }),
		[this](const json& args) { return _session.Screenshot(args); }
	});

	Register({
		"get_status",
		"Get emulator status: whether a ROM is loaded, pause state, speed mode, frame count, "
		"fps, console type, region, ROM identity (sha1/crc32), current video dimensions and "
		"the Mesen core version. Call this first when discovering an existing session.",
		ObjectSchema({}, {}, {}),
		[this](const json&) { return _session.GetStatus(); }
	});

	//--- Tier 1/2: inspection & debugging ---

	Register({
		"get_cpu_state",
		"Get the CPU registers of the emulated console (PC, A, X, Y, SP, flags, cycle count - "
		"exact fields depend on the console: 6502 for NES, 65816 for SNES, ARM7TDMI for GBA). "
		"The optional 'cpu' argument selects a sub-processor on consoles that have several "
		"(e.g. 'spc' on SNES); the default is the main CPU.",
		ObjectSchema({ {"cpu", "string"} }, {}, { {"cpu", "cpu to inspect, e.g. 'nes' (default), 'spc', 'sa1', 'gba'"} }),
		[this](const json& args) { return _session.GetCpuState(args); }
	});

	Register({
		"get_ppu_state",
		"Get the picture-processing-unit state: current scanline/cycle, frame count, rendering "
		"flags (background/sprite enable), VRAM scroll address, vblank status (NES), BG mode "
		"(SNES/GBA), etc. Useful to verify where in the frame the console is and whether "
		"rendering is enabled.",
		ObjectSchema({ {"cpu", "string"} }, {}, { {"cpu", "relevant for consoles with multiple PPUs"} }),
		[this](const json& args) { return _session.GetPpuState(args); }
	});

	Register({
		"get_memory_size",
		"Get the size of a memory region. Use this to discover valid memory_type values - "
		"calling it with an invalid name returns the list of available types.",
		ObjectSchema({ {"memory_type", "string"} }, { "memory_type" },
			{ {"memory_type", "memory region name, e.g. 'cpu', 'prg_rom', 'work_ram', 'save_ram', 'vram', 'oam', 'palette_ram', 'chr_rom'"} }),
		[this](const json& args) { return _session.GetMemorySize(args); }
	});

	Register({
		"read_memory",
		"Read bytes from a memory region. NES examples: memory_type 'cpu' addresses $0000-$FFFF "
		"(2KB internal RAM at $0000-$07FF with mirrors, PPU registers $2000-$3FFF, APU $4000-$401F, "
		"cart space $4020+); 'work_ram' is the raw 2KB; 'nametable_ram' the VRAM nametables; "
		"'sprite_ram' (OAM) the 256-byte sprite table; 'palette_ram' the 32-byte palette; "
		"'prg_rom'/'chr_rom' the cartridge ROMs. Returns hex (and a byte array for short reads).",
		ObjectSchema(
			{
				{"memory_type", "string"},
				{"address", "integer"},
				{"length", "integer"}
			},
			{ "memory_type", "address" },
			{
				{"memory_type", "memory region to read (see get_memory_size)"},
				{"address", "start address (int, or hex string like '$2002' or '0x2002')"},
				{"length", "number of bytes, 1..1048576 (default 1)"}
			}),
		[this](const json& args) { return _session.ReadMemory(args); }
	});

	Register({
		"write_memory",
		"Write bytes to a memory region (RAM, VRAM, OAM, palette - writing to ROM regions is "
		"ignored on real cartridges). This is a debugger write: register addresses written "
		"through 'cpu' do NOT trigger hardware side effects (e.g. writing $2006 via this tool "
		"does not change the PPU address latch - write the underlying PPU memory types instead).",
		ObjectSchema(
			{
				{"memory_type", "string"},
				{"address", "integer"},
				{"bytes", "integer"}
			},
			{ "memory_type", "address", "bytes" },
			{
				{"memory_type", "memory region to write"},
				{"address", "start address (int or hex string)"},
				{"bytes", "array of byte values 0-255, or a hex string like \"8D 06 20\""}
			}),
		[this](const json& args) { return _session.WriteMemory(args); }
	});

	Register({
		"search_memory",
		"Search a memory region for a byte pattern or a numeric value and return the matching "
		"addresses. Values are matched in the console's native little-endian byte order. "
		"Typical uses: find where a score/lives counter lives ('value' + value_size), find "
		"code signatures in prg_rom (hex pattern).",
		ObjectSchema(
			{
				{"memory_type", "string"},
				{"value", "integer"},
				{"value_size", "integer"},
				{"start", "integer"},
				{"max_results", "integer"}
			},
			{ "memory_type", "value" },
			{
				{"memory_type", "memory region to search"},
				{"value", "byte pattern as hex string (e.g. \"A9 16\") or integer value"},
				{"value_size", "for integer values: 1, 2 or 4 bytes (default 1)"},
				{"start", "start address (default 0)"},
				{"max_results", "maximum matches to return (default 100)"}
			}),
		[this](const json& args) { return _session.SearchMemory(args); }
	});

	Register({
		"disassemble",
		"Disassemble instructions starting at an address (default: the current PC). Returns "
		"address + assembly text rows. Supports the console's CPU (6502 for NES, 65816 for "
		"SNES, ARM/Thumb for GBA).",
		ObjectSchema(
			{
				{"address", "integer"},
				{"count", "integer"},
				{"cpu", "string"}
			},
			{},
			{
				{"address", "start address (default: current PC; int or hex string)"},
				{"count", "number of instructions, 1..500 (default 20)"},
				{"cpu", "cpu to disassemble for (default: main cpu)"}
			}),
		[this](const json& args) { return _session.Disassemble(args); }
	});

	Register({
		"evaluate_expression",
		"Evaluate a Mesen debugger expression against the current state. Hex literals are "
		"'$xx' (e.g. '$2002'), registers by name (A, X, Y, SP, PC...), and memory is read "
		"with brackets: '[$0700]' reads the byte at $0700, '[$42] == $5A' compares zero page. "
		"Examples: '[$2002] & $80' (vblank flag), 'A + X', 'PC == $C000'. This is also the "
		"language of breakpoint conditions and trace conditions. Memory reads through this "
		"tool are side-effect-free.",
		ObjectSchema(
			{
				{"expression", "string"},
				{"cpu", "string"}
			},
			{ "expression" },
			{
				{"expression", "expression to evaluate"},
				{"cpu", "context cpu (default: main cpu)"}
			}),
		[this](const json& args) { return _session.EvaluateExpression(args); }
	});

	Register({
		"set_breakpoint",
		"Set a breakpoint and leave emulation running until it is hit (use wait_for_breakpoint "
		"or step to observe the hit). 'address' may be a single address or a range "
		"(address..end_address). 'access' selects the trigger: 'execute' (default), 'read', "
		"'write', or a combination like 'read,write'. 'condition' is a debugger expression "
		"that must be true for the break to occur (e.g. 'A == 0'). Breaks report a breakpoint "
		"id used with remove_breakpoint.",
		ObjectSchema(
			{
				{"address", "integer"},
				{"end_address", "integer"},
				{"access", "string"},
				{"memory_type", "string"},
				{"condition", "string"},
				{"cpu", "string"}
			},
			{ "address" },
			{
				{"address", "address to break on (int or hex string)"},
				{"end_address", "optional end of an address range"},
				{"access", "'execute' (default), 'read', 'write', or combinations like 'read,write'"},
				{"memory_type", "memory region the address refers to (default: cpu bus)"},
				{"condition", "optional expression that must evaluate true to trigger"},
				{"cpu", "cpu the breakpoint applies to (default: main cpu)"}
			}),
		[this](const json& args) { return _session.SetBreakpoint(args); }
	});

	Register({
		"remove_breakpoint",
		"Remove a breakpoint by id, or all breakpoints with all=true.",
		ObjectSchema(
			{ {"id", "integer"}, {"all", "boolean"} },
			{},
			{ {"id", "id returned by set_breakpoint"}, {"all", "set true to remove every breakpoint"} }),
		[this](const json& args) { return _session.RemoveBreakpoint(args); }
	});

	Register({
		"step",
		"Advance execution by stepping. Types: 'instruction' (default), 'over' (step over "
		"subroutine calls), 'out' (run until the current subroutine returns), 'cycle' (CPU "
		"cycles), 'scanline', 'frame'. Waits until the step completes (timeout_ms, default "
		"5000) and returns the CPU state at the stop. Starting the debugger with a step while "
		"the game runs freely will stop at the next instruction boundary.",
		ObjectSchema(
			{
				{"type", "string"},
				{"count", "integer"},
				{"timeout_ms", "integer"},
				{"cpu", "string"}
			},
			{},
			{
				{"type", "instruction (default), over, out, cycle, scanline, frame"},
				{"count", "number of steps (default 1)"},
				{"cpu", "cpu to step (default: main cpu)"}
			}),
		[this](const json& args) { return _session.Step(args); }
	});

	Register({
		"continue",
		"Resume execution after a breakpoint/step (clears the debugger's stop).",
		ObjectSchema({}, {}, {}),
		[this](const json&) { return _session.Continue(); }
	});

	Register({
		"wait_for_breakpoint",
		"Block until a breakpoint is hit (or timeout_ms elapses, default 5000). Returns "
		"whether the CPU is stopped, which breakpoint triggered (id, cpu, source) and the "
		"CPU state at the break. This is the main synchronization primitive between 'set "
		"breakpoint + run' and inspecting state.",
		ObjectSchema({ {"timeout_ms", "integer"} }, {},
			{ {"timeout_ms", "how long to wait (default 5000; use longer for sparse breakpoints)"} }),
		[this](const json& args) { return _session.WaitForBreak(args); }
	});

	Register({
		"get_callstack",
		"Get the current call stack (subroutine call chain) for the CPU.",
		ObjectSchema({ {"cpu", "string"} }, {}, { {"cpu", "cpu to inspect (default: main cpu)"} }),
		[this](const json& args) { return _session.GetCallstack(args); }
	});

	Register({
		"trace",
		"Enable instruction tracing and/or fetch the most recent trace rows (each row is a "
		"formatted line with PC and disassembly). Optionally runs N frames while tracing. "
		"Custom 'format' strings use Mesen trace tags (e.g. '[PC] [A] [Disassembly]'); "
		"the default is '[PC] [Disassembly]'.",
		ObjectSchema(
			{
				{"enable", "boolean"},
				{"format", "string"},
				{"condition", "string"},
				{"run_frames", "integer"},
				{"rows", "integer"},
				{"cpu", "string"}
			},
			{},
			{
				{"enable", "enable/disable the trace logger (default true)"},
				{"format", "trace row format (Mesen trace tags)"},
				{"condition", "only trace rows matching this expression"},
				{"run_frames", "run this many frames before fetching rows (default 0)"},
				{"rows", "number of recent rows to return, 1..500 (default 50)"},
				{"cpu", "cpu to trace (default: main cpu)"}
			}),
		[this](const json& args) { return _session.Trace(args); }
	});

	Register({
		"get_debugger_status",
		"Get debugger state: whether the debugger is active, whether execution is stopped "
		"(at a breakpoint/step), the breakpoint list, available cpus and memory types for "
		"this console.",
		ObjectSchema({}, {}, {}),
		[this](const json& args) { return _session.GetDebuggerStatus(args); }
	});
	//--- Tier 3/4: input & validation ---

	Register({
		"set_controller",
		"Press buttons on a virtual controller. Buttons: up, down, left, right, start, "
		"select, b, a (+ x, y, l, r on SNES; + l, r on GBA). Pass an empty list to release. "
		"With hold_frames=N the buttons are held for exactly N frames (the tool advances "
		"them for you); without it they stay held until the next set_controller call. Use "
		"with run_frames + screenshot/read_memory to drive menus and gameplay.",
		ObjectSchema(
			{
				{"port", "integer"},
				{"buttons", "integer"},
				{"hold_frames", "integer"},
				{"timeout_ms", "integer"}
			},
			{ "buttons" },
			{
				{"port", "controller port, 1-4 (default 1)"},
				{"buttons", "array of button names, or a string like 'start' or 'a,start'"},
				{"hold_frames", "hold the buttons for this many frames (default: until changed)"}
			}),
		[this](const json& args) { return _session.SetController(args); }
	});

	Register({
		"release_controller",
		"Release all buttons on a virtual controller and stop overriding it.",
		ObjectSchema({ {"port", "integer"} }, {}, { {"port", "controller port (default 1)"} }),
		[this](const json& args) { return _session.ReleaseController(args); }
	});

	Register({
		"save_state",
		"Save the current emulation state to a file. Returns the path. Default location: "
		 "the session home directory. Pair with load_state for A/B experiments without "
		 "replaying inputs.",
		ObjectSchema({ {"path", "string"} }, {}, { {"path", "absolute path for the .mss file (default: auto-numbered in the session home)"} }),
		[this](const json& args) { return _session.SaveState(args); }
	});

	Register({
		"load_state",
		"Load a previously saved state file (restores CPU/RAM/PPU exactly). The currently "
		"loaded ROM must match the state file.",
		ObjectSchema({ {"path", "string"} }, { "path" }, { {"path", "path returned by save_state"} }),
		[this](const json& args) { return _session.LoadState(args); }
	});

	Register({
		"run_lua_script",
		"Run a Lua script inside the emulator using Mesen's scripting API: emu.read/"
		"emu.write (address, emu.memType.<type>) and emu.getMemorySize, callbacks via "
		"emu.addEventCallback (frame/nmi/sprite0/etc.), emu.createSavestate/"
		"loadSavestate, emu.getCpuState, HUD drawing (emu.drawString etc.), "
		"emu.addCheat, and more. Print output is captured and "
		"returned. By default the script is stopped after the optional run_frames window; "
		"pass auto_stop=false to keep an event-callback script resident (stop later with "
		"stop_lua_script). This is the power tool for arbitrary test logic - e.g. assert "
		"RAM values every frame, automate input, or measure coverage.",
		ObjectSchema(
			{
				{"code", "string"},
				{"run_frames", "integer"},
				{"auto_stop", "boolean"},
				{"timeout_ms", "integer"},
				{"cpu", "string"}
			},
			{ "code" },
			{
				{"code", "Lua source code"},
				{"run_frames", "advance this many frames while the script runs (default 0)"},
				{"auto_stop", "stop the script after running (default true)"}
			}),
		[this](const json& args) { return _session.RunLuaScript(args); }
	});

	Register({
		"get_lua_script_log",
		"Fetch the print output of a resident Lua script.",
		ObjectSchema({ {"script_id", "integer"}, {"cpu", "string"} }, { "script_id" },
			{ {"script_id", "id returned by run_lua_script"} }),
		[this](const json& args) { return _session.GetLuaScriptLog(args); }
	});

	Register({
		"stop_lua_script",
		"Stop a resident Lua script (started with run_lua_script + auto_stop=false).",
		ObjectSchema({ {"script_id", "integer"}, {"cpu", "string"} }, { "script_id" },
			{ {"script_id", "id returned by run_lua_script"} }),
		[this](const json& args) { return _session.StopLuaScript(args); }
	});

	Register({
		"get_cdl_stats",
		"Code/data logger statistics - how much of the cartridge program ROM has been "
		"executed as code or accessed as data so far (a proxy for test coverage of the "
		"ROM). Great for validation reports: 'the test run exercised 34% of PRG'. Also "
		"reports the number of distinct functions entered.",
		ObjectSchema({ {"memory_type", "string"}, {"cpu", "string"} }, {},
			{ {"memory_type", "region to analyze (default: the console's prg_rom)"} }),
		[this](const json& args) { return _session.GetCdlStats(args); }
	});

	Register({
		"run_rom_test",
		"Run a recorded ROM test (.mntest, Mesen's CI test format: a zip containing the "
		"ROM, recorded inputs and per-frame screen hashes). The test runs in a separate "
		"background emulator - your current session (ROM, breakpoints, memory edits) is "
		"untouched. Returns passed/failed and the number of mismatched frames. Combine "
		"with record_rom_test to build regression suites: record a known-good run once, "
		"replay it after every change.",
		ObjectSchema({ {"path", "string"} }, { "path" },
			{ {"path", "path to the .mntest file"} }),
		[this](const json& args) { return _session.RunRomTest(args); }
	});

	Register({
		"record_rom_test",
		"Start recording a .mntest ROM test from the CURRENT session: drive the game with "
		"set_controller/run_frames; every frame's screen hash and all inputs are recorded. "
		"Stop with stop_rom_test_record. Replay later with run_rom_test (also loadable in "
		"the Mesen GUI).",
		ObjectSchema(
			{
				{"path", "string"},
				{"reset", "boolean"}
			},
			{ "path" },
			{
				{"path", "path of the .mntest file to create"},
				{"reset", "reset the console when recording starts (default false)"}
			}),
		[this](const json& args) { return _session.RecordRomTest(args); }
	});

	Register({
		"stop_rom_test_record",
		"Finish and write out the .mntest file started by record_rom_test.",
		ObjectSchema({}, {}, {}),
		[this](const json&) { return _session.StopRomTestRecord(); }
	});

	Register({
		"set_cheats",
		"Apply Game Genie / Pro Action Replay cheat codes (the encoding family is detected "
		"from the code format and console). Pass codes: [] to clear all cheats. Useful to "
		"bypass level locks, get invincibility etc. when testing late-game behavior.",
		ObjectSchema({ {"codes", "integer"} }, { "codes" },
			{ {"codes", "array of cheat code strings, e.g. [\"SXIOPO\", \"81EFA2\"] (empty array clears)"} }),
		[this](const json& args) { return _session.SetCheats(args); }
	});

	Register({
		"capture_gif",
		"Record N frames of gameplay as an animated GIF (native resolution). Returns the "
		"file path (and optionally base64) - useful as a bug report attachment or to "
		"visually verify a sequence of actions.",
		ObjectSchema(
			{
				{"frames", "integer"},
				{"path", "string"},
				{"return_base64", "boolean"},
				{"timeout_ms", "integer"}
			},
			{},
			{
				{"frames", "number of frames to record (default 60, max 3600)"},
				{"path", "output .gif path (default: auto in the session home)"},
				{"return_base64", "also return the GIF base64-encoded for small captures (<= 8MB)"}
			}),
		[this](const json& args) { return _session.CaptureGif(args); }
	});
	//--- P4: PPU inspection, audio & trace files ---

	Register({
		"get_palette",
		"Get the console's palette RAM as decoded RGB colors (plus raw values): NES "
		"background + sprite palettes, SNES CGRAM, GBA palette RAM, etc. NES returns "
		"4x4 bg + 4x4 sprite entries of the 2C02 palette; entry 0 is the backdrop "
		"color shown when rendering is off.",
		ObjectSchema({ {"cpu", "string"} }, {}, { {"cpu", "default: main cpu"} }),
		[this](const json& args) { return _session.GetPalette(args); }
	});

	Register({
		"get_tilemap",
		"Render a background layer's tilemap to a PNG image (like the GUI tilemap "
		"viewer). Shows the full nametable/map including parts scrolled off-screen, "
		"with scroll position and layout metadata. NES: layer 0 = nametables. SNES/GBA/"
		"PCE: layers 1-4 (BG modes apply). Answers 'why is my background garbage' "
		"without decoding nametables by hand.",
		ObjectSchema(
			{
				{"layer", "integer"},
				{"save_path", "string"},
				{"cpu", "string"}
			},
			{},
			{
				{"layer", "background layer number (NES: 0 = nametables; SNES/PCE/GBA: 0-3 = BG1-4, default 0)"},
				{"save_path", "optional path to also write the PNG"}
			}),
		[this](const json& args) { return _session.GetTilemap(args); }
	});

	Register({
		"get_tiles",
		"Render the tileset (pattern tables / VRAM tiles) to a PNG image (like the GUI "
		"tile viewer). NES: CHR ROM/RAM as 16-tiles-per-row grid, 'palette' selects "
		"which of the 8 4-color palettes to apply (0-7, default 0). Use grayscale=true "
		"to view raw bitplanes without palette coloring.",
		ObjectSchema(
			{
				{"palette", "integer"},
				{"width", "integer"},
				{"start", "integer"},
				{"grayscale", "boolean"},
				{"save_path", "string"},
				{"cpu", "string"}
			},
			{},
			{
				{"palette", "palette index for coloring, NES 0-7 (default 0)"},
				{"width", "tiles per row (default 16, max 64)"},
				{"start", "start byte offset in the tile data (default 0)"},
				{"grayscale", "show bitplanes in grayscale instead of palette colors"},
				{"save_path", "optional path to also write the PNG"}
			}),
		[this](const json& args) { return _session.GetTiles(args); }
	});

	Register({
		"get_sprites",
		"List the sprite table (OAM) with decoded positions/sizes/tiles/palettes and a "
		"PNG preview of where sprites sit on screen. NES returns all 64 OAM entries "
		"(visibility: visible/offscreen/clipped). Use it to check sprite flicker, "
		"off-by-one placement or OAM corruption.",
		ObjectSchema({ {"cpu", "string"} }, {}, { {"cpu", "default: main cpu"} }),
		[this](const json& args) { return _session.GetSprites(args); }
	});

	Register({
		"get_audio_summary",
		"Audio verification: RMS level and peak per channel plus a clipping-sample "
		"count, over a recent window (window_ms, default 1000ms) and since load. "
		"Answers 'is the music actually playing' (rms ~0 = silence) and 'is it too "
		"loud' (clipping samples > 0, peak near 1.0). Note: the APU must be running - "
		"write $4015/$4000-$4013 or let the game's sound engine play.",
		ObjectSchema({ {"window_ms", "integer"} }, {},
			{ {"window_ms", "size of the recent-stats window (default 1000ms)"} }),
		[this](const json& args) { return _session.GetAudioSummary(args); }
	});

	Register({
		"capture_wav",
		"Record N frames of emulated audio to a WAV file (the actual mixed output). "
		"Returns the path (and optionally base64) - listen to what the game sounds "
		"like, or analyze the file yourself.",
		ObjectSchema(
			{
				{"frames", "integer"},
				{"path", "string"},
				{"return_base64", "boolean"},
				{"timeout_ms", "integer"}
			},
			{},
			{
				{"frames", "number of frames to record (default 60, max 3600)"},
				{"path", "output .wav path (default: auto in the session home)"},
				{"return_base64", "also return the WAV base64-encoded for small captures (<= 8MB)"}
			}),
		[this](const json& args) { return _session.CaptureWav(args); }
	});

	Register({
		"trace_to_file",
		"Log the instruction trace to a file (no row-count limits - for long captures "
		"that would exceed the in-memory 'trace' tool's budget). action='start' begins "
		"logging to path (optional run_frames advances the game while logging; "
		"auto_stop=true stops immediately after), action='stop' flushes and closes.",
		ObjectSchema(
			{
				{"action", "string"},
				{"path", "string"},
				{"format", "string"},
				{"condition", "string"},
				{"run_frames", "integer"},
				{"auto_stop", "boolean"},
				{"use_labels", "boolean"},
				{"cpu", "string"}
			},
			{},
			{
				{"action", "'start' (default) or 'stop'"},
				{"path", "output file (default: auto in the session home)"},
				{"format", "trace row format (default '[PC] [Disassembly]')"},
				{"condition", "only trace rows matching this expression"},
				{"run_frames", "advance this many frames while logging"},
				{"auto_stop", "stop logging after run_frames completes (default false)"}
			}),
		[this](const json& args) { return _session.TraceToFile(args); }
	});
}

void ToolRegistry::Register(ToolDefinition tool)
{
	_tools.push_back(std::move(tool));
}

json ToolRegistry::GetToolsList() const
{
	json tools = json::array();
	for(const ToolDefinition& tool : _tools) {
		tools.push_back(json{
			{"name", tool.name},
			{"description", tool.description},
			{"inputSchema", tool.inputSchema}
		});
	}
	return json{ {"tools", tools} };
}

json ToolRegistry::CallTool(const std::string& name, const json& args) const
{
	for(const ToolDefinition& tool : _tools) {
		if(tool.name == name) {
			try {
				return ToolCallResponse(tool.handler(args));
			} catch(std::exception& e) {
				return json{
					{"isError", true},
					{"content", json::array({ TextContent(std::string("internal tool error: ") + e.what()) })}
				};
			}
		}
	}

	return json{
		{"isError", true},
		{"content", json::array({ TextContent("unknown tool: " + name) })}
	};
}
