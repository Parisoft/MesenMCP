//MesenMCP - P0 headless proof of concept
//
//This front-end validates that the emulation core runs completely headless:
//no SDL, no X11, no window, no .NET. It loads a ROM, runs it at maximum speed
//for a number of frames, writes a PNG screenshot of the last frame and exits.
//
//The actual MCP server (stdio, JSON-RPC 2.0) replaces this CLI front-end in P1;
//the emulator lifecycle code below (init flags, frame polling, shutdown order)
//is the foundation the MCP session layer will be built on.
//
//Usage:
//  mesen-mcp --rom <file> [--frames N] [--screenshot out.png] [--home dir] [--timeout secs]
#include "Core/Shared/Emulator.h"
#include "Core/Shared/EmuSettings.h"
#include "Mcp/HeadlessRenderer.h"
#include "Core/Shared/Video/VideoDecoder.h"
#include "Core/NES/NesDefaultVideoFilter.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/PNGHelper.h"
#include "Utilities/Timer.h"
#include "Core/Shared/MessageManager.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace fs = std::filesystem;

static void PrintUsage()
{
	std::fprintf(stderr,
		"usage: mesen-mcp --rom <file> [--frames N] [--screenshot out.png] [--home dir] [--timeout secs] [--verbose]\n"
		"\n"
		"  --rom <file>        ROM to load (required)\n"
		"  --frames N          number of frames to run before exiting (default: 300)\n"
		"  --screenshot <file> write a PNG of the final frame\n"
		"  --home <dir>        home folder for emulator state (default: temp dir)\n"
		"  --timeout secs      max seconds to wait for the frame count (default: 60)\n"
		"  --verbose           print the emulator's log to stdout\n");
}

