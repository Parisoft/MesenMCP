//MesenMCP - PPU inspection, audio & trace-file tools (P4: WP4 a/b/e)
//
//The PPU tools are thin wrappers over the core's PpuTools - the same code the
//GUI viewers used - so they work for every console the core supports. State is
//passed through layout-agnostic buffers: the core fills its own per-console
//structs into the buffer and reads them back, so no console-specific types are
//needed here.
#include "Mcp/EmuSession.h"

#include "Core/Shared/DebuggerRequest.h"
#include "Core/Debugger/DebugTypes.h"
#include "Core/Debugger/PpuTools.h"
#include "Core/Debugger/TraceLogFileSaver.h"
#include "Core/Debugger/ITraceLogger.h"
#include "Core/Debugger/MemoryDumper.h"
#include "Core/Shared/Audio/SoundMixer.h"
#include "Mcp/WavCaptureDevice.h"
#include "Utilities/PNGHelper.h"
#include "Utilities/Base64.h"
#include "Utilities/FolderUtilities.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace
{
	json ErrorResult(const std::string& message)
	{
		return json{ {"error", message} };
	}

	//Console-specific PPU memory regions used by the viewers
	MemoryType GetVramMemoryType(ConsoleType console)
	{
		switch(console) {
			case ConsoleType::Nes: return MemoryType::NesPpuMemory;
			case ConsoleType::Snes: return MemoryType::SnesVideoRam;
			case ConsoleType::Gameboy: return MemoryType::GbVideoRam;
			case ConsoleType::Gba: return MemoryType::GbaVideoRam;
			case ConsoleType::PcEngine: return MemoryType::PceVideoRam;
			case ConsoleType::Sms: return MemoryType::SmsVideoRam;
			default: return MemoryType::None;
		}
	}

	MemoryType GetOamMemoryType(ConsoleType console)
	{
		switch(console) {
			case ConsoleType::Nes: return MemoryType::NesSpriteRam;
			case ConsoleType::Snes: return MemoryType::SnesSpriteRam;
			case ConsoleType::Gameboy: return MemoryType::GbSpriteRam;
			case ConsoleType::Gba: return MemoryType::GbaSpriteRam;
			case ConsoleType::PcEngine: return MemoryType::PceSpriteRam;
			case ConsoleType::Sms: return MemoryType::None;
			default: return MemoryType::None;
		}
	}

	//Tileset source + format for get_tiles
	bool GetTilesetConfig(ConsoleType console, MemoryDumper* dumper, MemoryType& memType, TileFormat& format, std::string& error)
	{
		auto sizeOf = [&](MemoryType t) { return dumper->GetMemorySize(t); };
		switch(console) {
			case ConsoleType::Nes:
				memType = sizeOf(MemoryType::NesChrRom) > 0 ? MemoryType::NesChrRom : MemoryType::NesChrRam;
				format = TileFormat::NesBpp2;
				return true;
			case ConsoleType::Snes:
				memType = MemoryType::SnesVideoRam;
				format = TileFormat::Bpp4;
				return true;
			case ConsoleType::Gameboy:
				memType = MemoryType::GbVideoRam;
				format = TileFormat::Bpp2;
				return true;
			case ConsoleType::Gba:
				memType = MemoryType::GbaVideoRam;
				format = TileFormat::GbaBpp4;
				return true;
			case ConsoleType::PcEngine:
				memType = MemoryType::PceVideoRam;
				format = TileFormat::Bpp4;
				return true;
			case ConsoleType::Sms:
				memType = MemoryType::SmsVideoRam;
				format = TileFormat::SmsBpp4;
				return true;
			default:
				error = "tile viewer is not supported for this console";
				return false;
		}
	}

	std::string MirroringToString(TilemapMirroring mirroring)
	{
		switch(mirroring) {
			case TilemapMirroring::Horizontal: return "horizontal";
			case TilemapMirroring::Vertical: return "vertical";
			case TilemapMirroring::SingleScreenA: return "single_screen_a";
			case TilemapMirroring::SingleScreenB: return "single_screen_b";
			case TilemapMirroring::FourScreens: return "four_screens";
			default: return "none";
		}
	}

	std::string VisibilityToString(SpriteVisibility visibility)
	{
		switch(visibility) {
			case SpriteVisibility::Visible: return "visible";
			case SpriteVisibility::Offscreen: return "offscreen";
			case SpriteVisibility::Disabled: return "disabled";
			default: return "unknown";
		}
	}

	std::string ToRgbHex(uint32_t argb)
	{
		char buf[8];
		snprintf(buf, sizeof(buf), "%06X", argb & 0xFFFFFF);
		return std::string(buf);
	}

	//Encode an ARGB framebuffer as base64 PNG (same format as the screenshot tool)
	std::string EncodePngBase64(const std::vector<uint32_t>& pixels, uint32_t width, uint32_t height)
	{
		std::stringstream stream;
		if(!PNGHelper::WritePNG(stream, const_cast<uint32_t*>(pixels.data()), width, height, 24)) {
			return "";
		}
		std::string png = stream.str();
		return Base64::Encode(std::vector<uint8_t>(png.begin(), png.end()));
	}

	//Fill the per-console PPU state + PPU tools state into layout-agnostic buffers.
	//Buffer sizes matter: the core memcpy's its whole per-console struct into
	//these - e.g. NesPpuToolsState embeds ExtModeConfig with an ext-RAM copy and
	//is ~8.3KB by itself. 16KB covers all consoles with a wide margin.
	struct PpuStateBuffers
	{
		uint8_t ppuState[16384] = {};
		uint8_t ppuToolsState[16384] = {};
	};

	void FillPpuStates(Debugger* dbg, PpuTools* tools, CpuType cpu, PpuStateBuffers& buffers)
	{
		dbg->GetPpuState((BaseState&)buffers.ppuState, cpu);
		tools->GetPpuToolsState((BaseState&)buffers.ppuToolsState);
	}
}

