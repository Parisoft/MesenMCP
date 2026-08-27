//MesenMCP - debugger-backed tool implementations (P2)
//
//All debug tools follow the locking conventions proven by the old InteropDLL:
//every access goes through Emulator::GetDebugger() -> DebuggerRequest (RAII),
//and the debugger is initialized lazily and kept alive (see EmuSession.h note 4).
#include "Mcp/EmuSession.h"

#include "Core/Shared/DebuggerRequest.h"
#include "Core/Shared/EmuSettings.h"
#include "Core/Debugger/DebugTypes.h"
#include "Core/Debugger/DebugUtilities.h"
#include "Core/Debugger/Breakpoint.h"
#include "Core/Debugger/CallstackManager.h"
#include "Core/Debugger/ExpressionEvaluator.h"
#include "Core/Debugger/DisassemblyInfo.h"
#include "Core/Debugger/ITraceLogger.h"
#include "Core/Debugger/LabelManager.h"
#include "Core/Debugger/MemoryDumper.h"
#include "Core/NES/NesTypes.h"
#include "Core/SNES/SnesCpuTypes.h"
#include "Core/SNES/SnesPpuTypes.h"
#include "Core/Gameboy/GbTypes.h"
#include "Core/GBA/GbaTypes.h"
#include "Utilities/HexUtilities.h"
#include "Utilities/Timer.h"

#include <algorithm>
#include <cstring>
#include <thread>

namespace
{
	json ErrorResult(const std::string& message)
	{
		return json{ {"error", message} };
	}

	std::string CpuTypeToString(CpuType cpu)
	{
		switch(cpu) {
			case CpuType::Nes: return "nes";
			case CpuType::Snes: return "snes";
			case CpuType::Spc: return "spc";
			case CpuType::Sa1: return "sa1";
			case CpuType::Gsu: return "gsu";
			case CpuType::Cx4: return "cx4";
			case CpuType::NecDsp: return "necdsp";
			case CpuType::St018: return "st018";
			case CpuType::Gameboy: return "gb";
			case CpuType::Gba: return "gba";
			case CpuType::Pce: return "pce";
			case CpuType::Sms: return "sms";
			case CpuType::Ws: return "ws";
			default: return "unknown";
		}
	}

	bool ParseCpuType(const std::string& value, CpuType& cpu)
	{
		if(value == "nes") { cpu = CpuType::Nes; }
		else if(value == "snes") { cpu = CpuType::Snes; }
		else if(value == "spc") { cpu = CpuType::Spc; }
		else if(value == "sa1") { cpu = CpuType::Sa1; }
		else if(value == "gsu") { cpu = CpuType::Gsu; }
		else if(value == "cx4") { cpu = CpuType::Cx4; }
		else if(value == "necdsp") { cpu = CpuType::NecDsp; }
		else if(value == "st018") { cpu = CpuType::St018; }
		else if(value == "gb" || value == "gameboy") { cpu = CpuType::Gameboy; }
		else if(value == "gba") { cpu = CpuType::Gba; }
		else if(value == "pce") { cpu = CpuType::Pce; }
		else if(value == "sms") { cpu = CpuType::Sms; }
		else if(value == "ws") { cpu = CpuType::Ws; }
		else { return false; }
		return true;
	}

	//Console prefix for memory type names, e.g. NesWorkRam -> "work_ram"
	std::string ConsolePrefix(ConsoleType type)
	{
		switch(type) {
			case ConsoleType::Nes: return "Nes";
			case ConsoleType::Snes: return "Snes";
			case ConsoleType::Gameboy: return "Gb";
			case ConsoleType::Gba: return "Gba";
			case ConsoleType::PcEngine: return "Pce";
			case ConsoleType::Sms: return "Sms";
			case ConsoleType::Ws: return "Ws";
			default: return "";
		}
	}

	//snake_case of an enum name, e.g. "NesNametableRam" -> "nes_nametable_ram"
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

	std::string MemoryTypeToName(MemoryType type)
	{
		std::string name = EnumToSnake(magic_enum::enum_name(type).data());
		//NesIntWorkRam? no - GbaIntWorkRam vs GbaExtWorkRam keep full names;
		//just shorten the common ones for ergonomics:
		return name;
	}

	uint32_t ParseHexOrDec(const std::string& valueIn, bool& ok)
	{
		ok = false;
		std::string value = valueIn;
		int base = 10;
		if(!value.empty() && value[0] == '$') {
			value = value.substr(1);
			base = 16;
		} else if(value.size() > 2 && (value.substr(0, 2) == "0x" || value.substr(0, 2) == "0X")) {
			value = value.substr(2);
			base = 16;
		}
		try {
			size_t pos = 0;
			unsigned long result = std::stoul(value, &pos, base);
			if(pos == value.size() && !value.empty()) {
				ok = true;
				return (uint32_t)result;
			}
			return 0;
		} catch(...) {
			return 0;
		}
	}

	std::vector<uint8_t> ParseHexBytes(const std::string& hex, bool& ok)
	{
		ok = false;
		auto nibble = [](char c) -> int {
			if(c >= '0' && c <= '9') { return c - '0'; }
			if(c >= 'a' && c <= 'f') { return c - 'a' + 10; }
			if(c >= 'A' && c <= 'F') { return c - 'A' + 10; }
			return -1;
		};
		std::string clean;
		for(char c : hex) {
			if(c != ' ' && c != ':' && c != ',') {
				clean += c;
			}
		}
		if(clean.size() % 2 != 0) {
			return {};
		}
		std::vector<uint8_t> bytes;
		for(size_t i = 0; i < clean.size(); i += 2) {
			int hi = nibble(clean[i]);
			int lo = nibble(clean[i + 1]);
			if(hi < 0 || lo < 0) {
				return {};
			}
			bytes.push_back((uint8_t)((hi << 4) | lo));
		}
		ok = !bytes.empty();
		return bytes;
	}

