//MesenMCP - MCP server over stdio
//
//JSON-RPC 2.0, newline-delimited, per the MCP stdio transport. stdout carries
//ONLY protocol messages; diagnostics go to stderr and the session log file.
#pragma once
#include "Mcp/EmuSession.h"
#include "Mcp/ToolRegistry.h"

#include <istream>
#include <ostream>

class McpServer
{
public:
	McpServer(EmuSession& session);

	//Run the stdio loop until EOF on input. Returns a process exit code.
	int Run(std::istream& input, std::ostream& output);

	//Handle a single incoming message; returns a response to write, or an
	//empty json object (null) for notifications. Exposed for testing.
	json HandleMessage(const json& message);

private:
	json HandleRequest(const std::string& method, const json& id, const json& params);
	json MakeError(const json& id, int code, const std::string& message) const;

	EmuSession& _session;
	ToolRegistry _tools;

	std::string _protocolVersion = "2025-06-18";
	bool _initialized = false;
};
