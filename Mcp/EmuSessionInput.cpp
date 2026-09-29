//MesenMCP - input & validation tool implementations (P3)
#include "Mcp/EmuSession.h"

#include "Core/Shared/DebuggerRequest.h"
#include "Core/Shared/EmuSettings.h"
#include "Core/Shared/SaveStateManager.h"
#include "Core/Shared/CheatManager.h"
#include "Core/Shared/Video/VideoRenderer.h"
#include "Core/Debugger/ScriptManager.h"
#include "Core/Debugger/CdlManager.h"
#include "Core/Debugger/MemoryDumper.h"
#include "Utilities/Video/IVideoRecorder.h"
#include "Utilities/Video/AviWriter.h"
#include "Utilities/Base64.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/HexUtilities.h"
#include "Utilities/Timer.h"
#include "magic_enum.hpp"

#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>

namespace fs = std::filesystem;

namespace
{
	json ErrorResult(const std::string& message)
	{
		return json{ {"error", message} };
	}

	std::string RomTestStateToString(RomTestState state)
	{
		switch(state) {
			case RomTestState::Passed: return "passed";
			case RomTestState::PassedWithWarnings: return "passed_with_warnings";
			case RomTestState::Failed: return "failed";
			default: return "unknown";
		}
	}

	//snake_case of an enum name, e.g. "NesController" -> "nes_controller"
	std::string EnumToSnake(const char* name)
	{
		std::string out;
		for(const char* p = name; *p; p++) {
			if(*p >= 'A' && *p <= 'Z') {
				if(!out.empty() && out.back() != '_') {
					out += '_';
				}
				out += (char)(*p - 'A' + 'a');
			} else {
				out += *p;
			}
		}
		return out;
	}

	CheatType GuessCheatType(const std::string& code, ConsoleType console)
	{
		//Heuristics per console; Mesen decodes the actual address/value in
		//CheatManager::TryConvertCode - this only picks the encoding family.
		switch(console) {
			case ConsoleType::Nes:
				return code.size() == 6 || code.size() == 8 ? CheatType::NesGameGenie : CheatType::NesProActionRocky;
			case ConsoleType::Snes:
				return code.size() == 8 ? CheatType::SnesGameGenie : CheatType::SnesProActionReplay;
			case ConsoleType::Gameboy:
				return code.size() == 9 ? CheatType::GbGameGenie : CheatType::GbGameShark;
			case ConsoleType::PcEngine:
				return CheatType::PceRaw;
			case ConsoleType::Sms:
				return code.size() == 8 ? CheatType::SmsGameGenie : CheatType::SmsProActionReplay;
			default:
				return CheatType::NesCustom;
		}
	}
}

//--- Input --------------------------------------------------------------------

json EmuSession::SetController(const json& args)
{
	if(!IsRomLoaded()) {
		return ErrorResult("no ROM is loaded");
	}

	uint32_t port = args.value("port", 1);
	int32_t holdFrames = args.contains("hold_frames") ? args.value("hold_frames", -1) : -1;

	std::vector<std::string> buttons;
	if(args.contains("buttons")) {
		if(args["buttons"].is_array()) {
			buttons = args["buttons"].get<std::vector<std::string>>();
		} else if(args["buttons"].is_string()) {
			//Also accept a single button name or a comma/space separated list
			std::string list = args["buttons"].get<std::string>();
			std::string token;
			auto flush = [&]() {
				if(!token.empty()) {
					buttons.push_back(token);
					token.clear();
				}
			};
			for(char c : list) {
				if(c == ',' || c == ' ') { flush(); }
				else { token += (char)::tolower(c); }
			}
			flush();
		}
	} else {
		return ErrorResult("missing 'buttons' argument (array of button names, or [] to release)");
	}

	std::string error;
	if(!_input->SetButtons(port, buttons, holdFrames, error)) {
		return ErrorResult(error);
	}

	json result;
	result["port"] = port;
	result["buttons"] = buttons;
	result["hold_frames"] = holdFrames < 0 ? json("until_changed") : json(holdFrames);
	if(holdFrames > 0) {
		//Hold for N frames then advance (so the press is actually sampled),
		//unless the caller passes hold_frames=0 (release immediately).
		json run = RunFrames(json{ {"frames", (uint32_t)holdFrames}, {"timeout_ms", args.value("timeout_ms", 10000)} });
		if(run.contains("error")) {
			return run;
		}
		result["frames_run"] = run["result"]["frames_run"];

		//The countdown ticks on the console's input polls - one per frame, at
		//the vblank scanline - so the auto-release can land just after the last
		//frame run_frames waited for. Wait for it, so a fixed-length press never
		//returns with the button still down.
		Timer releaseTimer;
		while(_input->GetPortMask(port) != 0 && releaseTimer.GetElapsedMS() < 2000) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		result["released"] = _input->GetPortMask(port) == 0;
	}
	return json{ {"result", result} };
}

