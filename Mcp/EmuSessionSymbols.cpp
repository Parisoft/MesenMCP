//MesenMCP - ca65 .dbg symbol/line tools
#include "Mcp/EmuSession.h"

#include "Core/Shared/DebuggerRequest.h"
#include "Core/Shared/RomInfo.h"
#include "Core/Debugger/Debugger.h"
#include "Core/Debugger/LabelManager.h"
#include "Core/Debugger/DebugBreakHelper.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace fs = std::filesystem;

namespace
{
	json ErrorResult(const std::string& message)
	{
		return json{ {"error", message} };
	}

	MemoryType GetPrgRomMemoryType(ConsoleType console)
	{
		switch(console) {
			case ConsoleType::Nes: return MemoryType::NesPrgRom;
			case ConsoleType::Snes: return MemoryType::SnesPrgRom;
			case ConsoleType::Gameboy: return MemoryType::GbPrgRom;
			case ConsoleType::Gba: return MemoryType::GbaPrgRom;
			case ConsoleType::PcEngine: return MemoryType::PcePrgRom;
			case ConsoleType::Sms: return MemoryType::SmsPrgRom;
			case ConsoleType::Ws: return MemoryType::WsPrgRom;
			default: return MemoryType::NesPrgRom;
		}
	}

	std::string ToHex(int64_t value)
	{
		char buf[16];
		snprintf(buf, sizeof(buf), "$%04llX", (unsigned long long)(value < 0 ? 0 : value));
		return std::string(buf);
	}

	std::string Basename(const std::string& path)
	{
		size_t slash = path.find_last_of("/\\");
		return slash == std::string::npos ? path : path.substr(slash + 1);
	}

	std::string Lower(std::string s)
	{
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
		return s;
	}
}

//--- .dbg loading -------------------------------------------------------------

