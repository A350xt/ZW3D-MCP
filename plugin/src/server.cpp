#include "server.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <map>
#include "mcp/mcp.h"

static const size_t kMaxBody = 16u * 1024 * 1024;
static const size_t kMaxHeader = 64u * 1024;

Server& server() {
  static Server s;
  return s;
}

namespace {
std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  return s;
}
std::string trim(const std::string& s) {
  size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
  return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

// "host[:port]" or "[::1]:port" -> host part (lower case, brackets kept)
std::string host_of(const std::string& hostport) {
  std::string h = lower(trim(hostport));
  if (!h.empty() && h[0] == '[') {
    size_t e = h.find(']');
    return e == std::string::npos ? h : h.substr(0, e + 1);
  }
  size_t c = h.find(':');
  return c == std::string::npos ? h : h.substr(0, c);
}
bool is_loopback_host(const std::string& h) { return h == "127.0.0.1" || h == "localhost" || h == "[::1]"; }

const char* reason(int status) {
  switch (status) {
    case 200: return "OK";
    case 202: return "Accepted";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 415: return "Unsupported Media Type";
    default: return "Error";
  }
}

bool send_all(SOCKET s, const std::string& data) {
  size_t off = 0;
  while (off < data.size()) {
    int n = send(s, data.data() + off, (int)(data.size() - off), 0);
    if (n <= 0) return false;
    off += (size_t)n;
  }
  return true;
}

void respond(SOCKET s, int status, const std::string& body, const std::string& extra_headers = "") {
  std::string head = "HTTP/1.1 " + std::to_string(status) + " " + reason(status) + "\r\n";
  if (!body.empty()) head += "Content-Type: application/json\r\n";
  head += "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\nCache-Control: no-store\r\n" + extra_headers + "\r\n";
  send_all(s, head + body);
}

std::string jsonrpc_error_body(int code, const std::string& msg) {
  return nlohmann::json{{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", code}, {"message", msg}}}}.dump();
}

// Reads from the socket until `pred(buf)` is true, the peer closes, the deadline passes, or `stop` is set.
bool read_until(SOCKET s, std::string& buf, size_t limit, std::chrono::steady_clock::time_point deadline, const std::atomic<bool>& stop,
                const std::function<bool(const std::string&)>& pred) {
  while (!pred(buf)) {
    if (stop || std::chrono::steady_clock::now() > deadline || buf.size() > limit) return false;
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(s, &rd);
    timeval tv{0, 200 * 1000};
    int r = select(0, &rd, nullptr, nullptr, &tv);
    if (r < 0) return false;
    if (r == 0) continue;
    char tmp[65536];
    int n = recv(s, tmp, sizeof(tmp), 0);
    if (n <= 0) return false;
    buf.append(tmp, (size_t)n);
  }
  return true;
}

// Decodes a chunked body that starts at `buf`; returns false if incomplete.
bool decode_chunked(const std::string& buf, std::string* out, bool* complete) {
  out->clear();
  *complete = false;
  size_t pos = 0;
  for (;;) {
    size_t eol = buf.find("\r\n", pos);
    if (eol == std::string::npos) return true;
    long len = std::strtol(buf.substr(pos, eol - pos).c_str(), nullptr, 16);
    if (len < 0) return false;
    pos = eol + 2;
    if (len == 0) {
      *complete = buf.find("\r\n", pos) != std::string::npos;
      return true;
    }
    if (buf.size() < pos + (size_t)len + 2) return true;
    out->append(buf, pos, (size_t)len);
    pos += (size_t)len + 2;
  }
}
}  // namespace

int check_request_headers(int port, const std::string& host, const std::string& origin, const std::string& authorization, const std::string& token) {
  (void)port;
  if (host.empty() || host.find('@') != std::string::npos || !is_loopback_host(host_of(host))) return 403;  // DNS rebinding: the Host must name the loopback interface
  if (!origin.empty()) {
    // Origin: scheme://host[:port]. Anything else (including "null") is a foreign page.
    size_t se = origin.find("://");
    if (se == std::string::npos) return 403;
    std::string rest = origin.substr(se + 3);
    size_t slash = rest.find('/');
    if (slash != std::string::npos) rest = rest.substr(0, slash);
    if (rest.find('@') != std::string::npos || !is_loopback_host(host_of(rest))) return 403;
  }
  if (!token.empty() && trim(authorization) != "Bearer " + token) return 401;
  return 0;
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
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons((u_short)port);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);  // loopback only, never 0.0.0.0
  if (bind(ls, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(ls, 8) != 0) {
    if (error) *error = "cannot bind 127.0.0.1:" + std::to_string(port) + " (port in use? set ZW3D_MCP_PORT)";
    closesocket(ls);
    WSACleanup();
    wsa_ = false;
    return false;
  }
  const char* tok = std::getenv("ZW3D_MCP_TOKEN");
  token_ = tok ? tok : "";
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
  while (!stop_) {
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(ls, &rd);
    timeval tv{0, 200 * 1000};
    if (select(0, &rd, nullptr, nullptr, &tv) <= 0) continue;
    SOCKET c = accept(ls, nullptr, nullptr);
    if (c == INVALID_SOCKET) continue;
    BOOL nd = TRUE;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char*)&nd, sizeof(nd));
    Serve((unsigned long long)c);
    shutdown(c, SD_SEND);
    closesocket(c);
  }
}