//--- Palette ------------------------------------------------------------------

json EmuSession::GetPalette(const json& args)
{
	std::string error;
	Debugger* dbg = EnsureDebugger(args, error);
	if(!dbg) { return ErrorResult(error); }
	CpuType cpu = ResolveCpu(args, error);
	if(!error.empty()) { return ErrorResult(error); }

	PpuTools* tools = dbg->GetPpuTools(cpu);
	DebugPaletteInfo info = tools->GetPaletteInfo(GetPaletteInfoOptions {});

	json result;
	result["color_count"] = info.ColorCount;
	result["colors_per_palette"] = info.ColorsPerPalette;
	result["background_color_count"] = info.BgColorCount;
	result["sprite_color_count"] = info.SpriteColorCount;
	result["sprite_palette_offset"] = info.SpritePaletteOffset;

	json bg = json::array();
	json sprite = json::array();
	for(uint32_t i = 0; i < info.ColorCount; i++) {
		json entry{
			{"raw", info.RawPalette[i]},
			{"rgb", ToRgbHex(info.RgbPalette[i])}
		};
		if(i < info.SpritePaletteOffset || info.SpriteColorCount == 0) {
			bg.push_back(entry);
		} else {
			sprite.push_back(entry);
		}
	}
	result["background"] = bg;
	result["sprites"] = sprite;
	return json{ {"result", result} };
}

//--- Tilemap ------------------------------------------------------------------