json EmuSession::LoadDbgFile(const json& args)
{
	if(!IsRomLoaded()) {
		return ErrorResult("no ROM is loaded (load the ROM built with the .dbg file first)");
	}
	if(!args.contains("path") || !args["path"].is_string()) {
		return ErrorResult("missing required string argument: path (the ca65 .dbg file)");
	}
	std::string path = args["path"].get<std::string>();
	if(!fs::is_regular_file(path)) {
		return ErrorResult(".dbg file not found: " + path);
	}

	std::string error;
	Debugger* dbg = EnsureDebugger(args, error);
	if(!dbg) { return ErrorResult(error); }

	std::unique_ptr<DbgData> data(new DbgData());
	if(!data->LoadFromFile(path, error)) {
		return ErrorResult(error);
	}

	CpuType cpu = ResolveCpu(args, error);
	if(!error.empty()) { return ErrorResult(error); }
	MemoryType cpuMem = DebugUtilities::GetCpuMemoryType(cpu);
	MemoryType prgMem = GetPrgRomMemoryType(_emu->GetConsoleType());
	uint32_t headerSize = _emu->GetRomInfo().Format == RomFormat::iNes ? 16 : 0;

	//Resolution reads live mapper state (GetRelativeAddress walks the memory
	//mappings) - park the CPU at an instruction boundary while it runs, exactly
	//like the core's own debug helpers do. Resumes on scope exit.
	DebugBreakHelper breakHelper(dbg);

	//Resolve labels: RAM labels use their CPU-bus address directly; code labels
	//map val -> PRG-ROM offset -> (via the debugger) back to a CPU-bus address.
	uint32_t codeLabels = 0, ramLabels = 0;
	std::unordered_map<uint32_t, DbgData::Line*> lineById;
	for(auto& line : data->lines) {
		lineById[line.id] = &line;
	}

	for(const DbgData::Symbol& sym : data->symbolsRaw) {
		if(sym.val < 0 || sym.name.empty()) {
			continue;
		}
		DbgData::Label label;
		label.name = sym.name;
		label.size = sym.size;

		//Definition file/line (first definition with a known line entry)
		for(uint32_t def : sym.defs) {
			auto it = lineById.find(def);
			if(it != lineById.end()) {
				auto fileIt = data->fileById.find(it->second->file);
				if(fileIt != data->fileById.end()) {
					label.file = data->files[fileIt->second].name;
					label.line = it->second->line;
				}
				break;
			}
		}

		auto segIt = sym.seg >= 0 ? data->segmentById.find((uint32_t)sym.seg) : data->segmentById.end();
		if(segIt != data->segmentById.end()) {
			const DbgData::Segment& seg = data->segments[segIt->second];
			if(seg.isRam) {
				label.isRam = true;
				label.cpuAddress = sym.val;
				label.prgAddress = -1;
			} else {
				int64_t prg = sym.val - seg.start + seg.fileOffset - headerSize;
				if(prg >= 0) {
					label.prgAddress = prg;
					AddressInfo rel = dbg->GetRelativeAddress({ (int32_t)prg, prgMem }, cpu);
					label.cpuAddress = rel.Address;
					codeLabels++;

					//Register on PRG-ROM so labels resolve for disassembly/expressions
					dbg->GetLabelManager()->SetLabel((uint32_t)prg, prgMem, label.name, "");
				} else {
					continue;
				}
			}
		} else {
			continue;
		}

		if(!label.isRam) {
			//already registered above
		} else {
			dbg->GetLabelManager()->SetLabel((uint32_t)label.cpuAddress, cpuMem, label.name, "");
			ramLabels++;
		}
		data->labels.push_back(label);
	}

	//Resolve line -> code ranges (from each line's spans) and build the
	//address -> line reverse map used to annotate disassembly
	for(auto& line : data->lines) {
		auto fileIt = data->fileById.find(line.file);
		if(fileIt == data->fileById.end()) {
			continue;
		}
		auto key = std::make_pair(fileIt->second, line.line);
		for(uint32_t spanId : line.spans) {
			auto spanIt = data->spanById.find(spanId);
			if(spanIt == data->spanById.end()) {
				continue;
			}
			const DbgData::Span& span = data->spansRaw[spanIt->second];
			auto segIt = data->segmentById.find(span.seg);
			if(segIt == data->segmentById.end()) {
				continue;
			}
			const DbgData::Segment& seg = data->segments[segIt->second];
			if(seg.isRam || span.size == seg.size) {
				//Skip RAM and whole-segment spans (same rule as the GUI importer)
				continue;
			}
			int64_t prgStart = span.start + seg.fileOffset - headerSize;
			int64_t prgEnd = prgStart + span.size - 1;
			if(prgStart < 0) {
				continue;
			}

			DbgData::LineRange* range;
			auto rangeIt = data->lineRangeIndex.find(key);
			if(rangeIt == data->lineRangeIndex.end()) {
				DbgData::LineRange newRange;
				newRange.file = fileIt->second;
				newRange.line = line.line;
				newRange.isData = span.isData;
				data->lineRanges.push_back(newRange);
				data->lineRangeIndex[key] = data->lineRanges.size() - 1;
				range = &data->lineRanges.back();
			} else {
				range = &data->lineRanges[rangeIt->second];
				range->isData = range->isData && span.isData;
			}
			range->prgStart = range->prgStart < 0 ? prgStart : std::min(range->prgStart, prgStart);
			range->prgEnd = std::max(range->prgEnd, prgEnd);

			//CPU-bus range (for breakpoints): map the endpoints through the debugger
			AddressInfo relStart = dbg->GetRelativeAddress({ (int32_t)prgStart, prgMem }, cpu);
			AddressInfo relEnd = dbg->GetRelativeAddress({ (int32_t)prgEnd, prgMem }, cpu);
			if(relStart.Address >= 0) {
				range->cpuStart = range->cpuStart < 0 ? relStart.Address : std::min(range->cpuStart, (int64_t)relStart.Address);
				int64_t cpuEnd = relEnd.Address >= relStart.Address ? relEnd.Address : relStart.Address;
				range->cpuEnd = std::max(range->cpuEnd, cpuEnd);

				for(int64_t addr = relStart.Address; addr <= cpuEnd; addr++) {
					data->addrToLine[(uint32_t)addr] = { fileIt->second, line.line };
				}
			}
		}
	}

	json result;
	result["path"] = path;
	result["source_files"] = data->files.size();
	result["labels"] = data->labels.size();
	result["code_labels"] = codeLabels;
	result["ram_labels"] = ramLabels;
	result["lines_with_code"] = data->lineRanges.size();
	result["note"] = "labels are registered for expressions/disassembly; use "
		"find_labels to locate symbols and set_breakpoint with label=/file=+line= "
		"to break on source code";
	_dbg = std::move(data);
	return json{ {"result", result} };
}