// One request per connection (Connection: close). Calls into the plugin block this thread until ZW3D's main thread has run them.
void Server::Serve(unsigned long long client) {
  SOCKET s = (SOCKET)client;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  std::string buf;
  if (!read_until(s, buf, kMaxHeader, deadline, stop_, [](const std::string& b) { return b.find("\r\n\r\n") != std::string::npos; })) {
    if (!buf.empty()) respond(s, 400, jsonrpc_error_body(-32600, "incomplete HTTP request"));
    return;
  }
  size_t hend = buf.find("\r\n\r\n");
  std::string head = buf.substr(0, hend);
  std::string rest = buf.substr(hend + 4);

  size_t le = head.find("\r\n");
  std::string request_line = head.substr(0, le);
  std::map<std::string, std::string> h;
  for (size_t pos = le == std::string::npos ? head.size() : le + 2; pos < head.size();) {
    size_t e = head.find("\r\n", pos);
    std::string line = head.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
    size_t c = line.find(':');
    if (c != std::string::npos) h[lower(trim(line.substr(0, c)))] = trim(line.substr(c + 1));
    if (e == std::string::npos) break;
    pos = e + 2;
  }
  size_t sp1 = request_line.find(' '), sp2 = request_line.find(' ', sp1 + 1);
  if (sp1 == std::string::npos || sp2 == std::string::npos) {
    respond(s, 400, jsonrpc_error_body(-32600, "bad request line"));
    return;
  }
  std::string method = request_line.substr(0, sp1), path = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
  size_t q = path.find('?');
  if (q != std::string::npos) path = path.substr(0, q);

  int bad = check_request_headers(port_, h["host"], h["origin"], h["authorization"], token_);
  if (bad) {
    respond(s, bad, jsonrpc_error_body(-32600, bad == 401 ? "missing or wrong bearer token" : "forbidden Host/Origin"),
            bad == 401 ? "WWW-Authenticate: Bearer\r\n" : "");
    return;
  }
  if (path != "/mcp" && path != "/mcp/" && path != "/") {
    respond(s, 404, jsonrpc_error_body(-32600, "not found: use POST /mcp"));
    return;
  }
  if (method != "POST") {
    respond(s, 405, jsonrpc_error_body(-32600, "this server only accepts POST (no SSE stream, no sessions)"), "Allow: POST\r\n");
    return;
  }

  // ---- body
  std::string body;
  bool chunked = lower(h["transfer-encoding"]).find("chunked") != std::string::npos;
  if (lower(h["expect"]) == "100-continue") send_all(s, "HTTP/1.1 100 Continue\r\n\r\n");
  if (chunked) {
    bool complete = false;
    auto pred = [&](const std::string& b) {
      std::string tmp;
      return decode_chunked(b, &tmp, &complete) && complete;
    };
    if (!read_until(s, rest, kMaxBody + 1024, deadline, stop_, pred)) {
      respond(s, 400, jsonrpc_error_body(-32600, "incomplete chunked body"));
      return;
    }
    decode_chunked(rest, &body, &complete);
  } else {
    size_t len = h.count("content-length") ? (size_t)std::strtoull(h["content-length"].c_str(), nullptr, 10) : 0;
    if (len > kMaxBody) {
      respond(s, 413, jsonrpc_error_body(-32600, "body larger than 16 MB"));
      return;
    }
    if (!read_until(s, rest, kMaxBody, deadline + std::chrono::seconds(45), stop_, [&](const std::string& b) { return b.size() >= len; })) {
      respond(s, 400, jsonrpc_error_body(-32600, "incomplete body"));
      return;
    }
    body = rest.substr(0, len);
  }

  nlohmann::json msg = nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
  if (msg.is_discarded()) {
    respond(s, 400, jsonrpc_error_body(-32700, "Parse error"));
    return;
  }
  nlohmann::json out;
  if (msg.is_array()) {
    out = nlohmann::json::array();
    for (const auto& m : msg) {
      nlohmann::json r = mcp::handle_message(m);
      if (!r.is_null()) out.push_back(r);
    }
    if (out.empty()) out = nullptr;
  } else {
    out = mcp::handle_message(msg);
  }
  if (out.is_null()) {
    respond(s, 202, "");  // only notifications / responses: nothing to return
    return;
  }
  respond(s, 200, out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
}
