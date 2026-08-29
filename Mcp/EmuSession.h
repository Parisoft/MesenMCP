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
// 4. The debugger is initialized lazily (on the first debug tool call) and is
//    never torn down while the emulation runs - Emulator::StopDebugger() can
//    deadlock at MaximumSpeed (P0 finding). The debugger survives ROM reloads
//    on its own (Emulator::InternalLoadRom preserves it).
#pragma once
#include "Core/Shared/Emulator.h"
#include "Core/Shared/RecordedRomTest.h"
#include "Core/Debugger/Debugger.h"
#include "Mcp/HeadlessRenderer.h"
#include "Mcp/NotificationBridge.h"
#include "Mcp/DbgData.h"
#include "Mcp/VirtualInputProvider.h"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <vector>

using json = nlohmann::json;

//User-facing breakpoint model (kept in the session; pushed to the core in full
//on every change, matching Debugger::SetBreakpoints' replace-all semantics)
struct BreakpointSpec
{
	uint32_t id = 0;
	CpuType cpuType = CpuType::Nes;
	MemoryType memoryType = MemoryType::NesMemory;
	int32_t startAddress = 0;
	int32_t endAddress = 0;
	std::string access;      //"read", "write", "execute" (comma-combined)
	std::string condition;   //Mesen debugger expression, optional
	bool enabled = true;
};

class EmuSession
{
public:
	//homeFolder: directory for emulator state (settings, battery RAM, logs).
	//Created if needed. All emulator state stays inside it.
	EmuSession(const std::string& homeFolder, bool verboseLog);
	~EmuSession();

	EmuSession(const EmuSession&) = delete;
	EmuSession& operator=(const EmuSession&) = delete;

	//--- Tier 0 tools ---
	json LoadRom(const json& args);
	json UnloadRom();
	json Reset(const json& args);
	json Pause();
	json Resume();
	json SetSpeed(const json& args);
	json RunFrames(const json& args);
	json Screenshot(const json& args);
	json GetStatus();

	//--- Tier 1/2 tools (debugger-backed) ---
	json GetCpuState(const json& args);
	json GetPpuState(const json& args);
	json GetRegisters(const json& args);
	json GetMemorySize(const json& args);
	json ReadMemory(const json& args);
	json WriteMemory(const json& args);
	json SearchMemory(const json& args);
	json Disassemble(const json& args);
	json EvaluateExpression(const json& args);
	json SetBreakpoint(const json& args);
	json RemoveBreakpoint(const json& args);
	json Step(const json& args);
	json Continue();
	json WaitForBreak(const json& args);
	json GetCallstack(const json& args);
	json Trace(const json& args);
	json GetDebuggerStatus(const json& args);

	//--- Tier 3/4 tools (input & validation) ---
	json SetController(const json& args);
	json ReleaseController(const json& args);
	json SaveState(const json& args);
	json LoadState(const json& args);
	json RunLuaScript(const json& args);
	json GetLuaScriptLog(const json& args);
	json StopLuaScript(const json& args);
	json GetCdlStats(const json& args);
	json RunRomTest(const json& args);
	json RecordRomTest(const json& args);
	json StopRomTestRecord();
	json SetCheats(const json& args);
	json CaptureGif(const json& args);

	//--- P4 tools (PPU inspection, audio, trace files) ---
	json GetPalette(const json& args);
	json GetTilemap(const json& args);
	json GetTiles(const json& args);
	json GetSprites(const json& args);
	json GetAudioSummary(const json& args);
	json LoadDbgFile(const json& args);
	json FindLabels(const json& args);
	json ListSourceFiles(const json& args);
	json CaptureWav(const json& args);
	json TraceToFile(const json& args);

	bool IsRomLoaded();

	HeadlessRenderer* GetRenderer() { return _renderer.get(); }
	Emulator* GetEmulator() { return _emu.get(); }

	static constexpr const char* Version = "0.3.0";

private:
	void ApplyDeterministicSettings(bool deterministic);
	void ApplySpeed(bool maximumSpeed);
	json BuildStatus();

	//Debugger helpers. EnsureDebugger() returns nullptr and fills `error` when
	//the debugger cannot be started (no ROM, etc.). GetDebuggerOrNull() returns
	//the Debugger* when already initialized (RAII via DebuggerRequest).
	Debugger* EnsureDebugger(const json& args, std::string& error);
	Debugger* GetDebuggerOrNull();
	void PushBreakpoints(Debugger* dbg);

	CpuType ResolveCpu(const json& args, std::string& error);
	DbgData::Label* FindDbgLabel(const std::string& name);
	bool ResolveDbgSourceRange(const json& args, int32_t& startAddress, int32_t& endAddress,
		MemoryType& memType, std::string& description, std::string& error);
	bool ResolveMemoryType(const std::string& name, MemoryType& type, std::string& error);
	std::vector<std::pair<std::string, MemoryType>> GetAvailableMemoryTypes(Debugger* dbg);
	json SerializeCpuState(Debugger* dbg, CpuType cpu);
	json SerializePpuState(Debugger* dbg, CpuType cpu);
	json BreakSummary(Debugger* dbg);

	std::string _homeFolder;
	bool _verboseLog;
	bool _maximumSpeed = true;

	std::unique_ptr<Emulator> _emu;
	std::unique_ptr<HeadlessRenderer> _renderer;
	std::shared_ptr<NotificationBridge> _bridge;
	std::unique_ptr<DbgData> _dbg;
	std::unique_ptr<VirtualInputProvider> _input;
	std::unique_ptr<class WavCaptureDevice> _audio;

	std::vector<BreakpointSpec> _breakpoints;
	uint32_t _nextBreakpointId = 1;
	std::shared_ptr<RecordedRomTest> _romTestRecorder;
	uint32_t _saveStateCounter = 0;
};
