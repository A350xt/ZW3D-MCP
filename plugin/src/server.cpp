#include "server.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include "dispatcher.h"

static const size_t kMaxLine = 16u * 1024 * 1024;

Server& server() {
  static Server s;
  return s;
}

static bool send_all(SOCKET s, const std::string& data) {
  size_t off = 0;
  while (off < data.size()) {
    int n = send(s, data.data() + off, (int)(data.size() - off), 0);
    if (n <= 0) return false;
    off += (size_t)n;
  }
  return true;
}

bool Server::Start(int port, std::string* error) {
  if (running_) return true;
  WSADATA wd;
  if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) {
    if (error) *error = "WSAStartup failed";
    return false;
  }
  wsa_ = true;
  SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (ls == INVALID_SOCKET) {
    if (error) *error = "socket() failed";
    WSACleanup();
    wsa_ = false;
    return false;
  }
  BOOL one = TRUE;
  setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons((u_short)port);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);  // loopback only, never 0.0.0.0
  if (bind(ls, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(ls, 4) != 0) {
    if (error) *error = "cannot bind 127.0.0.1:" + std::to_string(port) + " (port in use? set ZW3D_MCP_PORT)";
    closesocket(ls);
    WSACleanup();
    wsa_ = false;
    return false;
  }
  listen_ = ls;
  port_ = port;
  stop_ = false;
  running_ = true;
  thread_ = std::thread(&Server::Loop, this);
  return true;
}

void Server::Stop() {
  if (!running_) return;
  stop_ = true;
  if (thread_.joinable()) thread_.join();
  running_ = false;
  if (listen_ != ~0ULL) {
    closesocket((SOCKET)listen_);
    listen_ = ~0ULL;
  }
  if (wsa_) {
    WSACleanup();
    wsa_ = false;
  }
}

void Server::Loop() {
  SOCKET ls = (SOCKET)listen_;
  SOCKET client = INVALID_SOCKET;
  std::string buf;
  while (!stop_) {
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(ls, &rd);
    if (client != INVALID_SOCKET) FD_SET(client, &rd);
    timeval tv{0, 200 * 1000};
    int r = select(0, &rd, nullptr, nullptr, &tv);
    if (r <= 0) continue;
    if (FD_ISSET(ls, &rd)) {  // new connection replaces the old one
      SOCKET c = accept(ls, nullptr, nullptr);
      if (c != INVALID_SOCKET) {
        if (client != INVALID_SOCKET) closesocket(client);
        client = c;
        buf.clear();
        BOOL nd = TRUE;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (const char*)&nd, sizeof(nd));
      }
    }
    if (client != INVALID_SOCKET && FD_ISSET(client, &rd)) {
      char tmp[65536];
      int n = recv(client, tmp, sizeof(tmp), 0);
      if (n <= 0) {
        closesocket(client);
        client = INVALID_SOCKET;
        buf.clear();
        continue;
      }
      buf.append(tmp, (size_t)n);
      if (buf.size() > kMaxLine && buf.find('\n') == std::string::npos) {  // oversized frame: drop the client
        closesocket(client);
        client = INVALID_SOCKET;
        buf.clear();
        continue;
      }
      size_t pos;
      while (client != INVALID_SOCKET && (pos = buf.find('\n')) != std::string::npos) {
        std::string line = buf.substr(0, pos);
        buf.erase(0, pos + 1);
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        HandleLine((unsigned long long)client, line);
      }
    }
  }
  if (client != INVALID_SOCKET) closesocket(client);
}

void Server::HandleLine(unsigned long long client, const std::string& line) {
  json id = nullptr;
  json resp;
  json req = json::parse(line, nullptr, /*allow_exceptions=*/false);
  if (req.is_discarded() || !req.is_object()) {
    resp = {{"ok", false}, {"error", {{"code", "PARSE_ERROR"}, {"api_code", nullptr}, {"message", "请求不是合法 JSON 对象"}, {"hint", ""}}}};
  } else {
    id = req.value("id", json(nullptr));
    std::string method = req.value("method", std::string());
    json params = req.contains("params") && req["params"].is_object() ? req["params"] : json::object();
    int timeout_ms = req.value("timeout_ms", 60000);
    if (timeout_ms < 100) timeout_ms = 100;
    if (method.empty())
      resp = {{"ok", false}, {"error", {{"code", "INVALID_INPUT"}, {"api_code", nullptr}, {"message", "缺少 method"}, {"hint", ""}}}};
    else
      resp = dispatcher().Execute(method, params, timeout_ms);
  }
  resp["id"] = id;
  std::string out = resp.dump(-1, ' ', false, json::error_handler_t::replace);
  out.push_back('\n');
  send_all((SOCKET)client, out);
}
