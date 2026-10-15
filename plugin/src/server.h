#pragma once
#include <atomic>
#include <string>
#include <thread>

// NDJSON server on 127.0.0.1. One client at a time; a new connection replaces the old one.
// Runs entirely on its own thread and never touches the ZW3D API (that goes through the Dispatcher).
class Server {
 public:
  bool Start(int port, std::string* error);
  void Stop();  // joins the thread
  bool running() const { return running_; }
  int port() const { return port_; }

 private:
  void Loop();
  void HandleLine(unsigned long long client, const std::string& line);

  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_{false};
  int port_ = 0;
  unsigned long long listen_ = ~0ULL;
  bool wsa_ = false;
};

Server& server();
