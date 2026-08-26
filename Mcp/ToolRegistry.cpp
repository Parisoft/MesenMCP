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