json EmuSession::ReleaseController(const json& args)
{
	uint32_t port = args.value("port", 1);
	_input->ReleasePort(port);
	return json{ {"result", json{ {"port", port}, {"released", true} }} };
}

json EmuSession::GetControllerState(const json& args)
{
	if(!IsRomLoaded()) {
		return ErrorResult("no ROM is loaded");
	}

	uint32_t port = args.value("port", 1);
	if(port < 1 || port > VirtualInputProvider::MaxPort) {
		return ErrorResult("port must be 1 to " + std::to_string(VirtualInputProvider::MaxPort));
	}

	//Attach before reporting, so this reflects what the game will actually poll
	_input->SyncRegistration();

	json result;
	result["port"] = port;
	result["provider_attached"] = _input->IsRegistered();
	result["overriding"] = _input->IsPortActive(port);
	result["buttons_held"] = _input->GetPortMask(port);

	shared_ptr<IConsole> console = _emu->GetConsole();
	shared_ptr<BaseControlDevice> device = (console && console->GetControlManager())
		? console->GetControlManager()->GetControlDevice((uint8_t)(port - 1))
		: nullptr;
	if(!device) {
		result["connected"] = false;
		result["note"] = "no controller is plugged into port " + std::to_string(port);
		return json{ {"result", result} };
	}

	result["connected"] = true;
	result["controller_type"] = EnumToSnake(magic_enum::enum_name(device->GetControllerType()).data());

	//State as of the console's last input poll - i.e. exactly what the game saw
	json buttons = json::object();
	json pressed = json::array();
	for(const DeviceButtonName& button : device->GetKeyNameAssociations()) {
		if(button.IsNumeric) {
			if(button.ButtonId == BaseControlDevice::DeviceXCoordButtonId) {
				buttons[button.Name] = device->GetCoordinates().X;
			} else if(button.ButtonId == BaseControlDevice::DeviceYCoordButtonId) {
				buttons[button.Name] = device->GetCoordinates().Y;
			}
		} else if(device->IsPressed((uint8_t)button.ButtonId)) {
			buttons[button.Name] = true;
			pressed.push_back(button.Name);
		} else {
			buttons[button.Name] = false;
		}
	}
	result["buttons"] = buttons;
	result["pressed"] = pressed;
	return json{ {"result", result} };
}

//--- Savestates ---------------------------------------------------------------

json EmuSession::SaveState(const json& args)
{
	if(!IsRomLoaded()) {
		return ErrorResult("no ROM is loaded");
	}

	std::string path = args.value("path", "");
	if(path.empty()) {
		std::string dir = FolderUtilities::CombinePath(_homeFolder, "savestates");
		std::error_code ec;
		fs::create_directories(dir, ec);
		path = FolderUtilities::CombinePath(dir, "state-" + std::to_string(++_saveStateCounter) + ".mss");
	}

	if(!_emu->GetSaveStateManager()->SaveState(path, false)) {
		return ErrorResult("failed to write save state to " + path);
	}
	return json{ {"result", json{ {"path", path}, {"frame_count", _emu->GetFrameCount()} }} };
}

json EmuSession::LoadState(const json& args)
{
	if(!IsRomLoaded()) {
		return ErrorResult("no ROM is loaded");
	}
	if(!args.contains("path") || !args["path"].is_string()) {
		return ErrorResult("missing required string argument: path");
	}
	std::string path = args["path"].get<std::string>();
	if(!fs::is_regular_file(path)) {
		return ErrorResult("save state file not found: " + path);
	}
	if(!_emu->GetSaveStateManager()->LoadState(path, false)) {
		return ErrorResult("failed to load save state (wrong ROM or incompatible state file?)");
	}
	return json{ {"result", json{ {"path", path}, {"frame_count", _emu->GetFrameCount()} }} };
}

//--- Lua scripting ------------------------------------------------------------

