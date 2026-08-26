//MesenMCP - emulator session layer
//
//Owns the Emulator instance for one mesen-mcp process (one ROM per process is the
//MCP session model). Carries the headless-only lifecycle lessons from P0:
//
// 1. The core expects the front-end to seed the NES palette
//    (NesConfig::UserPalette) - otherwise every decoded pixel is black.
// 2. Deterministic settings (zeroed power-on RAM, no run-ahead, no frame
//    skipping) are required for reproducible tooling; Mesen's defaults are
//    deliberately non-deterministic (random RAM, RunAheadFrames=1).
// 3. EmulationFlags set BEFORE LoadRom are reset while the ROM loads - speed
//    flags must be applied after the load completes.
#pragma once
#include "Core/Shared/Emulator.h"
#include "Mcp/HeadlessRenderer.h"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>

using json = nlohmann::json;

class EmuSession
{
public:
	//homeFolder: directory for emulator state (settings, battery RAM, logs).
	//Created if needed. All emulator state stays inside it.
	EmuSession(const std::string& homeFolder, bool verboseLog);
	~EmuSession();

	EmuSession(const EmuSession&) = delete;
	EmuSession& operator=(const EmuSession&) = delete;

	//--- Tool implementations (arguments come in as MCP tool arguments) ---
	//Each returns a json object with either "result" (tool succeeded, payload
	//for the caller) or "error" (human-readable message; the server layer
	//turns that into an MCP isError response).

	json LoadRom(const json& args);
	json UnloadRom();
	json Reset(const json& args);
	json Pause();
	json Resume();
	json SetSpeed(const json& args);
	json RunFrames(const json& args);
	json Screenshot(const json& args);
	json GetStatus();

	bool IsRomLoaded();

	HeadlessRenderer* GetRenderer() { return _renderer.get(); }
	Emulator* GetEmulator() { return _emu.get(); }

	static constexpr const char* Version = "0.1.0";

private:
	void ApplyDeterministicSettings(bool deterministic);
	void ApplySpeed(bool maximumSpeed);
	json BuildStatus();

	std::string _homeFolder;
	bool _verboseLog;
	bool _maximumSpeed = true;

	std::unique_ptr<Emulator> _emu;
	std::unique_ptr<HeadlessRenderer> _renderer;
};