json EmuSession::GetTilemap(const json& args)
{
	std::string error;
	Debugger* dbg = EnsureDebugger(args, error);
	if(!dbg) { return ErrorResult(error); }
	CpuType cpu = ResolveCpu(args, error);
	if(!error.empty()) { return ErrorResult(error); }

	PpuTools* tools = dbg->GetPpuTools(cpu);
	MemoryType vramType = GetVramMemoryType(_emu->GetConsoleType());
	MemoryDumper* dumper = dbg->GetMemoryDumper();
	uint32_t vramSize = dumper->GetMemorySize(vramType);
	if(vramSize == 0) {
		return ErrorResult("no VRAM available for this console");
	}

	uint32_t layer = args.value("layer", 0);
	PpuStateBuffers buffers;
	FillPpuStates(dbg, tools, cpu, buffers);

	std::vector<uint8_t> vram(vramSize);
	dumper->GetMemoryValues(vramType, 0, vramSize - 1, vram.data());

	DebugPaletteInfo paletteInfo = tools->GetPaletteInfo(GetPaletteInfoOptions {});

	GetTilemapOptions options = {};
	options.Layer = (uint8_t)layer;
	options.CompareVram = nullptr;
	options.AccessCounters = nullptr;

	FrameInfo size = tools->GetTilemapSize(options, (BaseState&)buffers.ppuState);
	if(size.Width == 0 || size.Height == 0) {
		return ErrorResult("tilemap layer " + std::to_string(layer) + " is not available for this console/ROM");
	}

	std::vector<uint32_t> buffer((size_t)size.Width * size.Height);
	DebugTilemapInfo info = tools->GetTilemap(options, (BaseState&)buffers.ppuState,
		(BaseState&)buffers.ppuToolsState, vram.data(), paletteInfo.RgbPalette, buffer.data());

	std::string base64 = EncodePngBase64(buffer, size.Width, size.Height);
	if(base64.empty()) {
		return ErrorResult("failed to encode tilemap PNG");
	}

	json result;
	result["image_base64"] = base64;
	result["width"] = size.Width;
	result["height"] = size.Height;
	result["bpp"] = info.Bpp;
	result["tile_width"] = info.TileWidth;
	result["tile_height"] = info.TileHeight;
	result["rows"] = info.RowCount;
	result["columns"] = info.ColumnCount;
	result["scroll_x"] = info.ScrollX;
	result["scroll_y"] = info.ScrollY;
	result["mirroring"] = MirroringToString(info.Mirroring);
	result["tilemap_address"] = info.TilemapAddress;
	result["tileset_address"] = info.TilesetAddress;

	if(args.contains("save_path")) {
		std::string path = args["save_path"].get<std::string>();
		if(!PNGHelper::WritePNG(path, buffer.data(), size.Width, size.Height, 24)) {
			return ErrorResult("failed to write tilemap PNG to " + path);
		}
		result["saved_to"] = path;
	}
	return json{ {"result", result} };
}

//--- Tileset ------------------------------------------------------------------

