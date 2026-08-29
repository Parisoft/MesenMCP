//MesenMCP - emulator session layer
#include "Mcp/EmuSession.h"
#include "Core/Shared/DebuggerRequest.h"
#include "Mcp/WavCaptureDevice.h"
#include "Core/Shared/Audio/SoundMixer.h"

#include "Core/Shared/EmuSettings.h"
#include "Core/Shared/MessageManager.h"
#include "Core/Shared/SettingTypes.h"
#include "Core/Shared/RomInfo.h"
#include "Core/Shared/Video/VideoDecoder.h"
#include "Core/NES/NesDefaultVideoFilter.h"
#include "Utilities/ArchiveReader.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/PNGHelper.h"
#include "Utilities/Base64.h"
#include "Utilities/HexUtilities.h"
#include "Utilities/Timer.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

namespace fs = std::filesystem;

namespace
{
	std::string ConsoleTypeToString(ConsoleType type)
	{
		switch(type) {
			case ConsoleType::Nes: return "nes";
			case ConsoleType::Snes: return "snes";
			case ConsoleType::Gameboy: return "gameboy";
			case ConsoleType::Gba: return "gba";
			case ConsoleType::PcEngine: return "pcengine";
			case ConsoleType::Sms: return "sms";
			case ConsoleType::Ws: return "ws";
			default: return "unknown";
		}
	}

	std::string RegionToString(ConsoleRegion region)
	{
		switch(region) {
			case ConsoleRegion::Ntsc: return "ntsc";
			case ConsoleRegion::Pal: return "pal";
			case ConsoleRegion::Dendy: return "dendy";
			case ConsoleRegion::NtscJapan: return "ntsc-j";
			case ConsoleRegion::Auto: return "auto";
			default: return "unknown";
		}
	}

	std::string FormatToString(RomFormat format)
	{
		switch(format) {
			case RomFormat::iNes: return "ines";
			case RomFormat::Unif: return "unif";
			case RomFormat::Fds: return "fds";
			case RomFormat::Nsf: return "nsf";
			case RomFormat::Sfc: return "sfc";
			case RomFormat::Spc: return "spc";
			case RomFormat::Gb: return "gb";
			case RomFormat::Gbs: return "gbs";
			default: return "unknown";
		}
	}

	bool ParseRegion(const std::string& value, ConsoleRegion& region)
	{
		if(value == "auto") { region = ConsoleRegion::Auto; }
		else if(value == "ntsc") { region = ConsoleRegion::Ntsc; }
		else if(value == "pal") { region = ConsoleRegion::Pal; }
		else if(value == "dendy") { region = ConsoleRegion::Dendy; }
		else { return false; }
		return true;
	}

	json ErrorResult(const std::string& message)
	{
		return json{ {"error", message} };
	}
}

EmuSession::EmuSession(const std::string& homeFolder, bool verboseLog)
	: _homeFolder(homeFolder), _verboseLog(verboseLog)
{
	//stdout must stay protocol-clean in MCP mode; the P0 CLI mode enables the
	//core's stdout logging (verboseLog) because there is no protocol there.
	MessageManager::SetOptions(false, verboseLog);

	std::error_code ec;
	fs::create_directories(homeFolder, ec);
	FolderUtilities::SetHomeFolder(homeFolder);

	_emu.reset(new Emulator());
	_emu->Initialize(false);
	_emu->GetSettings()->SetFlag(EmulationFlags::TestMode);

	_renderer.reset(new HeadlessRenderer(_emu.get()));

	//Virtual controller input (MCP set_controller tool) - same mechanism movies use
	_input.reset(new VirtualInputProvider(_emu.get()));

	//Audio capture: keeps recent audio in a ring buffer (get_audio_summary) - the
	//same registration the old SdlSoundManager used
	_audio.reset(new WavCaptureDevice());
	_emu->GetSoundMixer()->RegisterAudioDevice(_audio.get());

	//Notification bridge: forwards breakpoint hits etc. from the emulation thread
	//to tool calls (WaitForBreak). The NotificationManager holds a shared_ptr to
	//the bridge, keeping it alive as long as the emulator exists.
	_bridge = std::make_shared<NotificationBridge>();
	_emu->GetNotificationManager()->RegisterNotificationListener(_bridge);
}

EmuSession::~EmuSession()
{
	if(_emu) {
		_renderer.reset();
		_emu->GetSoundMixer()->RegisterAudioDevice(nullptr);
		_audio.reset();
		_emu->Stop(false);
		_emu->Release();
	}
}

