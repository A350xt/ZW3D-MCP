#include "mcp.h"
#include "../errors.h"
#include "../plugin_info.h"
#include "tools.h"

namespace mcp {
namespace {
Executor g_exec;

json rpc_error(const json& id, int code, const std::string& message) {
  return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

json rpc_result(const json& id, json result) { return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}}; }

json text_content(const std::string& text) { return json::array({{{"type", "text"}, {"text", text}}}); }

json error_result(const std::string& text) { return {{"content", text_content(text)}, {"isError", true}}; }

std::string dump(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

json handle_initialize(const json& params) {
  static const char* supported[] = {"2025-06-18", "2025-03-26", "2024-11-05"};
  std::string wanted = params.is_object() && params.contains("protocolVersion") && params["protocolVersion"].is_string() ? params["protocolVersion"].get<std::string>() : "";
  std::string version = supported[0];
  for (const char* s : supported)
    if (wanted == s) version = s;
  return {{"protocolVersion", version},
          {"capabilities", {{"tools", {{"listChanged", false}}}, {"prompts", {{"listChanged", false}}}}},
          {"serverInfo", {{"name", "zw3d-mcp"}, {"version", ZW3DMCP_VERSION}}},
          {"instructions", kInstructions}};
}

json handle_tools_list() {
  json list = json::array();
  for (const auto& t : tools()) list.push_back({{"name", t.name}, {"description", t.description}, {"inputSchema", t.input_schema}});
  return {{"tools", list}};
}

// Returns a CallToolResult. Tool failures are results with isError=true (so the AI can read code + hint); only an unknown tool is a protocol error.
json call_tool(const json& params, bool* unknown, std::string* unknown_name) {
  *unknown = false;
  std::string name = params.is_object() && params.contains("name") && params["name"].is_string() ? params["name"].get<std::string>() : "";
  const Tool* tool = find_tool(name);
  if (!tool) {
    *unknown = true;
    *unknown_name = name;
    return {};
  }
  json args = params.contains("arguments") && params["arguments"].is_object() ? params["arguments"] : json::object();
  try {
    ToolCall c = tool->prepare(args);
    if (!g_exec) return error_result(format_error("INTERNAL", "plugin executor not ready", nullptr, ""));
    json env = g_exec(c.method, c.params, c.timeout_ms);
    if (env.value("ok", false)) {
      json result = env.contains("result") ? env["result"] : json::object();
      if (c.screenshot) return {{"content", json::array({screenshot_content(result)})}, {"isError", false}};
      return {{"content", text_content(dump(result))}, {"isError", false}};
    }
    json err = env.contains("error") && env["error"].is_object() ? env["error"] : json::object();
    return error_result(format_error(err.value("code", std::string("UNKNOWN")), err.value("message", std::string("unknown error")),
                                     err.contains("api_code") ? err["api_code"] : json(nullptr), err.value("hint", std::string())));
  } catch (const ApiError& e) {
    return error_result(format_error(e.code, e.what(), e.api_code ? json(e.api_code) : json(nullptr), e.hint));
  } catch (const std::exception& e) {
    return error_result(format_error("INTERNAL", e.what(), nullptr, ""));
  }
}
}  // namespace

void set_executor(Executor e) { g_exec = std::move(e); }

json handle_message(const json& msg) {
  if (!msg.is_object()) return rpc_error(nullptr, -32600, "Invalid Request");
  bool has_id = msg.contains("id") && !msg["id"].is_null();
  json id = has_id ? msg["id"] : json(nullptr);
  if (!msg.contains("method") || !msg["method"].is_string()) {
    if (msg.contains("result") || msg.contains("error")) return json();  // a response from the client: nothing to answer
    return rpc_error(id, -32600, "Invalid Request");
  }
  std::string method = msg["method"].get<std::string>();
  json params = msg.contains("params") ? msg["params"] : json::object();
  if (!has_id) return json();  // notifications (notifications/initialized, notifications/cancelled, ...)
  try {
    if (method == "initialize") return rpc_result(id, handle_initialize(params));
    if (method == "ping") return rpc_result(id, json::object());
    if (method == "tools/list") return rpc_result(id, handle_tools_list());
    if (method == "tools/call") {
      bool unknown = false;
      std::string name;
      json r = call_tool(params, &unknown, &name);
      if (unknown) return rpc_error(id, -32602, "Unknown tool: " + name);
      return rpc_result(id, r);
    }
    if (method == "resources/list") return rpc_result(id, {{"resources", json::array()}});
    if (method == "resources/templates/list") return rpc_result(id, {{"resourceTemplates", json::array()}});
    if (method == "prompts/list")
      return rpc_result(id, {{"prompts", json::array({{{"name", "zw3d_guide"},
                                                       {"description", "How to model in ZW3D with these tools: workflow, tool choice, and rules that avoid wasted calls"}}})}});
    if (method == "prompts/get") {
      std::string pname = params.is_object() && params.contains("name") && params["name"].is_string() ? params["name"].get<std::string>() : "";
      if (pname != "zw3d_guide") return rpc_error(id, -32602, "Unknown prompt: " + pname);
      return rpc_result(id, {{"description", "ZW3D modeling guide"},
                             {"messages", json::array({{{"role", "user"}, {"content", {{"type", "text"}, {"text", kInstructions}}}}})}});
    }
    if (method == "logging/setLevel") return rpc_result(id, json::object());
    return rpc_error(id, -32601, "Method not found: " + method);
  } catch (const std::exception& e) {
    return rpc_error(id, -32603, std::string("Internal error: ") + e.what());
  }
}

}  // namespace mcp