json EmuSession::GetTiles(const json& args)
{
	std::string error;
	Debugger* dbg = EnsureDebugger(args, error);
	if(!dbg) { return ErrorResult(error); }
	CpuType cpu = ResolveCpu(args, error);
	if(!error.empty()) { return ErrorResult(error); }

	PpuTools* tools = dbg->GetPpuTools(cpu);
	MemoryDumper* dumper = dbg->GetMemoryDumper();

	MemoryType memType;
	TileFormat format;
	if(!GetTilesetConfig(_emu->GetConsoleType(), dumper, memType, format, error)) {
		return ErrorResult(error);
	}

	uint32_t srcSize = dumper->GetMemorySize(memType);
	if(srcSize == 0) {
		return ErrorResult("no tile data (CHR/VRAM) available");
	}
	std::vector<uint8_t> source(srcSize);
	dumper->GetMemoryValues(memType, 0, srcSize - 1, source.data());

	DebugPaletteInfo paletteInfo = tools->GetPaletteInfo(GetPaletteInfoOptions {});

	//Tiles per row (default 16) and derived row count
	uint32_t width = args.value("width", 16);
	if(width == 0 || width > 64) {
		return ErrorResult("width must be between 1 and 64 (tiles per row)");
	}
	uint32_t bytesPerTile = 8; //2bpp NES/GB; corrected below for other formats
	switch(format) {
		case TileFormat::Bpp4: case TileFormat::SmsBpp4: case TileFormat::GbaBpp4:
		case TileFormat::PceSpriteBpp4: bytesPerTile = 32; break;
		default: bytesPerTile = 16; break;
	}
	//NES 2bpp tiles are 16 bytes; GB 2bpp are also 16; bpp1 formats 8 - keep 16 for 2bpp
	if(format == TileFormat::NesBpp2 || format == TileFormat::Bpp2 || format == TileFormat::PceBackgroundBpp2Cg0 || format == TileFormat::PceBackgroundBpp2Cg1) {
		bytesPerTile = 16;
	}
	uint32_t tileCount = srcSize / bytesPerTile;
	uint32_t height = (tileCount + width - 1) / width;

	GetTileViewOptions options = {};
	options.MemType = memType;
	options.Format = format;
	options.Layout = TileLayout::Normal;
	options.Filter = TileFilter::None;
	options.Background = TileBackground::Default;
	options.Width = (int32_t)width;
	options.Height = (int32_t)height;
	options.StartAddress = args.value("start", 0);
	options.Palette = args.value("palette", 0);
	options.UseGrayscalePalette = args.value("grayscale", false);

	uint32_t pixelWidth = width * 8;
	uint32_t pixelHeight = height * 8;
	if(pixelWidth * pixelHeight > 4096 * 4096) {
		return ErrorResult("tile view too large");
	}
	std::vector<uint32_t> buffer((size_t)pixelWidth * pixelHeight);
	tools->GetTileView(options, source.data(), srcSize, paletteInfo.RgbPalette, buffer.data());

	std::string base64 = EncodePngBase64(buffer, pixelWidth, pixelHeight);
	if(base64.empty()) {
		return ErrorResult("failed to encode tileset PNG");
	}

	json result;
	result["image_base64"] = base64;
	result["width"] = pixelWidth;
	result["height"] = pixelHeight;
	result["tile_count"] = tileCount;
	result["bytes_per_tile"] = bytesPerTile;
	result["source_size"] = srcSize;

	if(args.contains("save_path")) {
		std::string path = args["save_path"].get<std::string>();
		if(!PNGHelper::WritePNG(path, buffer.data(), pixelWidth, pixelHeight, 24)) {
			return ErrorResult("failed to write tileset PNG to " + path);
		}
		result["saved_to"] = path;
	}
	return json{ {"result", result} };
}

//--- Sprites ------------------------------------------------------------------