static int Run(const std::string& romPath, uint32_t targetFrames, const std::string& screenshotPath, const fs::path& homeFolder, uint32_t timeoutSecs, bool verbose)
{
	if(verbose) {
		//Route the emulator's internal log to stdout (goes to stderr/logging in the MCP server)
		MessageManager::SetOptions(false, true);
	}

	//Isolate all emulator state (settings, battery RAM, save states, logs) from the
	//user's real Mesen folders - each process gets its own home folder.
	std::error_code ec;
	fs::create_directories(homeFolder, ec);
	FolderUtilities::SetHomeFolder(homeFolder.string());

	Timer timer;

	//Emulator lifecycle mirrors the headless path proven by Mesen's CI test runner
	//(InteropDLL/TestApiWrapper.cpp: RunRecordedTest/RunTest).
	Emulator emu;
	emu.Initialize(false);

	//TestMode: suppress interactive side effects (battery prompts, etc.)
	emu.GetSettings()->SetFlag(EmulationFlags::TestMode);

	//Deterministic settings - mirrors RecordedRomTest::UpdateSettings() from Mesen's
	//own CI test runner. Critical bits:
	// - RamPowerOnState defaults to RANDOM on power-on (a deliberate Mesen feature to
	//   catch homebrew bugs) - real ROMs can crash into random RAM content without this.
	// - RunAheadFrames defaults to 1, which runs a second hidden emulation instance and
	//   drops/duplicates frames sent to rendering devices.
	// - Frame skipping / sprite limit removal are non-deterministic by default.
	{
		EmuSettings* settings = emu.GetSettings();
		settings->GetEmulationConfig().RunAheadFrames = 0;
		settings->GetNesConfig().RamPowerOnState = RamState::AllZeros;
		settings->GetSnesConfig().RamPowerOnState = RamState::AllZeros;
		settings->GetGameboyConfig().RamPowerOnState = RamState::AllZeros;
		settings->GetPcEngineConfig().RamPowerOnState = RamState::AllZeros;
		settings->GetSmsConfig().RamPowerOnState = RamState::AllZeros;
		settings->GetGbaConfig().RamPowerOnState = RamState::AllZeros;
		settings->GetSnesConfig().DisableFrameSkipping = true;
		settings->GetPcEngineConfig().DisableFrameSkipping = true;
		settings->GetGbaConfig().DisableFrameSkipping = true;
		settings->GetNesConfig().RemoveSpriteLimit = false;
		settings->GetSnesConfig().RemoveSpriteLimit = false;
		settings->GetGameboyConfig().RemoveSpriteLimit = false;
		settings->GetPcEngineConfig().RemoveSpriteLimit = false;
		settings->GetSmsConfig().RemoveSpriteLimit = false;

		//The core expects the NES palette to be provided by the front-end (the old GUI
		//pushed it into the config on startup) - without this every pixel decodes black.
		NesDefaultVideoFilter::GetBuiltInPalette(settings->GetNesConfig().UserPalette);
		settings->GetNesConfig().IsFullColorPalette = true;
	}

	HeadlessRenderer renderer(&emu);

	if(!emu.LoadRom((VirtualFile)romPath, VirtualFile())) {
		std::fprintf(stderr, "error: failed to load '%s'\n", romPath.c_str());
		return 2;
	}

	//MaximumSpeed disables the frame limiter so frames run as fast as possible.
	//Set after LoadRom - loading a ROM re-applies settings and resets emulation flags.
	emu.GetSettings()->SetFlag(EmulationFlags::MaximumSpeed);

	std::fprintf(stderr, "[p0] loaded: %s (console type: %d)\n", romPath.c_str(), (int)emu.GetConsoleType());

	//Run the requested number of frames as fast as the CPU allows.
	while(emu.GetFrameCount() < targetFrames) {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		if(timer.GetElapsedMS() > timeoutSecs * 1000.0) {
			std::fprintf(stderr, "error: timed out after %.1fs (frame count: %u)\n", timer.GetElapsedMS() / 1000.0, emu.GetFrameCount());
			emu.Stop(false);
			emu.Release();
			return 3;
		}
	}

	double elapsed = timer.GetElapsedMS() / 1000.0;
	std::fprintf(stderr, "[p0] ran %u frames in %.2fs (~%.0f fps)\n", emu.GetFrameCount(), elapsed, targetFrames / elapsed);

	if(verbose) {
		std::fprintf(stderr, "[p0] renderer: %u frames received, decoder frames: %u\n",
			renderer._updateFrameCount.load(), emu.GetVideoDecoder()->GetFrameCount());
	}


	int result = 0;
	if(!screenshotPath.empty()) {
		std::vector<uint32_t> pixels;
		uint32_t width = 0, height = 0, frameNumber = 0;
		if(renderer.GetLastFrame(pixels, width, height, frameNumber)) {
			bool written = PNGHelper::WritePNG(screenshotPath, pixels.data(), width, height, 24);
			if(written) {
				uintmax_t size = fs::file_size(screenshotPath, ec);
				std::fprintf(stderr, "[p0] screenshot: %ux%u (frame %u) -> %s (%llu bytes)\n",
					width, height, frameNumber, screenshotPath.c_str(), (unsigned long long)size);
			} else {
				std::fprintf(stderr, "error: failed to write screenshot '%s'\n", screenshotPath.c_str());
				result = 4;
			}
		} else {
			std::fprintf(stderr, "error: no frame was rendered, cannot take a screenshot\n");
			result = 4;
		}
	}

	//Shutdown order: stop emulation (joins the emulation thread), then release
	//(stops the video decoder/renderer threads), then let ~HeadlessRenderer
	//unregister from the (now idle) video renderer.
	emu.Stop(false);
	emu.Release();
	return result;
}

int main(int argc, char** argv)
{
	std::string romPath;
	std::string screenshotPath;
	fs::path homeFolder;
	uint32_t targetFrames = 300;
	uint32_t timeoutSecs = 60;
	bool verbose = false;

	for(int i = 1; i < argc; i++) {
		std::string arg = argv[i];
		auto getValue = [&](const char* name) -> std::string {
			if(i + 1 >= argc) {
				std::fprintf(stderr, "error: missing value for %s\n", name);
				PrintUsage();
				std::exit(1);
			}
			return argv[++i];
		};

		if(arg == "--rom") {
			romPath = getValue("--rom");
		} else if(arg == "--frames") {
			targetFrames = (uint32_t)std::stoul(getValue("--frames"));
		} else if(arg == "--screenshot") {
			screenshotPath = getValue("--screenshot");
		} else if(arg == "--home") {
			homeFolder = getValue("--home");
		} else if(arg == "--timeout") {
			timeoutSecs = (uint32_t)std::stoul(getValue("--timeout"));
		} else if(arg == "--verbose" || arg == "-v") {
			verbose = true;
		} else if(arg == "--help" || arg == "-h") {
			PrintUsage();
			return 0;
		} else {
			std::fprintf(stderr, "error: unknown argument '%s'\n", arg.c_str());
			PrintUsage();
			return 1;
		}
	}

	if(romPath.empty()) {
		PrintUsage();
		return 1;
	}

	if(homeFolder.empty()) {
		homeFolder = fs::temp_directory_path() / ("mesen-mcp-" + std::to_string((uint64_t)::getpid()));
	}

	return Run(romPath, targetFrames, screenshotPath, homeFolder, timeoutSecs, verbose);
}