	StepType ParseStepType(const std::string& value, bool& ok)
	{
		ok = true;
		if(value == "instruction" || value == "in") { return StepType::Step; }
		if(value == "over") { return StepType::StepOver; }
		if(value == "out") { return StepType::StepOut; }
		if(value == "cycle" || value == "cpu_cycle") { return StepType::CpuCycleStep; }
		if(value == "scanline" || value == "ppu_scanline") { return StepType::PpuScanline; }
		if(value == "frame" || value == "ppu_frame") { return StepType::PpuFrame; }
		ok = false;
		return StepType::Step;
	}

	std::string BreakSourceToString(BreakSource source)
	{
		switch(source) {
			case BreakSource::Breakpoint: return "breakpoint";
			case BreakSource::Pause: return "pause";
			case BreakSource::CpuStep: return "cpu_step";
			case BreakSource::PpuStep: return "ppu_step";
			case BreakSource::Irq: return "irq";
			case BreakSource::Nmi: return "nmi";
			default: return "unspecified";
		}
	}

	//Poll until the debugger reports execution stopped, a new break event
	//arrives, or the timeout elapses. When the CPU was already stopped (e.g.
	//stepping from a breakpoint), only a NEW break event counts as progress.
	bool WaitForExecutionStop(Debugger* dbg, NotificationBridge* bridge, uint64_t beforeSequence,
		bool wasStopped, uint32_t timeoutMs)
	{
		auto progressed = [&]() {
			uint64_t seq = bridge->CurrentSequence();
			return wasStopped ? (seq > beforeSequence) : (seq > beforeSequence || dbg->IsExecutionStopped());
		};
		Timer timer;
		while(timer.GetElapsedMS() < timeoutMs) {
			if(progressed()) {
				return true;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(3));
		}
		return progressed();
	}
}

//--- Debugger lifecycle -------------------------------------------------------

Debugger* EmuSession::GetDebuggerOrNull()
{
	if(!IsRomLoaded()) {
		return nullptr;
	}
	DebuggerRequest request = _emu->GetDebugger(false);
	return request.GetDebugger();
}

Debugger* EmuSession::EnsureDebugger(const json& args, std::string& error)
{
	if(!IsRomLoaded()) {
		error = "no ROM is loaded";
		return nullptr;
	}

	if(GetDebuggerOrNull() == nullptr) {
		//Lazily start the debugger (kept alive for the rest of the session; it
		//survives ROM reloads - see EmuSession.h note 4)
		//Also enable the per-console debugger instrumentation flags - the GUI set
		//these when its debugger windows were open; some features (e.g. the GBA
		//code/data logger feed) are gated behind them.
		EmuSettings* settings = _emu->GetSettings();
		settings->SetDebuggerFlag(DebuggerFlags::NesDebuggerEnabled, true);
		settings->SetDebuggerFlag(DebuggerFlags::SnesDebuggerEnabled, true);
		settings->SetDebuggerFlag(DebuggerFlags::GbDebuggerEnabled, true);
		settings->SetDebuggerFlag(DebuggerFlags::GbaDebuggerEnabled, true);
		settings->SetDebuggerFlag(DebuggerFlags::PceDebuggerEnabled, true);
		settings->SetDebuggerFlag(DebuggerFlags::SmsDebuggerEnabled, true);
		settings->SetDebuggerFlag(DebuggerFlags::WsDebuggerEnabled, true);

		_emu->InitDebugger();

		DebuggerRequest request = _emu->GetDebugger(false);
		if(request.GetDebugger() == nullptr) {
			error = "failed to start the debugger";
			return nullptr;
		}
	}

	Debugger* dbg = GetDebuggerOrNull();
	if(dbg == nullptr) {
		error = "debugger is not available";
	}
	return dbg;
}

CpuType EmuSession::ResolveCpu(const json& args, std::string& error)
{
	std::vector<CpuType> cpuTypes = _emu->GetCpuTypes();
	if(args.contains("cpu")) {
		std::string name = args["cpu"].get<std::string>();
		CpuType cpu;
		if(!ParseCpuType(name, cpu)) {
			error = "unknown cpu '" + name + "'";
			return (CpuType)-1;
		}
		bool supported = false;
		for(CpuType t : cpuTypes) {
			supported |= (t == cpu);
		}
		if(!supported) {
			error = "cpu '" + name + "' does not exist in the loaded ROM";
			return (CpuType)-1;
		}
		return cpu;
	}
	return cpuTypes.empty() ? CpuType::Nes : cpuTypes[0];
}

//--- Memory types -------------------------------------------------------------

std::vector<std::pair<std::string, MemoryType>> EmuSession::GetAvailableMemoryTypes(Debugger* dbg)
{
	std::vector<std::pair<std::string, MemoryType>> result;
	MemoryDumper* dumper = dbg->GetMemoryDumper();
	std::string prefix = ConsolePrefix(_emu->GetConsoleType());
	std::string prefixLower;
	for(char c : prefix) { prefixLower += (char)::tolower(c); }
	for(MemoryType type : magic_enum::enum_values<MemoryType>()) {
		if(type == MemoryType::None) {
			continue;
		}
		std::string name = EnumToSnake(magic_enum::enum_name(type).data());
		if(!prefixLower.empty() && name == prefixLower + "_memory") {
			name = "cpu";  //the console's CPU bus (e.g. NesMemory)
		} else if(!prefixLower.empty() && name.rfind(prefixLower + "_", 0) == 0) {
			name = name.substr(prefixLower.size() + 1);
		}
		if(dumper->GetMemorySize(type) > 0) {
			result.push_back({ name, type });
		}
	}
	return result;
}