json EmuSession::GetSprites(const json& args)
{
	std::string error;
	Debugger* dbg = EnsureDebugger(args, error);
	if(!dbg) { return ErrorResult(error); }
	CpuType cpu = ResolveCpu(args, error);
	if(!error.empty()) { return ErrorResult(error); }

	PpuTools* tools = dbg->GetPpuTools(cpu);
	MemoryDumper* dumper = dbg->GetMemoryDumper();
	ConsoleType console = _emu->GetConsoleType();

	MemoryType vramType = GetVramMemoryType(console);
	MemoryType oamType = GetOamMemoryType(console);
	uint32_t vramSize = dumper->GetMemorySize(vramType);
	uint32_t oamSize = dumper->GetMemorySize(oamType);
	if(vramSize == 0 || oamSize == 0) {
		return ErrorResult("no VRAM/OAM available for this console");
	}
	PpuStateBuffers buffers;
	FillPpuStates(dbg, tools, cpu, buffers);

	std::vector<uint8_t> vram(vramSize);
	dumper->GetMemoryValues(vramType, 0, vramSize - 1, vram.data());
	std::vector<uint8_t> oam(oamSize);
	dumper->GetMemoryValues(oamType, 0, oamSize - 1, oam.data());

	DebugPaletteInfo paletteInfo = tools->GetPaletteInfo(GetPaletteInfoOptions {});

	GetSpritePreviewOptions options = {};
	options.Background = SpriteBackground::Background;

	DebugSpritePreviewInfo previewInfo = tools->GetSpritePreviewInfo(options, (BaseState&)buffers.ppuState, (BaseState&)buffers.ppuToolsState);

	std::vector<DebugSpriteInfo> sprites(previewInfo.SpriteCount);
	constexpr uint32_t SpritePreviewSize = 128 * 128;
	std::vector<uint32_t> spritePreviews((size_t)previewInfo.SpriteCount * SpritePreviewSize);
	//The core paints the preview at the FULL buffer size from GetSpritePreviewInfo
	//(NES: 256x256 incl. a spare 16-row band; SNES: 512x256 with an offscreen
	//margin column area) - never the visible-only size, or the heap overflows.
	std::vector<uint32_t> screenPreview;
	uint32_t previewWidth = previewInfo.VisibleWidth ? previewInfo.VisibleWidth : previewInfo.Width;
	uint32_t previewHeight = previewInfo.VisibleHeight ? previewInfo.VisibleHeight : previewInfo.Height;
	if(previewInfo.Width > 0 && previewInfo.Height > 0) {
		previewWidth = previewInfo.Width;
		previewHeight = previewInfo.Height;
		screenPreview.resize((size_t)previewWidth * previewHeight);
	}

	tools->GetSpriteList(options, (BaseState&)buffers.ppuState, (BaseState&)buffers.ppuToolsState,
		vram.data(), oam.data(), paletteInfo.RgbPalette, sprites.data(), spritePreviews.data(),
		screenPreview.empty() ? nullptr : screenPreview.data());

	json result;
	result["sprite_count"] = previewInfo.SpriteCount;
	if(previewInfo.VisibleWidth > 0) {
		result["screen_width"] = previewInfo.VisibleWidth;
		result["screen_height"] = previewInfo.VisibleHeight;
	}

	json list = json::array();
	for(const DebugSpriteInfo& sprite : sprites) {
		list.push_back(json{
			{"index", sprite.SpriteIndex},
			{"x", sprite.X},
			{"y", sprite.Y},
			{"raw_x", sprite.RawX},
			{"raw_y", sprite.RawY},
			{"width", sprite.Width},
			{"height", sprite.Height},
			{"tile_index", sprite.TileIndex},
			{"tile_address", sprite.TileAddress},
			{"palette", sprite.Palette},
			{"visibility", VisibilityToString(sprite.Visibility)}
		});
	}
	result["sprites"] = list;

	//Screen preview (sprites on a background) as an image, at the core's full
	//preview size (includes offscreen margins, like the GUI viewer)
	if(!screenPreview.empty()) {
		std::string base64 = EncodePngBase64(screenPreview, previewWidth, previewHeight);
		if(!base64.empty()) {
			result["preview_image_base64"] = base64;
		}
		result["preview_width"] = previewWidth;
		result["preview_height"] = previewHeight;
	}
	return json{ {"result", result} };
}

//--- Audio --------------------------------------------------------------------

json EmuSession::GetAudioSummary(const json& args)
{
	if(!IsRomLoaded()) {
		return ErrorResult("no ROM is loaded");
	}
	uint32_t windowMs = args.value("window_ms", 1000);

	WavCaptureDevice* audio = _audio.get();
	if(!audio->HasData()) {
		return json{ {"result", json{
			{"capturing", false},
			{"note", "no audio has been produced yet - run some frames first"}
		}} };
	}

	WavCaptureDevice::ChannelStats recent[2];
	uint64_t windowClips = 0;
	uint32_t windowSamples = 0;
	audio->GetRecentStats(windowMs, recent, windowClips, windowSamples);

	double totalRms[2] = { 0, 0 };
	int16_t totalPeak[2] = { 0, 0 };
	uint64_t totalSamples = 0, totalClips = 0;
	audio->GetTotalStats(totalRms, totalPeak, totalSamples, totalClips);

	uint64_t rate = audio->GetSampleRate() ? audio->GetSampleRate() : 48000;
	json result;
	result["capturing"] = true;
	result["sample_rate"] = rate;
	result["channels"] = audio->IsStereo() ? 2 : 1;
	result["total_seconds"] = (double)totalSamples / (rate * (audio->IsStereo() ? 2 : 1));
	result["window_ms"] = windowMs;
	result["window"] = json{
		{"rms_left", recent[0].Rms},
		{"rms_right", recent[1].Rms},
		{"peak_left", recent[0].Peak},
		{"peak_right", recent[1].Peak},
		{"clipping_samples", windowClips}
	};
	result["totals"] = json{
		{"rms_left", totalRms[0]},
		{"rms_right", totalRms[1]},
		{"peak_left", totalPeak[0] / 32768.0},
		{"peak_right", totalPeak[1] / 32768.0},
		{"clipping_samples", totalClips}
	};
	return json{ {"result", result} };
}