json EmuSession::RunLuaScript(const json& args)
{
	std::string error;
	Debugger* dbg = EnsureDebugger(args, error);
	if(!dbg) { return ErrorResult(error); }

	if(!args.contains("code") || !args["code"].is_string()) {
		return ErrorResult("missing required string argument: code (Lua source)");
	}
	std::string code = args["code"].get<std::string>();

	int32_t scriptId = dbg->GetScriptManager()->LoadScript("mcp-script", "", code, -1);
	if(scriptId < 0) {
		return ErrorResult("failed to load the Lua script (syntax error?) - check get_lua_script_log");
	}

	//Optionally advance frames while the script's frame callbacks run
	if(args.value("run_frames", 0) > 0) {
		json run = RunFrames(json{ {"frames", args["run_frames"]}, {"timeout_ms", args.value("timeout_ms", 10000)} });
		if(run.contains("error")) {
			return run;
		}
	}

	json result;
	result["script_id"] = scriptId;
	result["log"] = dbg->GetScriptManager()->GetScriptLog(scriptId);
	if(args.value("auto_stop", true)) {
		dbg->GetScriptManager()->RemoveScript(scriptId);
		result["stopped"] = true;
	}
	return json{ {"result", result} };
}

json EmuSession::GetLuaScriptLog(const json& args)
{
	std::string error;
	Debugger* dbg = EnsureDebugger(args, error);
	if(!dbg) { return ErrorResult(error); }
	if(!args.contains("script_id")) {
		return ErrorResult("missing required argument: script_id");
	}
	int32_t scriptId = args["script_id"].get<int32_t>();
	return json{ {"result", json{ {"script_id", scriptId}, {"log", dbg->GetScriptManager()->GetScriptLog(scriptId)} }} };
}

json EmuSession::StopLuaScript(const json& args)
{
	std::string error;
	Debugger* dbg = EnsureDebugger(args, error);
	if(!dbg) { return ErrorResult(error); }
	if(!args.contains("script_id")) {
		return ErrorResult("missing required argument: script_id");
	}
	int32_t scriptId = args["script_id"].get<int32_t>();
	dbg->GetScriptManager()->RemoveScript(scriptId);
	return json{ {"result", json{ {"script_id", scriptId}, {"stopped", true} }} };
}

//--- Code/data logger (coverage) ----------------------------------------------

json EmuSession::GetCdlStats(const json& args)
{
	std::string error;
	Debugger* dbg = EnsureDebugger(args, error);
	if(!dbg) { return ErrorResult(error); }

	MemoryType memType = MemoryType::NesPrgRom;
	if(args.contains("memory_type")) {
		if(!ResolveMemoryType(args["memory_type"].get<std::string>(), memType, error)) {
			return ErrorResult(error);
		}
	} else {
		//Default: the main program ROM of the loaded console
		switch(_emu->GetConsoleType()) {
			case ConsoleType::Nes: memType = MemoryType::NesPrgRom; break;
			case ConsoleType::Snes: memType = MemoryType::SnesPrgRom; break;
			case ConsoleType::Gameboy: memType = MemoryType::GbPrgRom; break;
			case ConsoleType::Gba: memType = MemoryType::GbaPrgRom; break;
			case ConsoleType::PcEngine: memType = MemoryType::PcePrgRom; break;
			case ConsoleType::Sms: memType = MemoryType::SmsPrgRom; break;
			case ConsoleType::Ws: memType = MemoryType::WsPrgRom; break;
			default: memType = MemoryType::NesPrgRom; break;
		}
	}

	CdlManager* cdl = dbg->GetCdlManager();
	CdlStatistics stats = cdl->GetCdlStatistics(memType);

	uint32_t size = dbg->GetMemoryDumper()->GetMemorySize(memType);
	json result;
	result["memory_type"] = args.value("memory_type", "prg_rom");
	result["size"] = size;
	result["code_bytes"] = stats.CodeBytes;
	result["data_bytes"] = stats.DataBytes;
	result["function_count"] = stats.FunctionCount;
	if(stats.TotalBytes > 0) {
		result["coverage_pct"] = (double)(stats.CodeBytes + stats.DataBytes) * 100.0 / stats.TotalBytes;
		result["code_pct"] = (double)stats.CodeBytes * 100.0 / stats.TotalBytes;
		result["total_bytes"] = stats.TotalBytes;
	}
	return json{ {"result", result} };
}

//--- Recorded ROM tests (.mntest) ---------------------------------------------

