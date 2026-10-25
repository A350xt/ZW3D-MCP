// Entry points. ZW3D derives them from the DLL file name (zw3dmcp.dll -> zw3dmcpInit / zw3dmcpExit).
#include <cstdlib>
#include <string>
#include "VxApi.h"
#include "dialogs.h"
#include "dispatcher.h"
#include "handlers/handlers.h"
#include "mcp/mcp.h"
#include "plugin_info.h"
#include "server.h"

static int configured_port() {
  const char* p = std::getenv("ZW3D_MCP_PORT");
  int v = p ? std::atoi(p) : 0;
  return (v > 0 && v < 65536) ? v : ZW3DMCP_DEFAULT_PORT;
}

static bool autostart() {
  const char* a = std::getenv("ZW3D_MCP_AUTOSTART");
  return !(a && std::string(a) == "0");
}

static void say(const std::string& s) { cvxMsgDisp(s.c_str()); }

static bool start_server() {
  if (server().running()) return true;
  mcp::set_executor([](const std::string& method, const nlohmann::json& params, int timeout_ms) {
    // Dialog tools run on the calling (socket) thread: they must work while ZW3D's main thread is blocked behind a modal dialog.
    if (method == "list_dialogs") {
      auto d = find_dialogs(true);
      return nlohmann::json{{"ok", true}, {"result", {{"count", d.size()}, {"dialogs", dialogs_to_json(d)}}}};
    }
    if (method == "dismiss_dialog") {
      try {
        return nlohmann::json{{"ok", true}, {"result", dismiss_dialog(params.value("action", std::string("accept")), params.value("button", std::string()))}};
      } catch (const std::exception& e) {
        return nlohmann::json{{"ok", false}, {"error", {{"code", "NO_DIALOG"}, {"api_code", nullptr}, {"message", e.what()}, {"hint", "list_dialogs 查看当前是否有弹窗。"}}}};
      }
    }
    return dispatcher().Execute(method, params, timeout_ms);
  });
  std::string err;
  int port = configured_port();
  if (!server().Start(port, &err)) {
    say("[ZW3D-MCP] start failed: " + err);
    return false;
  }
  say("[ZW3D-MCP] MCP endpoint: http://127.0.0.1:" + std::to_string(port) + "/mcp");
  return true;
}

// Registered commands: `int Fn(int idData)` as in the vendor examples.
static int McpStart(int) {
  start_server();
  return 0;
}

static int McpStop(int) {
  // Stop() joins the socket thread, which may be waiting for this (main) thread inside Execute(): release it first.
  dispatcher().Shutdown();
  server().Stop();
  dispatcher().Resume();
  say("[ZW3D-MCP] stopped");
  return 0;
}

extern "C" __declspec(dllexport) int zw3dmcpInit(int /*format*/, void* /*data*/) {
  cvxCmdFunc("McpStart", (void*)McpStart, VX_CODE_GENERAL);
  cvxCmdFunc("McpStop", (void*)McpStop, VX_CODE_GENERAL);
  if (!dispatcher().Create()) {
    say("[ZW3D-MCP] failed to create the dispatcher window");
    return 0;
  }
  cleanup_screenshots(600);  // stale files from a previous crashed session
  if (autostart()) start_server();
  else say("[ZW3D-MCP] loaded (autostart disabled); run McpStart to listen");
  return 0;
}

extern "C" __declspec(dllexport) int zw3dmcpExit(void) {
  dispatcher().Shutdown();  // release any waiting socket thread first, so Stop() cannot block on us
  server().Stop();
  dispatcher().Destroy();
  cleanup_screenshots(-1);  // leave no screenshot files behind in %TEMP%
  cvxCmdFuncUnload("McpStart");
  cvxCmdFuncUnload("McpStop");
  return 0;
}