bool EmuSession::ResolveMemoryType(const std::string& nameIn, MemoryType& type, std::string& error)
{
	std::string name = nameIn;
	std::transform(name.begin(), name.end(), name.begin(), ::tolower);

	Debugger* dbg = GetDebuggerOrNull();
	if(dbg == nullptr) {
		error = "no ROM is loaded";
		return false;
	}

	//Friendly aliases
	std::string prefix = ConsolePrefix(_emu->GetConsoleType());
	if(name == "cpu" || name == "cpu_bus" || name == "bus") {
		CpuType cpu = ResolveCpu(json{}, error);
		if(!error.empty()) {
			return false;
		}
		type = DebugUtilities::GetCpuMemoryType(cpu);
		return true;
	}
	if(name == "ram" || name == "wram") { name = "work_ram"; }
	if(name == "sram") { name = "save_ram"; }
	if(name == "vram") { name = "video_ram"; }
	if(name == "oam") { name = "sprite_ram"; }
	if(name == "palette" || name == "cgram") { name = "palette_ram"; }

	std::string prefixed = prefix.empty() ? name : EnumToSnake((prefix + "_" + name).c_str());
	std::string flatPrefixed;
	for(char c : prefixed) { if(c != '_') { flatPrefixed += c; } }

	auto values = magic_enum::enum_values<MemoryType>();
	for(MemoryType t : values) {
		std::string tname = EnumToSnake(magic_enum::enum_name(t).data());
		std::string tflat;
		for(char c : tname) { if(c != '_') { tflat += c; } }
		if(tname == name || tname == prefixed || tflat == flatPrefixed || magic_enum::enum_name(t) == name) {
			if(dbg->GetMemoryDumper()->GetMemorySize(t) == 0) {
				break; //matched by name but not present in this console
			}
			type = t;
			return true;
		}
	}

	std::string available;
	for(auto& [n, t] : GetAvailableMemoryTypes(dbg)) {
		available += (available.empty() ? "" : ", ") + n;
	}
	error = "unknown or unavailable memory type '" + nameIn + "'. Available: " + available;
	return false;
}

//--- Tier 1: state ------------------------------------------------------------

json EmuSession::GetCpuState(const json& args)
{
	std::string error;
	Debugger* dbg = EnsureDebugger(args, error);
	if(!dbg) { return ErrorResult(error); }
	CpuType cpu = ResolveCpu(args, error);
	if(!error.empty()) { return ErrorResult(error); }
	return json{ {"result", SerializeCpuState(dbg, cpu)} };
}

json EmuSession::SerializeCpuState(Debugger* dbg, CpuType cpu)
{
	json state;
	state["cpu"] = CpuTypeToString(cpu);
	switch(cpu) {
		case CpuType::Nes: {
			NesCpuState s;
			dbg->GetCpuState(s, cpu);
			state["pc"] = s.PC;
			state["a"] = s.A;
			state["x"] = s.X;
			state["y"] = s.Y;
			state["sp"] = s.SP;
			state["ps"] = s.PS;
			state["flags"] = json{
				{"n", (s.PS & 0x80) != 0}, {"v", (s.PS & 0x40) != 0},
				{"d", (s.PS & 0x08) != 0}, {"i", (s.PS & 0x04) != 0},
				{"z", (s.PS & 0x02) != 0}, {"c", (s.PS & 0x01) != 0} };
			state["cycle_count"] = s.CycleCount;
			state["nmi_flag"] = s.NmiFlag;
			state["irq_flag"] = s.IrqFlag;
			break;
		}
		case CpuType::Snes: case CpuType::Sa1: {
			SnesCpuState s;
			dbg->GetCpuState(s, cpu);
			state["pc"] = s.PC;
			state["k"] = s.K;
			state["full_pc"] = (s.K << 16) | s.PC;
			state["a"] = s.A;
			state["x"] = s.X;
			state["y"] = s.Y;
			state["sp"] = s.SP;
			state["d"] = s.D;
			state["dbr"] = s.DBR;
			state["ps"] = s.PS;
			state["flags"] = json{
				{"n", (s.PS & 0x80) != 0}, {"v", (s.PS & 0x40) != 0},
				{"m", (s.PS & 0x20) != 0}, {"x", (s.PS & 0x10) != 0},
				{"d", (s.PS & 0x08) != 0}, {"i", (s.PS & 0x04) != 0},
				{"z", (s.PS & 0x02) != 0}, {"c", (s.PS & 0x01) != 0} };
			state["emulation_mode"] = s.EmulationMode;
			state["cycle_count"] = s.CycleCount;
			break;
		}
		case CpuType::Gameboy: {
			GbCpuState s;
			dbg->GetCpuState(s, cpu);
			state["pc"] = s.PC;
			state["a"] = s.A;
			state["b"] = s.B;
			state["c"] = s.C;
			state["d"] = s.D;
			state["e"] = s.E;
			state["h"] = s.H;
			state["l"] = s.L;
			state["sp"] = s.SP;
			state["flags"] = json{
				{"z", (s.Flags & 0x80) != 0}, {"n", (s.Flags & 0x40) != 0},
				{"h", (s.Flags & 0x20) != 0}, {"c", (s.Flags & 0x10) != 0} };
			state["cycle_count"] = s.CycleCount;
			break;
		}
		case CpuType::Gba: {
			GbaCpuState s;
			dbg->GetCpuState(s, cpu);
			json regs = json::object();
			for(int i = 0; i < 13; i++) {
				regs["r" + std::to_string(i)] = s.R[i];
			}
			regs["r13_sp"] = s.R[13];
			regs["r14_lr"] = s.R[14];
			regs["r15_pc"] = s.R[15];
			state["registers"] = regs;
			state["pc"] = s.R[15];
			state["thumb"] = s.CPSR.Thumb;
			state["mode"] = (int)s.CPSR.Mode;
			state["irq_disable"] = s.CPSR.IrqDisable;
			state["fiq_disable"] = s.CPSR.FiqDisable;
			state["flags"] = json{ {"negative", s.CPSR.Negative}, {"zero", s.CPSR.Zero}, {"carry", s.CPSR.Carry}, {"overflow", s.CPSR.Overflow} };
			state["stopped"] = s.Stopped;
			break;
		}
		default:
			state["error"] = "state serialization not implemented for cpu '" + CpuTypeToString(cpu) + "'";
			break;
	}
	return state;
}

