//MesenMCP - headless MCP server for the Mesen emulation core
//
//Default mode: MCP server over stdio (newline-delimited JSON-RPC 2.0), one ROM
//per process. Spawned by an MCP client; type "mesen-mcp --help" for options.
//
//The P0 proof-of-concept CLI (--rom ...) is kept as a second mode for quick
//manual testing: it loads a ROM, runs N frames at maximum speed, writes a PNG
//screenshot and exits.
#include "Mcp/EmuSession.h"
#include "Mcp/McpServer.h"

#include <cstdio>
#include <cstdlib>
#include <execinfo.h>
#include <filesystem>
#include <iostream>
#include <string>
#include <signal.h>

#include <unistd.h>

static void CrashHandler(int sig)
{
	void* frames[32];
	int n = backtrace(frames, 32);
	fprintf(stderr, "\n=== FATAL signal %d, backtrace (%d frames) ===\n", sig, n);
	backtrace_symbols_fd(frames, n, 2);
	_exit(128 + sig);
}

namespace fs = std::filesystem;

static void PrintUsage()
{
	std::fprintf(stderr,
		"mesen-mcp - headless MCP server for the Mesen emulation core\n"
		"\n"
		"usage: mesen-mcp [--home <dir>] [--verbose]\n"
		"       mesen-mcp --rom <file> [--frames N] [--screenshot out.png] [--timeout secs]\n"
		"\n"
		"With --rom, runs the P0 proof-of-concept CLI (load, run N frames, screenshot,\n"
		"exit). Without it, starts the MCP server on stdio.\n"
		"\n"
		"  --home <dir>     home folder for emulator state (settings, battery RAM, logs,\n"
		"                   firmware files such as gba_bios.bin). Default: temp dir per process\n"
		"  --verbose        mirror the emulator's internal log to stderr\n");
}

static int RunPocCli(const std::string& romPath, uint32_t targetFrames, const std::string& screenshotPath,
	const fs::path& homeFolder, uint32_t timeoutSecs)
{
	EmuSession session(homeFolder.string(), /*verboseLog=*/true);

	json loadResult = session.LoadRom(json{ {"path", romPath} });
	if(loadResult.contains("error")) {
		std::fprintf(stderr, "error: %s\n", loadResult["error"].get<std::string>().c_str());
		return 2;
	}

	json runResult = session.RunFrames(json{ {"frames", targetFrames}, {"timeout_ms", timeoutSecs * 1000} });
	if(runResult.contains("error")) {
		std::fprintf(stderr, "error: %s\n", runResult["error"].get<std::string>().c_str());
		return 3;
	}

	json run = runResult["result"];
	std::fprintf(stderr, "[p0] ran %u frames in %.2fs\n",
		run["frames_run"].get<uint32_t>(), run["elapsed_ms"].get<uint32_t>() / 1000.0);

	if(run["timed_out"].get<bool>()) {
		std::fprintf(stderr, "error: timed out before reaching the target frame count\n");
		return 3;
	}

	int result = 0;
	if(!screenshotPath.empty()) {
		json shot = session.Screenshot(json{ {"save_path", screenshotPath} });
		if(shot.contains("error")) {
			std::fprintf(stderr, "error: %s\n", shot["error"].get<std::string>().c_str());
			result = 4;
		} else {
			std::fprintf(stderr, "[p0] screenshot: %ux%u -> %s\n",
				shot["result"]["width"].get<uint32_t>(),
				shot["result"]["height"].get<uint32_t>(),
				screenshotPath.c_str());
		}
	}

	std::fprintf(stderr, "[p0] done\n");
	return result;
}

int main(int argc, char** argv)
{
	//Never die on SIGPIPE (broken stdout when the client disconnects); the stdio
	//loop notices the failed/EOF stream and exits cleanly.
	::signal(SIGPIPE, SIG_IGN);
#if !defined(__SANITIZE_ADDRESS__)
	::signal(SIGSEGV, CrashHandler);
	::signal(SIGABRT, CrashHandler);
#endif

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

	if(homeFolder.empty()) {
		homeFolder = fs::temp_directory_path() / ("mesen-mcp-" + std::to_string((uint64_t)::getpid()));
	}

	if(!romPath.empty()) {
		return RunPocCli(romPath, targetFrames, screenshotPath, homeFolder, timeoutSecs);
	}

	//MCP stdio server mode. NOTE: core logging to stdout stays disabled here even
	//with --verbose (stdout is the transport); logs go to the session home folder.
	{
		EmuSession session(homeFolder.string(), /*verboseLog=*/false);
		McpServer server(session);
		return server.Run(std::cin, std::cout);
	}
}