void EmuSession::ApplyDeterministicSettings(bool deterministic)
{
	//Mirrors RecordedRomTest::UpdateSettings() from Mesen's test runner, with the
	//power-on RAM state configurable (random RAM is a legit thing to test against).
	EmuSettings* settings = _emu->GetSettings();
	EmulationConfig& emuCfg = settings->GetEmulationConfig();
	emuCfg.RunAheadFrames = 0; //run-ahead runs a hidden 2nd instance and drops frames

	RamState ramState = deterministic ? RamState::AllZeros : RamState::Random;
	settings->GetNesConfig().RamPowerOnState = ramState;
	settings->GetSnesConfig().RamPowerOnState = ramState;
	settings->GetGameboyConfig().RamPowerOnState = ramState;
	settings->GetPcEngineConfig().RamPowerOnState = ramState;
	settings->GetSmsConfig().RamPowerOnState = ramState;
	settings->GetGbaConfig().RamPowerOnState = ramState;

	settings->GetSnesConfig().DisableFrameSkipping = true;
	settings->GetPcEngineConfig().DisableFrameSkipping = true;
	settings->GetGbaConfig().DisableFrameSkipping = true;

	settings->GetNesConfig().RemoveSpriteLimit = false;
	settings->GetSnesConfig().RemoveSpriteLimit = false;
	settings->GetGameboyConfig().RemoveSpriteLimit = false;
	settings->GetPcEngineConfig().RemoveSpriteLimit = false;
	settings->GetSmsConfig().RemoveSpriteLimit = false;

	//The core expects the front-end to provide the NES palette (the old GUI pushed
	//it through its config file). Without this every decoded pixel is black.
	NesDefaultVideoFilter::GetBuiltInPalette(settings->GetNesConfig().UserPalette);
	settings->GetNesConfig().IsFullColorPalette = true;
}

void EmuSession::ApplySpeed(bool maximumSpeed)
{
	_maximumSpeed = maximumSpeed;
	if(maximumSpeed) {
		_emu->GetSettings()->SetFlag(EmulationFlags::MaximumSpeed);
	} else {
		_emu->GetSettings()->ClearFlag(EmulationFlags::MaximumSpeed);
	}
}

json EmuSession::LoadRom(const json& args)
{
	if(!args.contains("path") || !args["path"].is_string()) {
		return ErrorResult("missing required string argument: path");
	}
	std::string romPath = args["path"].get<std::string>();

	std::error_code ec;
	if(!fs::is_regular_file(romPath, ec)) {
		return ErrorResult("ROM file not found: " + romPath);
	}

	std::string patchPath = args.value("patch_path", "");
	if(!patchPath.empty() && !fs::is_regular_file(patchPath, ec)) {
		return ErrorResult("patch file not found: " + patchPath);
	}

	ConsoleRegion region = ConsoleRegion::Auto;
	if(args.contains("region")) {
		std::string regionStr = args["region"].get<std::string>();
		if(!ParseRegion(regionStr, region)) {
			return ErrorResult("invalid region '" + regionStr + "' (expected auto, ntsc, pal or dendy)");
		}
	}

	bool deterministic = args.value("deterministic", true);
	bool maximumSpeed;
	if(args.contains("speed")) {
		std::string speed = args["speed"].get<std::string>();
		if(speed != "max" && speed != "realtime") {
			return ErrorResult("invalid speed '" + speed + "' (expected max or realtime)");
		}
		maximumSpeed = (speed == "max");
	} else {
		maximumSpeed = true;
	}

	//Settings must be applied before the console is instantiated during LoadRom
	ApplyDeterministicSettings(deterministic);
	_emu->GetSettings()->GetNesConfig().Region = region;

	//Plug standard controllers into ports 1-2 - headless sessions start with NO
	//controllers configured (ControllerConfig defaults to ControllerType::None,
	//the GUI filled this in from its config file)
	{
		NesConfig& nes = _emu->GetSettings()->GetNesConfig();
		if(nes.Port1.Type == ControllerType::None) { nes.Port1.Type = ControllerType::NesController; }
		if(nes.Port2.Type == ControllerType::None) { nes.Port2.Type = ControllerType::NesController; }

		SnesConfig& snes = _emu->GetSettings()->GetSnesConfig();
		if(snes.Port1.Type == ControllerType::None) { snes.Port1.Type = ControllerType::SnesController; }
		if(snes.Port2.Type == ControllerType::None) { snes.Port2.Type = ControllerType::SnesController; }

		GameboyConfig& gb = _emu->GetSettings()->GetGameboyConfig();
		if(gb.Controller.Type == ControllerType::None) { gb.Controller.Type = ControllerType::GameboyController; }

		GbaConfig& gba = _emu->GetSettings()->GetGbaConfig();
		if(gba.Controller.Type == ControllerType::None) { gba.Controller.Type = ControllerType::GbaController; }
	}

	//Archive support: if the path is a zip/7z, load the first ROM file inside it
	string innerRom;
	{
		std::string romExt = fs::path(romPath).extension().string();
		for(char& ch : romExt) { ch = (char)::tolower(ch); }
		if(romExt == ".zip" || romExt == ".7z") {
			unique_ptr<ArchiveReader> reader = ArchiveReader::GetReader(romPath);
			if(reader) {
				static const std::vector<std::string> romExts = {
					".nes", ".fds", ".nsf", ".unif", ".sfc", ".smc", ".gb", ".gbc",
					".gba", ".pce", ".sgx", ".sms", ".gg", ".sg", ".ws", ".wsc"
				};
				for(string& file : reader->GetFileList()) {
					std::string ext = fs::path(file).extension().string();
					for(char& ch : ext) { ch = (char)::tolower(ch); }
					bool matched = false;
					for(const std::string& valid : romExts) {
						if(ext == valid) { matched = true; break; }
					}
					if(matched) {
						innerRom = file;
						break;
					}
				}
			}
			if(innerRom.empty()) {
				return ErrorResult("archive contains no ROM file: " + romPath);
			}
		}
	}

	bool ok;
	if(patchPath.empty()) {
		ok = innerRom.empty()
			? _emu->LoadRom((VirtualFile)romPath, VirtualFile())
			: _emu->LoadRom(VirtualFile(romPath, innerRom), VirtualFile());
	} else {
		ok = _emu->LoadRom((VirtualFile)romPath, (VirtualFile)patchPath);
	}

	if(!ok) {
		return ErrorResult(
			"failed to load ROM '" + romPath + "'. Common causes: unsupported mapper, "
			"corrupt/unrecognized file, or missing firmware (FDS needs disksys.rom, GBA needs "
			"gba_bios.bin in the firmware folder of the session home directory: " + _homeFolder + ")");
	}

	//Emulation flags are reset while a ROM loads - apply the speed AFTER loading.
	ApplySpeed(maximumSpeed);

	return BuildStatus();
}

