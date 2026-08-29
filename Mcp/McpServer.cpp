//MesenMCP - MCP server over stdio
#include "Mcp/McpServer.h"

#include <cstdio>
#include <iostream>
#include <string>

using json = nlohmann::json;

namespace
{
	constexpr const char* ServerName = "mesen-mcp";

	bool IsRequestId(const json& value)
	{
		return value.is_string() || value.is_number_integer();
	}
}

McpServer::McpServer(EmuSession& session)
	: _session(session), _tools(session)
{
}

json McpServer::MakeError(const json& id, int code, const std::string& message) const
{
	return json{
		{"jsonrpc", "2.0"},
		{"id", id},
		{"error", { {"code", code}, {"message", message} }}
	};
}

json McpServer::HandleMessage(const json& message)
{
	if(!message.is_object() || !message.contains("jsonrpc") || message["jsonrpc"] != "2.0" ||
		!message.contains("method") || !message["method"].is_string()) {
		if(message.is_object() && IsRequestId(message.value("id", json()))) {
			return MakeError(message["id"], -32600, "invalid request");
		}
		return json();
	}

	const std::string method = message["method"].get<std::string>();
	const bool isRequest = message.contains("id") && IsRequestId(message["id"]);
	const json params = message.value("params", json::object());

	if(!isRequest) {
		//Notifications (notifications/initialized, notifications/cancelled, ...) - ignore
		return json();
	}

	const json& id = message["id"];

	//Per spec, most requests before initialize are rejected
	if(!_initialized && method != "initialize" && method != "ping") {
		return MakeError(id, -32002, "server not initialized: send an 'initialize' request first");
	}

	return HandleRequest(method, id, params);
}

json McpServer::HandleRequest(const std::string& method, const json& id, const json& params)
{
	if(method == "initialize") {
		std::string clientVersion = params.value("protocolVersion", _protocolVersion);
		if(clientVersion != "2024-11-05" && clientVersion != "2025-03-26" && clientVersion != "2025-06-18") {
			clientVersion = _protocolVersion;
		}
		_protocolVersion = clientVersion;
		_initialized = true;
		return json{
			{"jsonrpc", "2.0"},
			{"id", id},
			{"result", {
				{"protocolVersion", _protocolVersion},
				{"capabilities", {
					{"tools", { {"listChanged", false} }}
				}},
				{"serverInfo", { {"name", ServerName}, {"version", EmuSession::Version} }},
				{"instructions",
					"Emulates NES/SNES/GBA(+) games headlessly so you can validate and debug ROMs. "
					"Typical flow: load_rom -> run_frames (60 frames = 1s of game time) -> screenshot "
					"to see the screen -> repeat. Use get_status to inspect the session. Frames run "
					"at maximum speed by default; timing within the emulated console is accurate "
					"regardless of host speed. GBA requires gba_bios.bin in the session firmware folder."}
			}}
		};
	}

	if(method == "ping") {
		return json{ {"jsonrpc", "2.0"}, {"id", id}, {"result", json::object()} };
	}

	if(method == "tools/list") {
		return json{
			{"jsonrpc", "2.0"},
			{"id", id},
			{"result", _tools.GetToolsList()}
		};
	}

	if(method == "tools/call") {
		std::string name = params.value("name", "");
		json arguments = params.value("arguments", json::object());
		if(name.empty()) {
			return MakeError(id, -32602, "missing tool name");
		}
		return json{
			{"jsonrpc", "2.0"},
			{"id", id},
			{"result", _tools.CallTool(name, arguments)}
		};
	}

	return MakeError(id, -32601, "method not found: " + method);
}

int McpServer::Run(std::istream& input, std::ostream& output)
{
	std::string line;
	while(std::getline(input, line)) {
		//Tolerate \r\n line endings
		while(!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
			line.pop_back();
		}
		if(line.empty()) {
			continue;
		}

		json message;
		try {
			message = json::parse(line);
		} catch(json::exception& e) {
			json response = MakeError(nullptr, -32700, std::string("parse error: ") + e.what());
			output << response.dump() << std::endl;
			continue;
		}

		json response = HandleMessage(message);
		if(!response.is_null()) {
			output << response.dump() << std::endl;
		}
	}

	//stdin closed - shut down cleanly (EmuSession destructor stops the emulator)
	std::fprintf(stderr, "[mesen-mcp] input closed, shutting down\n");
	return 0;
}
