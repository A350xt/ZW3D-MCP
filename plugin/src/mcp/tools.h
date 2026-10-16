#pragma once
// The MCP tool table: name, description, JSON Schema and the validation/translation that turns a tool call into a plugin method call.
#include <functional>
#include <string>
#include <vector>
#include "json.hpp"

namespace mcp {
using json = nlohmann::json;

struct ToolCall {
  std::string method;       // plugin handler to run on ZW3D's main thread
  json params = json::object();
  int timeout_ms = 60000;
  bool screenshot = false;  // reply carries a temp file path that must be turned into an image
};

struct Tool {
  std::string name;
  std::string description;
  json input_schema;
  std::function<ToolCall(const json& arguments)> prepare;  // validates; throws ApiError(INVALID_INPUT)
};

const std::vector<Tool>& tools();
const Tool* find_tool(const std::string& name);
extern const char* const kInstructions;

// "[CODE] message (api_code=N) Hint: ..." as shown to the AI.
std::string format_error(const std::string& code, const std::string& message, const json& api_code, const std::string& hint);

// Turns the plugin's reply of a screenshot call into an MCP image content item (reads, then always deletes, the temp file).
// Throws ApiError if the path is not one of our own shot_*.png files.
json screenshot_content(const json& plugin_result);
}  // namespace mcp