//--- Label lookup -------------------------------------------------------------

DbgData::Label* EmuSession::FindDbgLabel(const std::string& nameIn)
{
	if(!_dbg) {
		return nullptr;
	}
	DbgData::Label* ciMatch = nullptr;
	for(auto& label : _dbg->labels) {
		if(label.name == nameIn) {
			return &label;
		}
		if(!ciMatch && Lower(label.name) == Lower(nameIn)) {
			ciMatch = &label;
		}
	}
	return ciMatch;
}

json EmuSession::FindLabels(const json& args)
{
	if(!_dbg) {
		return ErrorResult("no .dbg file is loaded - call load_dbg_file first");
	}
	if(!args.contains("query") || !args["query"].is_string()) {
		return ErrorResult("missing required string argument: query (label name or substring)");
	}
	std::string query = args["query"].get<std::string>();
	bool exact = args.value("exact", false);
	uint32_t limit = args.value("limit", 50);

	std::string queryLower = Lower(query);
	json matches = json::array();
	uint32_t count = 0;
	for(auto& label : _dbg->labels) {
		bool match;
		if(exact) {
			match = Lower(label.name) == queryLower;
		} else {
			match = Lower(label.name).find(queryLower) != std::string::npos;
		}
		if(!match) {
			continue;
		}
		json entry;
		entry["name"] = label.name;
		if(label.cpuAddress >= 0) {
			entry["address"] = label.cpuAddress;
			entry["address_hex"] = ToHex(label.cpuAddress);
		}
		if(label.prgAddress >= 0) {
			entry["prg_address"] = label.prgAddress;
		}
		entry["memory_type"] = label.isRam ? "cpu" : "prg_rom";
		entry["ram"] = label.isRam;
		if(label.size > 1) {
			entry["size"] = label.size;
		}
		if(!label.file.empty()) {
			entry["file"] = label.file;
			entry["line"] = label.line + 1; //1-based for humans
		}
		matches.push_back(entry);
		count++;
		if(count >= limit) {
			break;
		}
	}

	json result;
	result["matches"] = matches;
	result["match_count"] = count;
	result["truncated"] = count >= limit;
	return json{ {"result", result} };
}

json EmuSession::ListSourceFiles(const json& args)
{
	if(!_dbg) {
		return ErrorResult("no .dbg file is loaded - call load_dbg_file first");
	}
	std::map<uint32_t, uint32_t> linesPerFile;
	for(auto& range : _dbg->lineRanges) {
		linesPerFile[range.file]++;
	}

	json list = json::array();
	for(size_t i = 0; i < _dbg->files.size(); i++) {
		json entry;
		entry["file"] = _dbg->files[i].name;
		entry["basename"] = Basename(_dbg->files[i].name);
		uint32_t lines = 0;
		auto it = linesPerFile.find((uint32_t)i);
		if(it != linesPerFile.end()) {
			lines = it->second;
		}
		entry["lines_with_code"] = lines;
		if(lines == 0) {
			entry["note"] = "no code spans (header/data file?)";
		}
		list.push_back(entry);
	}
	return json{ {"result", json{ {"files", list} }} };
}

//--- Source-line breakpoint resolution ----------------------------------------