json EmuSession::UnloadRom()
{
	if(!IsRomLoaded()) {
		return ErrorResult("no ROM is loaded");
	}
	_emu->Stop(false);
	return json{ {"result", "unloaded"} };
}

json EmuSession::Reset(const json& args)
{
	if(!IsRomLoaded()) {
		return ErrorResult("no ROM is loaded");
	}
	bool powerCycle = args.value("mode", "reset") == "power_cycle";

	//Known core deadlock: resetting while the console CPU is halted (e.g. a GBA
	//game idling in BIOS IntrWait) with the debugger active hangs Emulator::Lock
	//forever. Run the reset with a watchdog and fall back to stop+reload (which
	//is equivalent to a power cycle) when it stalls. The stuck reset thread
	//completes on its own once the emulator thread stops.
	std::atomic<bool> done = false;
	std::thread watchdog([&]() {
		if(powerCycle) {
			_emu->PowerCycle();
		} else {
			_emu->Reset();
		}
		done = true;
	});

	Timer timer;
	while(!done.load() && timer.GetElapsedMS() < 3000) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	if(done.load()) {
		watchdog.join();
		return json{ {"result", powerCycle ? "power cycled" : "reset"} };
	}

	watchdog.detach();
	string romPath = (string)_emu->GetRomInfo().RomFile;

	//Fallback: stop + reload. The stuck emu thread usually unwedges once it is
	//asked to stop - but guard this too, and report clearly if all is lost.
	std::atomic<bool> fallbackDone = false;
	std::thread fallback([&]() {
		_emu->Stop(false);
		if(_emu->LoadRom((VirtualFile)romPath, VirtualFile())) {
			ApplySpeed(_maximumSpeed);
		}
		fallbackDone = true;
	});
	Timer fallbackTimer;
	while(!fallbackDone.load() && fallbackTimer.GetElapsedMS() < 3000) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	if(fallbackDone.load()) {
		fallback.join();
		if(!IsRomLoaded()) {
			return ErrorResult("reset stalled (known CPU-halt + debugger deadlock) and the fallback reload failed");
		}
		return json{ {"result", "reset (fallback: stop + reload after the reset path stalled)"} };
	}
	fallback.detach();
	return ErrorResult("reset stalled (known core deadlock: console CPU halted/stopped while the "
		"debugger is active wedges the emulation thread). The session cannot recover from this - "
		"restart the server (a fresh load_rom in a new process works fine). Avoid reset while a "
		"GBA game idles in a halted state with the debugger running.");
}