json EmuSession::GetPpuState(const json& args)
{
	std::string error;
	Debugger* dbg = EnsureDebugger(args, error);
	if(!dbg) { return ErrorResult(error); }
	CpuType cpu = ResolveCpu(args, error);
	if(!error.empty()) { return ErrorResult(error); }
	return json{ {"result", SerializePpuState(dbg, cpu)} };
}

json EmuSession::SerializePpuState(Debugger* dbg, CpuType cpu)
{
	json state;
	switch(_emu->GetConsoleType()) {
		case ConsoleType::Nes: {
			NesPpuState s;
			dbg->GetPpuState(s, cpu);
			state["scanline"] = s.Scanline;
			state["cycle"] = s.Cycle;
			state["frame_count"] = s.FrameCount;
			state["scanline_count"] = s.ScanlineCount;
			state["vblank_flag"] = s.StatusFlags.VerticalBlank;
			state["sprite0_hit"] = s.StatusFlags.Sprite0Hit;
			state["sprite_overflow"] = s.StatusFlags.SpriteOverflow;
			state["mask"] = json{
				{"background_enabled", s.Mask.BackgroundEnabled},
				{"sprites_enabled", s.Mask.SpritesEnabled},
				{"grayscale", s.Mask.Grayscale},
				{"left_column_background_hidden", s.Mask.BackgroundMask},
				{"left_column_sprites_hidden", s.Mask.SpriteMask} };
			state["control"] = json{
				{"nmi_enabled", s.Control.NmiOnVerticalBlank},
				{"background_pattern_addr", s.Control.BackgroundPatternAddr},
				{"sprite_pattern_addr", s.Control.SpritePatternAddr},
				{"large_sprites", s.Control.LargeSprites} };
			state["vram_address"] = s.VideoRamAddr;
			state["temp_vram_address"] = s.TmpVideoRamAddr;
			state["scroll_x"] = s.ScrollX;
			break;
		}
		case ConsoleType::Snes: {
			SnesPpuState s;
			dbg->GetPpuState(s, cpu);
			state["scanline"] = s.Scanline;
			state["cycle"] = s.Cycle;
			state["hclock"] = s.HClock;
			state["frame_count"] = s.FrameCount;
			state["bg_mode"] = s.BgMode;
			state["forced_blank"] = s.ForcedBlank;
			state["screen_brightness"] = s.ScreenBrightness;
			state["main_screen_layers"] = s.MainScreenLayers;
			state["sub_screen_layers"] = s.SubScreenLayers;
			break;
		}
		case ConsoleType::Gba: {
			GbaPpuState s;
			dbg->GetPpuState(s, cpu);
			state["frame_count"] = s.FrameCount;
			state["scanline"] = s.Scanline;
			state["cycle"] = s.Cycle;
			state["bg_mode"] = s.BgMode;
			state["forced_blank"] = s.ForcedBlank;
			state["hblank_irq_enabled"] = s.HblankIrqEnabled;
			state["vblank_irq_enabled"] = s.VblankIrqEnabled;
			break;
		}
		default:
			state["error"] = "ppu state serialization not implemented for this console";
			break;
	}
	return state;
}

//--- Combined register/cycle view ---------------------------------------------

json EmuSession::GetRegisters(const json& args)
{
	std::string error;
	Debugger* dbg = EnsureDebugger(args, error);
	if(!dbg) { return ErrorResult(error); }
	CpuType cpu = ResolveCpu(args, error);
	if(!error.empty()) { return ErrorResult(error); }

	//One-shot view: CPU registers + decoded status flags + cycle count, plus
	//the PPU position (scanline + dot within the scanline + frame count)
	json result = SerializeCpuState(dbg, cpu);
	result["ppu"] = SerializePpuState(dbg, cpu);
	result["master_clock"] = _emu->GetMasterClock();
	result["master_clock_rate"] = _emu->GetMasterClockRate();
	result.erase("error");
	return json{ {"result", result} };
}

//--- Tier 1: memory -----------------------------------------------------------

json EmuSession::GetMemorySize(const json& args)
{
	std::string error;
	Debugger* dbg = EnsureDebugger(args, error);
	if(!dbg) { return ErrorResult(error); }
	if(!args.contains("memory_type")) {
		return ErrorResult("missing required string argument: memory_type");
	}
	MemoryType type;
	if(!ResolveMemoryType(args["memory_type"].get<std::string>(), type, error)) {
		return ErrorResult(error);
	}
	uint32_t size = dbg->GetMemoryDumper()->GetMemorySize(type);
	return json{ {"result", {
		{"memory_type", args["memory_type"]},
		{"size", size},
		{"size_hex", HexUtilities::ToHex32(size)}
	}} };
}

