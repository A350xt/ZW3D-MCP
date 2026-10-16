#pragma once
#include <atomic>
#include <string>
#include <thread>

// MCP over HTTP ("Streamable HTTP", JSON responses only): POST http://127.0.0.1:<port>/mcp with a JSON-RPC message.
// Loopback only. Host and Origin headers are checked (blocks DNS-rebinding / cross-site requests from web pages) and an optional
// bearer token (env ZW3D_MCP_TOKEN) is enforced. The server thread never touches the ZW3D API: tool calls go through the Dispatcher.
class Server {
 public:
  bool Start(int port, std::string* error);
  void Stop();  // joins the thread
  bool running() const { return running_; }
  int port() const { return port_; }

 private:
  void Loop();
  void Serve(unsigned long long client);

  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_{false};
  int port_ = 0;
  std::string token_;
  unsigned long long listen_ = ~0ULL;
  bool wsa_ = false;
};

Server& server();

// Exposed for tests: decides whether a request's Host/Origin/Authorization headers are acceptable. Returns 0 if ok, else the HTTP status.
int check_request_headers(int port, const std::string& host, const std::string& origin, const std::string& authorization, const std::string& token);