json EmuSession::Pause()
{
	if(!IsRomLoaded()) {
		return ErrorResult("no ROM is loaded");
	}
	//With the debugger active, Pause() arms a break at the next instruction
	//boundary instead of setting the flag synchronously - wait briefly for it.
	uint64_t seq = _bridge->CurrentSequence();
	bool wasStopped = false;
	if(Debugger* dbg = _emu->GetDebugger(false).GetDebugger()) {
		wasStopped = dbg->IsExecutionStopped();
	}
	_emu->Pause();
	if(wasStopped) {
		//Already stopped by the debugger - the pause request is a no-op
		return BuildStatus();
	}
	Timer timer;
	while(timer.GetElapsedMS() < 2000) {
		if(_emu->IsPaused()) {
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	return BuildStatus();
}

json EmuSession::Resume()
{
	if(!IsRomLoaded()) {
		return ErrorResult("no ROM is loaded");
	}
	_emu->Resume();
	return BuildStatus();
}

json EmuSession::SetSpeed(const json& args)
{
	if(!args.contains("speed") || !args["speed"].is_string()) {
		return ErrorResult("missing required string argument: speed (max or realtime)");
	}
	std::string speed = args["speed"].get<std::string>();
	if(speed != "max" && speed != "realtime") {
		return ErrorResult("invalid speed '" + speed + "' (expected max or realtime)");
	}
	ApplySpeed(speed == "max");
	return BuildStatus();
}

json EmuSession::RunFrames(const json& args)
{
	if(!IsRomLoaded()) {
		return ErrorResult("no ROM is loaded");
	}

	uint32_t frames = args.value("frames", 60);
	uint32_t timeoutMs = args.value("timeout_ms", 10000);
	if(frames == 0 || frames > 10 * 60 * 60 * 60) {
		return ErrorResult("frames must be between 1 and 21600000");
	}
	if(_emu->IsPaused()) {
		return ErrorResult("emulation is paused - call resume first");
	}

	uint32_t startCount = _emu->GetFrameCount();
	uint32_t target = startCount + frames;

	Timer timer;
	while(_emu->GetFrameCount() < target) {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		if(timer.GetElapsedMS() > timeoutMs) {
			break;
		}
	}

	uint32_t endCount = _emu->GetFrameCount();
	return json{
		{"result", {
			{"frames_requested", frames},
			{"frames_run", endCount >= startCount ? endCount - startCount : 0},
			{"frame_count", endCount},
			{"elapsed_ms", (uint32_t)timer.GetElapsedMS()},
			{"timed_out", endCount < target}
		}}
	};
}

json EmuSession::Screenshot(const json& args)
{
	std::vector<uint32_t> pixels;
	uint32_t width = 0, height = 0, frameNumber = 0;
	if(!_renderer->GetLastFrame(pixels, width, height, frameNumber)) {
		return ErrorResult("no frame has been rendered yet - run some frames first (run_frames)");
	}

	std::stringstream pngStream;
	if(!PNGHelper::WritePNG(pngStream, pixels.data(), width, height, 24)) {
		return ErrorResult("failed to encode PNG");
	}
	std::string png = pngStream.str();
	std::string base64 = Base64::Encode(std::vector<uint8_t>(png.begin(), png.end()));

	json out;
	json& result = out["result"];
	result["image_base64"] = base64;
	result["width"] = width;
	result["height"] = height;
	result["frame_number"] = frameNumber;

	if(args.contains("save_path")) {
		std::string savePath = args["save_path"].get<std::string>();
		if(!PNGHelper::WritePNG(savePath, pixels.data(), width, height, 24)) {
			return ErrorResult("failed to write screenshot to " + savePath);
		}
		result["saved_to"] = savePath;
	}
	return out;
}

json EmuSession::BuildStatus()
{
	json status;
	status["rom_loaded"] = IsRomLoaded();
	status["paused"] = _emu->IsPaused();
	status["speed"] = _maximumSpeed ? "max" : "realtime";
	status["frame_count"] = _emu->GetFrameCount();
	status["fps"] = _emu->GetFps();
	status["emulator_version"] = _emu->GetSettings()->GetVersionString();

	if(IsRomLoaded()) {
		json& rom = status["rom"];
		rom["file"] = _emu->GetRomInfo().RomFile.GetFileName();
		rom["format"] = FormatToString(_emu->GetRomInfo().Format);
		rom["console_type"] = ConsoleTypeToString(_emu->GetConsoleType());
		rom["region"] = RegionToString(_emu->GetRegion());
		rom["sha1"] = _emu->GetHash(HashType::Sha1);
		rom["crc32"] = HexUtilities::ToHex(_emu->GetCrc32());
	}

	uint32_t width = 0, height = 0, frameNumber = 0;
	std::vector<uint32_t> pixels;
	if(_renderer->GetLastFrame(pixels, width, height, frameNumber)) {
		status["video"] = json{ {"width", width}, {"height", height}, {"frame_number", frameNumber} };
	}
	return json{ {"result", status} };
}

json EmuSession::GetStatus()
{
	return BuildStatus();
}

bool EmuSession::IsRomLoaded()
{
	//_console is reset when emulation stops (Emulator::Stop), so IsRunning()
	//doubles as "a ROM is loaded"
	return _emu->IsRunning();
}