json EmuSession::ReadMemory(const json& args)
{
	std::string error;
	Debugger* dbg = EnsureDebugger(args, error);
	if(!dbg) { return ErrorResult(error); }
	if(!args.contains("memory_type") || !args.contains("address")) {
		return ErrorResult("required arguments: memory_type (string), address (int); optional: length (int)");
	}
	MemoryType type;
	if(!ResolveMemoryType(args["memory_type"].get<std::string>(), type, error)) {
		return ErrorResult(error);
	}

	bool ok = false;
	uint32_t address = args.contains("address") && args["address"].is_string()
		? ParseHexOrDec(args["address"].get<std::string>(), ok)
		: args["address"].get<uint32_t>();
	if(!ok && args["address"].is_string()) { return ErrorResult("invalid address"); }

	uint32_t length = args.value("length", 1);
	if(length == 0 || length > 1024 * 1024) {
		return ErrorResult("length must be between 1 and 1048576");
	}

	MemoryDumper* dumper = dbg->GetMemoryDumper();
	uint32_t size = dumper->GetMemorySize(type);
	if(address >= size || length > size - address) {
		return ErrorResult("range [$" + HexUtilities::ToHex32(address) + ", +" + std::to_string(length) +
			") is out of bounds for memory type (size $" + HexUtilities::ToHex32(size) + ")");
	}

	std::vector<uint8_t> data(length);
	dumper->GetMemoryValues(type, address, address + length - 1, data.data());

	std::string hex;
	hex.reserve(length * 3);
	for(uint8_t byte : data) {
		hex += HexUtilities::ToHex(byte) + " ";
	}
	if(!hex.empty()) {
		hex.pop_back();
	}

	json result{
		{"memory_type", args["memory_type"]},
		{"address", address},
		{"length", length},
		{"hex", hex}
	};
	if(length <= 64) {
		json bytes = json::array();
		for(uint8_t byte : data) {
			bytes.push_back(byte);
		}
		result["bytes"] = bytes;
	}
	return json{ {"result", result} };
}

json EmuSession::WriteMemory(const json& args)
{
	std::string error;
	Debugger* dbg = EnsureDebugger(args, error);
	if(!dbg) { return ErrorResult(error); }
	if(!args.contains("memory_type") || !args.contains("address") || !args.contains("bytes")) {
		return ErrorResult("required arguments: memory_type (string), address (int), bytes "
			"(array of ints 0-255, or a hex string like \"8D 06 20\")");
	}
	MemoryType type;
	if(!ResolveMemoryType(args["memory_type"].get<std::string>(), type, error)) {
		return ErrorResult(error);
	}

	bool ok = false;
	uint32_t address = args["address"].is_string()
		? ParseHexOrDec(args["address"].get<std::string>(), ok)
		: args["address"].get<uint32_t>();
	if(!ok && args["address"].is_string()) { return ErrorResult("invalid address"); }

	std::vector<uint8_t> bytes;
	if(args["bytes"].is_string()) {
		bytes = ParseHexBytes(args["bytes"].get<std::string>(), ok);
		if(!ok) {
			return ErrorResult("invalid hex byte string (expected e.g. \"8D 06 20\" or \"8D0620\")");
		}
	} else if(args["bytes"].is_array()) {
		for(const json& v : args["bytes"]) {
			if(!v.is_number_integer() || v.get<int>() < 0 || v.get<int>() > 255) {
				return ErrorResult("bytes array must contain integers 0-255");
			}
			bytes.push_back((uint8_t)v.get<int>());
		}
	} else {
		return ErrorResult("bytes must be an array of ints or a hex string");
	}
	if(bytes.empty()) {
		return ErrorResult("no bytes to write");
	}

	MemoryDumper* dumper = dbg->GetMemoryDumper();
	uint32_t size = dumper->GetMemorySize(type);
	if(address >= size || bytes.size() > size - address) {
		return ErrorResult("write range is out of bounds (size $" + HexUtilities::ToHex32(size) + ")");
	}

	dumper->SetMemoryValues(type, address, bytes.data(), (uint32_t)bytes.size());
	return json{ {"result", json{
		{"written", bytes.size()},
		{"address", address},
		{"memory_type", args["memory_type"]}
	}} };
}

json EmuSession::SearchMemory(const json& args)
{
	std::string error;
	Debugger* dbg = EnsureDebugger(args, error);
	if(!dbg) { return ErrorResult(error); }
	if(!args.contains("memory_type") || !args.contains("value")) {
		return ErrorResult("required arguments: memory_type (string), value (int or hex string); "
			"optional: start, max_results");
	}
	MemoryType type;
	if(!ResolveMemoryType(args["memory_type"].get<std::string>(), type, error)) {
		return ErrorResult(error);
	}

	std::vector<uint8_t> needle;
	if(args["value"].is_string()) {
		bool ok = false;
		needle = ParseHexBytes(args["value"].get<std::string>(), ok);
		if(!ok) { return ErrorResult("value as string must be hex bytes, e.g. \"A9 16\""); }
	} else if(args["value"].is_number_integer()) {
		//Single integer values are searched little-endian (native console order)
		uint64_t value = (uint64_t)args["value"].get<int64_t>();
		int width = args.value("value_size", 1);
		if(width != 1 && width != 2 && width != 4) {
			return ErrorResult("value_size must be 1, 2 or 4");
		}
		if(width == 1 && value > 0xFF) { width = 2; }
		if(width == 2 && value > 0xFFFF) { width = 4; }
		for(int i = 0; i < width; i++) {
			needle.push_back((uint8_t)((value >> (8 * i)) & 0xFF));
		}
	} else {
		return ErrorResult("value must be an integer or hex string");
	}

	MemoryDumper* dumper = dbg->GetMemoryDumper();
	uint32_t size = dumper->GetMemorySize(type);
	uint32_t start = args.value("start", 0);
	uint32_t maxResults = args.value("max_results", 100);
	if(start >= size) {
		return ErrorResult("start is out of bounds");
	}

	//Read in chunks and search
	std::vector<uint32_t> matches;
	constexpr uint32_t ChunkSize = 256 * 1024;
	std::vector<uint8_t> buffer;
	uint32_t pos = start;
	while(pos < size && matches.size() < maxResults) {
		uint32_t chunkEnd = std::min(size, pos + ChunkSize);
		uint32_t chunkLen = chunkEnd - pos;
		if(chunkLen < needle.size()) { break; }
		buffer.resize(chunkLen);
		dumper->GetMemoryValues(type, pos, chunkEnd - 1, buffer.data());
		for(uint32_t i = 0; i + needle.size() <= chunkLen && matches.size() < maxResults; i++) {
			if(memcmp(buffer.data() + i, needle.data(), needle.size()) == 0) {
				matches.push_back(pos + i);
			}
		}
		pos = chunkEnd - (uint32_t)needle.size() + 1;
	}

	json addresses = json::array();
	for(uint32_t match : matches) {
		addresses.push_back(match);
	}
	return json{ {"result", json{
		{"matches", addresses},
		{"match_count", matches.size()},
		{"truncated", matches.size() >= maxResults},
		{"memory_type", args["memory_type"]}
	}} };
}

