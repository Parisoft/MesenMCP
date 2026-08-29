//MesenMCP - tool registry
//
//Maps MCP tool names to JSON schemas and handlers backed by EmuSession.
//The descriptions here are the agent-facing documentation - they are what an
//LLM client sees in tools/list, so they must explain semantics precisely.
#pragma once
#include "Mcp/EmuSession.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

struct ToolDefinition
{
	std::string name;
	std::string description;
	json inputSchema;
	std::function<json(const json& args)> handler;
};

class ToolRegistry
{
public:
	explicit ToolRegistry(EmuSession& session);

	//Returns the tools/list payload
	json GetToolsList() const;

	//Returns the tools/call payload for one call (never throws)
	json CallTool(const std::string& name, const json& args) const;

private:
	void Register(ToolDefinition tool);

	EmuSession& _session;
	std::vector<ToolDefinition> _tools;
};