bool EmuSession::ResolveDbgSourceRange(const json& args, int32_t& startAddress,
	int32_t& endAddress, MemoryType& memType, std::string& description, std::string& error)
{
	if(!_dbg) {
		error = "no .dbg file is loaded - call load_dbg_file first";
		return false;
	}
	if(!args.contains("line")) {
		error = "missing required argument: line (1-based source line number)";
		return false;
	}

	std::string fileFilter = args.value("file", "");
	uint32_t line1 = args["line"].get<uint32_t>();
	uint32_t endLine1 = args.value("end_line", line1);

	//Resolve the file: exact name, basename, or unique substring
	int64_t fileIdx = -1;
	if(!fileFilter.empty()) {
		std::string filterLower = Lower(fileFilter);
		bool ambiguous = false;
		for(size_t i = 0; i < _dbg->files.size(); i++) {
			const std::string& name = _dbg->files[i].name;
			bool match = Lower(name) == filterLower || Lower(Basename(name)) == filterLower
				|| Lower(name).find(filterLower) != std::string::npos;
			if(match) {
				if(fileIdx >= 0) { ambiguous = true; break; }
				fileIdx = (int64_t)i;
			}
		}
		if(ambiguous) {
			error = "file filter '" + fileFilter + "' matches multiple source files - use the full name (see list_source_files)";
			return false;
		}
		if(fileIdx < 0) {
			error = "source file not found in the .dbg data: " + fileFilter;
			return false;
		}
	}

	//Find line ranges covering line1..endLine1
	int64_t prgStart = -1, prgEnd = -1, cpuStart = -1, cpuEnd = -1;
	bool found = false;
	std::string foundFile;
	for(uint32_t l = line1; l <= endLine1; l++) {
		auto it = _dbg->lineRangeIndex.find({ (uint32_t)std::max<int64_t>(fileIdx, 0), l - 1 });
		if(fileIdx < 0) {
			//Search every file for this line
			for(size_t f = 0; f < _dbg->files.size(); f++) {
				it = _dbg->lineRangeIndex.find({ (uint32_t)f, l - 1 });
				if(it != _dbg->lineRangeIndex.end()) {
					break;
				}
			}
		}
		if(it == _dbg->lineRangeIndex.end()) {
			continue;
		}
		const DbgData::LineRange& range = _dbg->lineRanges[it->second];
		found = true;
		foundFile = _dbg->files[range.file].name;
		if(range.prgStart >= 0) {
			prgStart = prgStart < 0 ? range.prgStart : std::min(prgStart, range.prgStart);
			prgEnd = std::max(prgEnd, range.prgEnd);
		}
		if(range.cpuStart >= 0) {
			cpuStart = cpuStart < 0 ? range.cpuStart : std::min(cpuStart, range.cpuStart);
			cpuEnd = std::max(cpuEnd, range.cpuEnd);
		}
	}
	if(!found) {
		//Help the caller: list nearby lines that DO have code
		std::string nearby;
		uint32_t from = line1 > 5 ? line1 - 5 : 1;
		for(uint32_t l = from; l <= line1 + 5; l++) {
			for(size_t f = 0; f < _dbg->files.size(); f++) {
				if(_dbg->lineRangeIndex.count({ (uint32_t)f, l - 1 })) {
					nearby += (nearby.empty() ? "" : ", ") + std::to_string(l);
					break;
				}
			}
		}
		error = "no code found at " + (fileFilter.empty() ? std::string("line") : fileFilter + ":line")
			+ " " + std::to_string(line1) + (nearby.empty() ? " (nearby lines have no code either)" : ". Lines with code nearby: " + nearby);
		return false;
	}

	//Prefer the CPU-bus range (executable breakpoints), fall back to PRG-ROM
	MemoryType prgMem = GetPrgRomMemoryType(_emu->GetConsoleType());
	if(cpuStart >= 0) {
		startAddress = (int32_t)cpuStart;
		endAddress = (int32_t)cpuEnd;
		memType = DebugUtilities::GetCpuMemoryType(ResolveCpu(json{}, error));
	} else if(prgStart >= 0) {
		startAddress = (int32_t)prgStart;
		endAddress = (int32_t)prgEnd;
		memType = prgMem;
	} else {
		error = "the source line maps to data, not code (cannot break on it)";
		return false;
	}

	std::string fname = foundFile.empty() ? std::string("source") : Basename(foundFile);
	description = fname + ":" + std::to_string(line1) + (endLine1 != line1 ? "-" + std::to_string(endLine1) : "");
	return true;
}