//--- Tier 1: disassembly & expressions ----------------------------------------

json EmuSession::Disassemble(const json& args)
{
	std::string error;
	Debugger* dbg = EnsureDebugger(args, error);
	if(!dbg) { return ErrorResult(error); }
	CpuType cpu = ResolveCpu(args, error);
	if(!error.empty()) { return ErrorResult(error); }

	MemoryType memType = DebugUtilities::GetCpuMemoryType(cpu);
	uint32_t memSize = dbg->GetMemoryDumper()->GetMemorySize(memType);

	bool ok = false;
	uint32_t address;
	if(args.contains("address")) {
		address = args["address"].is_string() ? ParseHexOrDec(args["address"].get<std::string>(), ok)
			: (args["address"].get<uint32_t>());
		if(!ok && args["address"].is_string()) { return ErrorResult("invalid address"); }
	} else {
		address = dbg->GetProgramCounter(cpu, false);
	}
	uint32_t count = args.value("count", 20);
	if(count == 0 || count > 500) {
		return ErrorResult("count must be between 1 and 500");
	}

	json rows = json::array();
	for(uint32_t i = 0; i < count; i++) {
		DisassemblyInfo info;
		info.Initialize(address, 0, cpu, memType, dbg->GetMemoryDumper());
		std::string text;
		info.GetDisassembly(text, address, dbg->GetLabelManager(), _emu->GetSettings());
		rows.push_back(json{
			{"address", address},
			{"address_hex", HexUtilities::ToHex32(address)},
			{"text", text}
		});
		address = (address + info.GetOpSize()) % memSize;
	}

	return json{ {"result", json{
		{"cpu", CpuTypeToString(cpu)},
		{"instructions", rows}
	}} };
}

json EmuSession::EvaluateExpression(const json& args)
{
	std::string error;
	Debugger* dbg = EnsureDebugger(args, error);
	if(!dbg) { return ErrorResult(error); }
	CpuType cpu = ResolveCpu(args, error);
	if(!error.empty()) { return ErrorResult(error); }
	if(!args.contains("expression") || !args["expression"].is_string()) {
		return ErrorResult("missing required string argument: expression");
	}

	std::string expression = args["expression"].get<std::string>();
	EvalResultType resultType = EvalResultType::Invalid;
	int64_t value = dbg->EvaluateExpression(expression, cpu, resultType, true);

	if(resultType == EvalResultType::Invalid) {
		return ErrorResult("invalid expression: " + expression);
	}

	std::string typeName;
	switch(resultType) {
		case EvalResultType::Numeric: typeName = "numeric"; break;
		case EvalResultType::Boolean: typeName = "boolean"; break;
		case EvalResultType::DivideBy0: return ErrorResult("division by zero in expression");
		case EvalResultType::OutOfScope: return ErrorResult("expression is out of scope while the CPU is running");
		default: typeName = "numeric"; break;
	}

	return json{ {"result", json{
		{"value", value},
		{"hex", HexUtilities::ToHex32((uint32_t)value)},
		{"type", typeName}
	}} };
}

//--- Tier 2: breakpoints ------------------------------------------------------

void EmuSession::PushBreakpoints(Debugger* dbg)
{
		std::vector<Breakpoint> coreBreakpoints;
		for(const BreakpointSpec& spec : _breakpoints) {
			int flags = (int)BreakpointTypeFlags::None;
			if(spec.access.find("read") != std::string::npos) { flags |= (int)BreakpointTypeFlags::Read; }
			if(spec.access.find("write") != std::string::npos) { flags |= (int)BreakpointTypeFlags::Write; }
			if(spec.access.find("exec") != std::string::npos) { flags |= (int)BreakpointTypeFlags::Execute; }
			if(flags == (int)BreakpointTypeFlags::None) { flags = (int)BreakpointTypeFlags::Execute; }

			Breakpoint bp;
			bp.Init(spec.id, spec.cpuType, spec.memoryType, (BreakpointTypeFlags)flags,
				spec.startAddress, spec.endAddress, spec.enabled, false, false, spec.condition);
			coreBreakpoints.push_back(bp);
		}
	dbg->SetBreakpoints(coreBreakpoints.data(), (uint32_t)coreBreakpoints.size());
}

