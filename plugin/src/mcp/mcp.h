#pragma once
// MCP (JSON-RPC 2.0) protocol layer: initialize, ping, tools/list, tools/call. Transport-agnostic.
#include <functional>
#include <string>
#include "json.hpp"

namespace mcp {
using json = nlohmann::json;

// Runs a plugin method on ZW3D's main thread and returns the envelope {"ok":true,"result":..} / {"ok":false,"error":{..}}.
using Executor = std::function<json(const std::string& method, const json& params, int timeout_ms)>;
void set_executor(Executor e);

// Handles one JSON-RPC message. Returns the response, or a null json for notifications (no response expected).
json handle_message(const json& message);
}  // namespace mcp