json EmuSession::CaptureWav(const json& args)
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
		path = FolderUtilities::CombinePath(dir, "audio-" + std::to_string(::time(nullptr)) + ".wav");
	}

	_emu->GetSoundMixer()->StartRecording(path);
	RunFrames(json{ {"frames", frames}, {"timeout_ms", args.value("timeout_ms", 30000)} });
	_emu->GetSoundMixer()->StopRecording();

	std::error_code ec;
	uintmax_t size = fs::file_size(path, ec);
	if(ec || size <= 44) {
		return ErrorResult("WAV capture failed (file missing or header-only)");
	}

	json result;
	result["path"] = path;
	result["bytes"] = (uint64_t)size;
	if(args.value("return_base64", false) && size <= 8 * 1024 * 1024) {
		std::ifstream file(path, std::ios::binary);
		std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		result["file_base64"] = Base64::Encode(std::vector<uint8_t>(data.begin(), data.end()));
	}
	return json{ {"result", result} };
}

//--- Trace to file ------------------------------------------------------------

json EmuSession::TraceToFile(const json& args)
{
	std::string action = args.value("action", "start");

	if(action == "stop") {
		std::string error;
		Debugger* dbg = GetDebuggerOrNull();
		if(!dbg) {
			return ErrorResult("the debugger (and trace logging) was never started");
		}
		dbg->GetTraceLogFileSaver()->StopLogging();
		return json{ {"result", json{ {"action", "stopped"} }} };
	}

	if(action != "start") {
		return ErrorResult("action must be 'start' or 'stop'");
	}

	std::string error;
	Debugger* dbg = EnsureDebugger(args, error);
	if(!dbg) { return ErrorResult(error); }
	CpuType cpu = ResolveCpu(args, error);
	if(!error.empty()) { return ErrorResult(error); }

	std::string path = args.value("path", "");
	if(path.empty()) {
		std::string dir = FolderUtilities::CombinePath(_homeFolder, "traces");
		std::error_code ec;
		fs::create_directories(dir, ec);
		path = FolderUtilities::CombinePath(dir, "trace-" + std::to_string(::time(nullptr)) + ".txt");
	}

	//The file saver only logs rows while the trace logger is enabled
	TraceLoggerOptions options = {};
	options.Enabled = true;
	options.IndentCode = false;
	options.UseLabels = args.value("use_labels", true);
	std::string format = args.value("format", "[PC] [Disassembly]");
	std::string condition = args.value("condition", "");
	strncpy(options.Format, format.c_str(), sizeof(options.Format) - 1);
	strncpy(options.Condition, condition.c_str(), sizeof(options.Condition) - 1);
	dbg->GetTraceLogger(cpu)->SetOptions(options);

	dbg->GetTraceLogFileSaver()->StartLogging(path);

	json result;
	result["action"] = "started";
	result["path"] = path;
	result["note"] = "trace rows are appended while the game runs; call trace_to_file "
		"with action='stop' to flush and close the file";

	if(args.value("run_frames", 0) > 0) {
		json run = RunFrames(json{ {"frames", args["run_frames"]}, {"timeout_ms", args.value("timeout_ms", 10000)} });
		if(run.contains("error")) {
			dbg->GetTraceLogFileSaver()->StopLogging();
			return run;
		}
	}
	if(args.value("auto_stop", false)) {
		dbg->GetTraceLogFileSaver()->StopLogging();
		result["action"] = "started+stopped";
		std::error_code ec;
		result["bytes"] = (uint64_t)fs::file_size(path, ec);
	}
	return json{ {"result", result} };
}