json EmuSession::SetBreakpoint(const json& args)
{
	std::string error;
	Debugger* dbg = EnsureDebugger(args, error);
	if(!dbg) { return ErrorResult(error); }
	CpuType cpu = ResolveCpu(args, error);
	if(!error.empty()) { return ErrorResult(error); }

	if(!args.contains("address")) {
		return ErrorResult("required argument: address (int or hex string); optional: end_address, "
			"access (read/write/execute), memory_type, condition");
	}

	bool ok = true;
	int32_t address = args["address"].is_string()
		? (int32_t)ParseHexOrDec(args["address"].get<std::string>(), ok)
		: args["address"].get<int32_t>();
	if(!ok) { return ErrorResult("invalid address"); }
	int32_t endAddress = address;
	if(args.contains("end_address")) {
		endAddress = args["end_address"].is_string()
			? (int32_t)ParseHexOrDec(args["end_address"].get<std::string>(), ok)
			: args["end_address"].get<int32_t>();
		if(!ok) { return ErrorResult("invalid end_address"); }
	}
	if(endAddress < address) {
		return ErrorResult("end_address must be >= address");
	}

	std::string access = args.value("access", "execute");
	for(char& c : access) { c = (char)::tolower(c); }

	//Validate access as a combination of read/write/execute tokens
	std::string normalized;
	std::string token;
	bool validAccess = true;
	auto checkToken = [&]() {
		if(token.empty()) {
			return;
		}
		if(token == "read" || token == "write" || token == "execute" || token == "exec") {
			normalized += (token == "exec" ? std::string("execute") : token) + ",";
		} else {
			validAccess = false;
		}
		token.clear();
	};
	for(char c : access) {
		if(c == ',' || c == ' ') { checkToken(); }
		else { token += c; }
	}
	checkToken();
	if(!validAccess || normalized.empty()) {
		return ErrorResult("access must combine 'read', 'write' and/or 'execute' (e.g. \"read,write\")");
	}
	access = normalized.substr(0, normalized.size() - 1);

	MemoryType memoryType = DebugUtilities::GetCpuMemoryType(cpu);
	if(args.contains("memory_type")) {
		if(!ResolveMemoryType(args["memory_type"].get<std::string>(), memoryType, error)) {
			return ErrorResult(error);
		}
	}

	if(_breakpoints.size() >= 64) {
		return ErrorResult("breakpoint limit reached (64); remove some first");
	}

	BreakpointSpec spec;
	spec.id = _nextBreakpointId++;
	spec.cpuType = cpu;
	spec.memoryType = memoryType;
	spec.startAddress = address;
	spec.endAddress = endAddress;
	spec.access = access;
	spec.condition = args.value("condition", "");
	spec.enabled = args.value("enabled", true);
	_breakpoints.push_back(spec);

	PushBreakpoints(dbg);

	return json{ {"result", json{
		{"id", spec.id},
		{"address", spec.startAddress},
		{"end_address", spec.endAddress},
		{"access", spec.access},
		{"condition", spec.condition},
		{"total_breakpoints", _breakpoints.size()}
	}} };
}

json EmuSession::RemoveBreakpoint(const json& args)
{
	std::string error;
	Debugger* dbg = EnsureDebugger(args, error);
	if(!dbg) { return ErrorResult(error); }

	if(args.value("all", false)) {
		size_t removed = _breakpoints.size();
		_breakpoints.clear();
		PushBreakpoints(dbg);
		return json{ {"result", json{ {"removed", removed} }} };
	}

	if(!args.contains("id")) {
		return ErrorResult("required argument: id, or all=true");
	}
	uint32_t id = args["id"].get<uint32_t>();
	size_t before = _breakpoints.size();
	_breakpoints.erase(std::remove_if(_breakpoints.begin(), _breakpoints.end(),
		[id](const BreakpointSpec& b) { return b.id == id; }), _breakpoints.end());
	if(_breakpoints.size() == before) {
		return ErrorResult("no breakpoint with id " + std::to_string(id));
	}
	PushBreakpoints(dbg);
	return json{ {"result", json{ {"removed", id}, {"total_breakpoints", _breakpoints.size()} }} };
}

//--- Tier 2: stepping / break flow --------------------------------------------

json EmuSession::BreakSummary(Debugger* dbg)
{
	NotificationBridge::BreakInfo info = _bridge->GetLastBreak();
	json out;
	out["execution_stopped"] = dbg->IsExecutionStopped();
	if(info.Valid) {
		out["last_break"] = json{
			{"breakpoint_id", info.BreakpointId},
			{"cpu", CpuTypeToString(info.SourceCpu)},
			{"source", BreakSourceToString(info.Source)}
		};
	}
	return out;
}

json EmuSession::Step(const json& args)
{
	std::string error;
	Debugger* dbg = EnsureDebugger(args, error);
	if(!dbg) { return ErrorResult(error); }
	CpuType cpu = ResolveCpu(args, error);
	if(!error.empty()) { return ErrorResult(error); }

	std::string typeName = args.value("type", "instruction");
	bool ok = false;
	StepType type = ParseStepType(typeName, ok);
	if(!ok) {
		return ErrorResult("invalid step type '" + typeName + "' (instruction, over, out, cycle, scanline, frame)");
	}
	uint32_t count = args.value("count", 1);
	if(count == 0 || count > 1000000) {
		return ErrorResult("count must be between 1 and 1000000");
	}
	uint32_t timeoutMs = args.value("timeout_ms", 5000);

	//Give the emulation thread a moment to fully park when stepping from a
	//stopped state - racing Debugger::Step against the park sequence can lose
	//the step request entirely.
	if(dbg->IsExecutionStopped()) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	bool wasStopped = dbg->IsExecutionStopped();
	uint64_t seq = _bridge->CurrentSequence();

	//Debugger::Step arms a step request and releases the current break - at max
	//speed this races occasionally and the request is lost (no new break event).
	//Retry a couple of times before giving up.
	bool stopped = false;
	for(int attempt = 0; attempt < 3 && !stopped; attempt++) {
		dbg->Step(cpu, (int32_t)count, type, BreakSource::CpuStep);
		stopped = WaitForExecutionStop(dbg, _bridge.get(), seq, wasStopped, attempt == 0 ? timeoutMs : 1500);
	}

	json result;
	result["stopped"] = stopped;
	result["step_type"] = typeName;
	result["count"] = count;
	result["cpu_state"] = SerializeCpuState(dbg, cpu);
	result["last_break"] = BreakSummary(dbg)["last_break"];
	if(!stopped) {
		result["note"] = "timed out waiting for the step to complete - the CPU may not be "
			"executing (e.g. waiting on a halted state); try continue or check status";
	}
	return json{ {"result", result} };
}

