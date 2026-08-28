//MesenMCP - ca65 (.dbg) debug file support
//
//Parses cc65/ca65 .dbg files (the same grammar as Mesen's GUI importer,
//UI/Debugger/Integration/DbgImporter.cs) into label and source-line tables:
//
//   seg  id=N, name="...", start=0xHEX, size=0xHEX, ooffs=DEC [, type=rw]
//   file id=N, name="path", mtime=...
//   line id=N, file=N, line=N(1-based), type=N, span=a+b+c
//   span id=N, seg=N, start=DEC(offset in segment), size=DEC [, type=N=code]
//   sym  id=N, type=lab, name="...", val=0xHEX, seg=N, size=DEC, def=a+b, ...
//
//Address math (mirrors the GUI importer): a code label's PRG-ROM offset is
//val - segment.start + segment.ooffs - headerSize (headerSize = 16 for iNES).
//A line's code range comes from its spans: prg = span.start + seg.ooffs - hdr.
#pragma once
#include "pch.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class DbgData
{
public:
	struct Segment
	{
		uint32_t id = 0;
		std::string name;
		uint32_t start = 0;
		uint32_t size = 0;
		bool isRam = true;
		uint32_t fileOffset = 0; //ooffs
	};

	struct File
	{
		uint32_t id = 0;
		std::string name;
	};

	struct Line
	{
		uint32_t id = 0;
		uint32_t file = 0;
		uint32_t line = 0;      //0-based
		std::vector<uint32_t> spans;
	};

	struct Span
	{
		uint32_t id = 0;
		uint32_t seg = 0;
		uint32_t start = 0;     //offset within the segment
		uint32_t size = 0;
		bool isData = false;
	};

	struct Symbol
	{
		uint32_t id = 0;
		std::string name;
		int64_t val = -1;       //CPU-space address, hex in the file
		int64_t seg = -1;
		int64_t size = -1;
		std::string type;       //"lab", etc.
		std::vector<uint32_t> defs;
	};

	//Resolved label (address conversion done by EmuSession, which knows the console)
	struct Label
	{
		std::string name;
		int64_t cpuAddress = -1;    //address on the CPU bus (-1 if unmapped)
		int64_t prgAddress = -1;    //offset in PRG-ROM memory (-1 for RAM labels)
		bool isRam = false;
		int64_t size = -1;
		std::string file;           //defining source file, if known
		int64_t line = -1;          //0-based defining line, if known
	};

	//Resolved source line -> code range
	struct LineRange
	{
		uint32_t file = 0;
		uint32_t line = 0;          //0-based
		int64_t prgStart = -1;
		int64_t prgEnd = -1;
		int64_t cpuStart = -1;
		int64_t cpuEnd = -1;
		bool isData = false;
	};

	std::string path;
	std::string basePath;

	std::vector<Segment> segments;
	std::vector<File> files;
	std::vector<Label> labels;
	std::vector<LineRange> lineRanges;
	std::map<std::pair<uint32_t, uint32_t>, size_t> lineRangeIndex; //(file, line0) -> lineRanges idx
	std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> addrToLine; //cpu addr -> (fileIdx, line0)

	//Raw tables used by EmuSession during resolution
	std::vector<Line> lines;
	std::unordered_map<uint32_t, size_t> spanById;
	std::vector<Span> spansRaw;
	std::unordered_map<uint32_t, size_t> segmentById;
	std::unordered_map<uint32_t, size_t> fileById;
	std::vector<Symbol> symbolsRaw;

	bool LoadFromFile(const std::string& dbgPath, std::string& error)
	{
		std::ifstream in(dbgPath);
		if(!in) {
			error = "cannot open .dbg file: " + dbgPath;
			return false;
		}
		path = dbgPath;
		size_t slash = dbgPath.find_last_of("/\\");
		basePath = slash == std::string::npos ? "." : dbgPath.substr(0, slash);

		std::string row;
		while(std::getline(in, row)) {
			if(row.empty() || row[0] == ';') {
				continue;
			}
			size_t tab = row.find('\t');
			if(tab == std::string::npos) {
				continue;
			}
			std::string type = row.substr(0, tab);
			std::string rest = row.substr(tab + 1);

			if(type == "seg") {
				ParseSegment(rest);
			} else if(type == "file") {
				ParseFile(rest);
			} else if(type == "line") {
				ParseLine(rest);
			} else if(type == "span") {
				ParseSpan(rest);
			} else if(type == "sym") {
				ParseSymbol(rest);
			}
			//"scope"/"csym" are not needed for labels/line breakpoints
		}
		return true;
	}

private:
	static std::vector<std::string> SplitFields(const std::string& rest)
	{
		std::vector<std::string> fields;
		size_t pos = 0;
		while(pos <= rest.size()) {
			size_t comma = rest.find(',', pos);
			std::string field = rest.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
			//Strip quoted sections when finding the separator is not enough: fields
			//are key=value with the value optionally in quotes (no commas inside).
			fields.push_back(field);
			if(comma == std::string::npos) {
				break;
			}
			pos = comma + 1;
		}
		return fields;
	}

	static std::string Unquote(const std::string& value)
	{
		if(value.size() >= 2 && value.front() == '"' && value.back() == '"') {
			return value.substr(1, value.size() - 2);
		}
		return value;
	}

	void ParseSegment(const std::string& rest)
	{
		Segment seg;
		bool hasStart = false, hasSize = false, hasOoffs = false;
		for(const std::string& field : SplitFields(rest)) {
			size_t eq = field.find('=');
			if(eq == std::string::npos) {
				continue;
			}
			std::string key = field.substr(0, eq);
			std::string value = field.substr(eq + 1);
			if(key == "id") { seg.id = std::stoul(value); }
			else if(key == "name") { seg.name = Unquote(value); }
			else if(key == "start") { seg.start = std::stoul(value, nullptr, 16); hasStart = true; }
			else if(key == "size") { seg.size = std::stoul(value, nullptr, 16); hasSize = true; }
			else if(key == "ooffs") { seg.fileOffset = std::stoul(value); hasOoffs = true; }
			else if(key == "type" && value == "rw") { /* handled below */ }
		}
		if(!hasStart || !hasSize) {
			return;
		}
		//Segments mapped into the output file (ooffs) are code/data in ROM;
		//type=rw marks writable (RAM) segments even when they have an ooffs
		bool isRw = rest.find("type=rw") != std::string::npos;
		seg.isRam = !hasOoffs || isRw;
		segmentById[seg.id] = segments.size();
		segments.push_back(seg);
	}

	void ParseFile(const std::string& rest)
	{
		File file;
		bool hasId = false, hasName = false;
		for(const std::string& field : SplitFields(rest)) {
			size_t eq = field.find('=');
			if(eq == std::string::npos) {
				continue;
			}
			std::string key = field.substr(0, eq);
			std::string value = field.substr(eq + 1);
			if(key == "id") { file.id = std::stoul(value); hasId = true; }
			else if(key == "name") { file.name = Unquote(value); hasName = true; }
		}
		if(hasId && hasName) {
			fileById[file.id] = files.size();
			files.push_back(file);
		}
	}

	void ParseLine(const std::string& rest)
	{
		Line line;
		bool hasId = false, hasFile = false, hasLineNumber = false;
		for(const std::string& field : SplitFields(rest)) {
			size_t eq = field.find('=');
			if(eq == std::string::npos) {
				continue;
			}
			std::string key = field.substr(0, eq);
			std::string value = field.substr(eq + 1);
			if(key == "id") { line.id = std::stoul(value); hasId = true; }
			else if(key == "file") { line.file = std::stoul(value); hasFile = true; }
			else if(key == "line") {
				//1-based in the file, 0-based here (line 0 entries are clamped)
				int64_t n = std::stoll(value) - 1;
				line.line = n < 0 ? 0 : (uint32_t)n;
				hasLineNumber = true;
			} else if(key == "span") {
				for(const std::string& id : SplitPlusList(value)) {
					line.spans.push_back(std::stoul(id));
				}
			}
		}
		if(hasId && hasFile && hasLineNumber) {
			lines.push_back(line);
		}
	}

	void ParseSpan(const std::string& rest)
	{
		Span span;
		bool hasId = false, hasSeg = false, hasStart = false, hasSize = false;
		for(const std::string& field : SplitFields(rest)) {
			size_t eq = field.find('=');
			if(eq == std::string::npos) {
				continue;
			}
			std::string key = field.substr(0, eq);
			std::string value = field.substr(eq + 1);
			if(key == "id") { span.id = std::stoul(value); hasId = true; }
			else if(key == "seg") { span.seg = std::stoul(value); hasSeg = true; }
			else if(key == "start") { span.start = std::stoul(value); hasStart = true; }
			else if(key == "size") { span.size = std::stoul(value); hasSize = true; }
			else if(key == "type") { span.isData = true; }
		}
		if(hasId && hasSeg && hasStart && hasSize) {
			spanById[span.id] = spansRaw.size();
			spansRaw.push_back(span);
		}
	}

	void ParseSymbol(const std::string& rest)
	{
		Symbol sym;
		bool hasId = false, hasName = false;
		for(const std::string& field : SplitFields(rest)) {
			size_t eq = field.find('=');
			if(eq == std::string::npos) {
				continue;
			}
			std::string key = field.substr(0, eq);
			std::string value = field.substr(eq + 1);
			if(key == "id") { sym.id = std::stoul(value); hasId = true; }
			else if(key == "name") { sym.name = Unquote(value); hasName = true; }
			else if(key == "val") {
				//0x-prefixed hex; skip negative/wrapped values
				if(value.size() > 2) {
					try { sym.val = std::stoll(value.substr(2), nullptr, 16); } catch(...) {}
				}
			}
			else if(key == "seg") { sym.seg = std::stoll(value); }
			else if(key == "size") {
				try { sym.size = std::stoll(value); } catch(...) {}
			}
			else if(key == "type") { sym.type = value; }
			else if(key == "def") {
				for(const std::string& id : SplitPlusList(value)) {
					sym.defs.push_back(std::stoul(id));
				}
			}
		}
		if(hasId && hasName && !sym.name.empty()) {
			symbolsRaw.push_back(sym);
		}
	}

	static std::vector<std::string> SplitPlusList(const std::string& value)
	{
		std::vector<std::string> out;
		std::string token;
		for(char c : value) {
			if(c == '+') {
				if(!token.empty()) { out.push_back(token); token.clear(); }
			} else {
				token += c;
			}
		}
		if(!token.empty()) { out.push_back(token); }
		return out;
	}
};