json EmuSession::RunRomTest(const json& args)
{
	if(!args.contains("path") || !args["path"].is_string()) {
		return ErrorResult("missing required string argument: path (.mntest file)");
	}
	std::string path = args["path"].get<std::string>();
	if(!fs::is_regular_file(path)) {
		return ErrorResult("test file not found: " + path);
	}

	//Run in a dedicated background emulator (the pattern proven by Mesen's CI):
	//the current session (ROM, breakpoints, memory edits) is untouched.
	std::unique_ptr<Emulator> testEmu(new Emulator());
	testEmu->Initialize(false);
	testEmu->GetSettings()->SetFlag(EmulationFlags::TestMode);
	std::shared_ptr<RecordedRomTest> romTest(new RecordedRomTest(testEmu.get(), true));
	RomTestResult result = romTest->Run(path);
	testEmu->Release();

	json out;
	json& r = out["result"];
	r["state"] = RomTestStateToString(result.State);
	r["mismatched_frames"] = result.ErrorCode; //bad frame count (0 = all frames matched)
	r["last_frame_hash"] = std::string(result.LastFrameHash, 40);
	return out;
}

json EmuSession::RecordRomTest(const json& args)
{
	if(!IsRomLoaded()) {
		return ErrorResult("no ROM is loaded");
	}
	if(_romTestRecorder) {
		return ErrorResult("a ROM test recording is already in progress - stop it first");
	}
	if(!args.contains("path") || !args["path"].is_string()) {
		return ErrorResult("missing required string argument: path (.mntest file to create)");
	}
	std::string path = args["path"].get<std::string>();

	_romTestRecorder.reset(new RecordedRomTest(_emu.get(), false));
	_romTestRecorder->Record(path, args.value("reset", false));
	return json{ {"result", json{
		{"path", path},
		{"note", "recording - drive the game with set_controller/run_frames, then call stop_rom_test_record"}
	}} };
}

json EmuSession::StopRomTestRecord()
{
	if(!_romTestRecorder) {
		return ErrorResult("no ROM test recording is in progress");
	}
	_romTestRecorder->Stop();
	_romTestRecorder.reset();
	return json{ {"result", "recording stopped"} };
}

//--- Cheats -------------------------------------------------------------------

json EmuSession::SetCheats(const json& args)
{
	if(!IsRomLoaded()) {
		return ErrorResult("no ROM is loaded");
	}

	std::vector<CheatCode> codes;
	if(args.contains("codes")) {
		if(!args["codes"].is_array()) {
			return ErrorResult("codes must be an array of cheat code strings");
		}
		ConsoleType console = _emu->GetConsoleType();
		for(const json& code : args["codes"]) {
			if(!code.is_string()) {
				return ErrorResult("codes must be an array of cheat code strings");
			}
			std::string str = code.get<std::string>();
			//Strip separators
			std::string clean;
			for(char c : str) {
				if(c != '-' && c != ' ') { clean += c; }
			}
			if(clean.empty()) {
				return ErrorResult("empty cheat code in list");
			}

			CheatCode cheat = {};
			cheat.Type = GuessCheatType(clean, console);
			strncpy(cheat.Code, clean.c_str(), sizeof(cheat.Code) - 1);
			codes.push_back(cheat);
		}
	}

	//SetCheats with an empty list clears all cheats
	_emu->GetCheatManager()->SetCheats(codes);

	return json{ {"result", json{ {"active_cheats", codes.size()} }} };
}

//--- Video capture ------------------------------------------------------------

json EmuSession::CaptureGif(const json& args)
{
	if(!IsRomLoaded()) {
		return ErrorResult("no ROM is loaded");
	}
	uint32_t frames = args.value("frames", 60);
	if(frames == 0 || frames > 3600) {
		return ErrorResult("frames must be between 1 and 3600");
	}

	std::string path = args.value("path", "");
	if(path.empty()) {
		std::string dir = FolderUtilities::CombinePath(_homeFolder, "captures");
		std::error_code ec;
		fs::create_directories(dir, ec);
		path = FolderUtilities::CombinePath(dir, "capture-" + std::to_string(::time(nullptr)) + ".gif");
	}

	RecordAviOptions options = {};
	options.Codec = VideoCodec::GIF;
	options.CompressionLevel = 0;
	options.RecordSystemHud = false;
	options.RecordInputHud = false;
	_emu->GetVideoRenderer()->StartRecording(path, options);

	RunFrames(json{ {"frames", frames}, {"timeout_ms", args.value("timeout_ms", 30000)} });

	_emu->GetVideoRenderer()->StopRecording();

	std::error_code ec;
	uintmax_t size = fs::file_size(path, ec);
	if(ec || size == 0) {
		return ErrorResult("GIF capture failed (no frames written)");
	}

	json result;
	result["path"] = path;
	result["bytes"] = (uint64_t)size;
	if(args.value("return_base64", false) && size <= 8 * 1024 * 1024) {
		std::ifstream file(path, std::ios::binary);
		std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		result["image_base64"] = Base64::Encode(std::vector<uint8_t>(data.begin(), data.end()));
	}
	return json{ {"result", result} };
}