json EmuSession::Continue()
{
	std::string error;
	Debugger* dbg = EnsureDebugger(json{}, error);
	if(!dbg) { return ErrorResult(error); }
	dbg->Run();
	return json{ {"result", BreakSummary(dbg)} };
}

json EmuSession::WaitForBreak(const json& args)
{
	std::string error;
	Debugger* dbg = EnsureDebugger(json{}, error);
	if(!dbg) { return ErrorResult(error); }
	uint32_t timeoutMs = args.value("timeout_ms", 5000);

	uint64_t seq = _bridge->CurrentSequence();
	NotificationBridge::BreakInfo info = _bridge->WaitForBreak(seq, timeoutMs);

	json result;
	result["stopped"] = info.Valid;
	if(info.Valid) {
		result["breakpoint_id"] = info.BreakpointId;
		result["cpu"] = CpuTypeToString(info.SourceCpu);
		result["source"] = BreakSourceToString(info.Source);
		result["cpu_state"] = SerializeCpuState(dbg, info.SourceCpu);
	}
	return json{ {"result", result} };
}

json EmuSession::GetCallstack(const json& args)
{
	std::string error;
	Debugger* dbg = EnsureDebugger(args, error);
	if(!dbg) { return ErrorResult(error); }
	CpuType cpu = ResolveCpu(args, error);
	if(!error.empty()) { return ErrorResult(error); }

	StackFrameInfo frames[128];
	uint32_t size = 0;
	dbg->GetCallstackManager(cpu)->GetCallstack(frames, size);

	json framesJson = json::array();
	for(uint32_t i = 0; i < size && i < 128; i++) {
		framesJson.push_back(json{
			{"source", frames[i].Source},
			{"target", frames[i].Target},
			{"return", frames[i].Return}
		});
	}
	return json{ {"result", json{ {"cpu", CpuTypeToString(cpu)}, {"frames", framesJson} }} };
}

json EmuSession::Trace(const json& args)
{
	std::string error;
	Debugger* dbg = EnsureDebugger(args, error);
	if(!dbg) { return ErrorResult(error); }
	CpuType cpu = ResolveCpu(args, error);
	if(!error.empty()) { return ErrorResult(error); }

	bool enable = args.value("enable", true);
	TraceLoggerOptions options = {};
	options.Enabled = enable;
	options.IndentCode = args.value("indent", false);
	options.UseLabels = args.value("use_labels", true);
	std::string format = args.value("format", "[PC] [Disassembly]");
	std::string condition = args.value("condition", "");
	strncpy(options.Format, format.c_str(), sizeof(options.Format) - 1);
	strncpy(options.Condition, condition.c_str(), sizeof(options.Condition) - 1);
	dbg->GetTraceLogger(cpu)->SetOptions(options);

	//Optionally advance frames while tracing, then fetch the most recent rows
	uint32_t frames = args.value("run_frames", 0);
	if(frames > 0) {
		json run = RunFrames(json{ {"frames", frames}, {"timeout_ms", args.value("timeout_ms", 10000)} });
		if(run.contains("error")) {
			return run;
		}
	}

	uint32_t rowCount = std::min(args.value("rows", 50u), 500u);
	std::vector<TraceRow> traceRows(rowCount);
	uint32_t available = dbg->GetExecutionTrace(traceRows.data(), 0, rowCount);

	json rows = json::array();
	for(uint32_t i = 0; i < available && i < rowCount; i++) {
		rows.push_back(traceRows[i].LogOutput);
	}
	return json{ {"result", json{
		{"enabled", enable},
		{"cpu", CpuTypeToString(cpu)},
		{"rows", rows},
		{"row_count", available}
	}} };
}

json EmuSession::GetDebuggerStatus(const json& args)
{
	std::string error;
	Debugger* dbg = GetDebuggerOrNull();
	json result;
	result["debugger_started"] = dbg != nullptr;
	if(dbg) {
		result["execution_stopped"] = dbg->IsExecutionStopped();
		result["last_break"] = BreakSummary(dbg)["last_break"];

		json breakpoints = json::array();
		for(const BreakpointSpec& spec : _breakpoints) {
			breakpoints.push_back(json{
				{"id", spec.id},
				{"address", spec.startAddress},
				{"end_address", spec.endAddress},
				{"access", spec.access},
				{"memory_type", MemoryTypeToName(spec.memoryType)},
				{"cpu", CpuTypeToString(spec.cpuType)},
				{"condition", spec.condition},
				{"enabled", spec.enabled}
			});
		}
		result["breakpoints"] = breakpoints;

		json cpus = json::array();
		for(CpuType cpu : _emu->GetCpuTypes()) {
			cpus.push_back(CpuTypeToString(cpu));
		}
		result["available_cpus"] = cpus;

		json memTypes = json::array();
		for(auto& [name, type] : GetAvailableMemoryTypes(dbg)) {
			memTypes.push_back(name);
		}
		result["available_memory_types"] = memTypes;
	}
	return json{ {"result", result} };
}
