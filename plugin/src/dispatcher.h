#pragma once
#include <windows.h>
#include <atomic>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include "json.hpp"

using json = nlohmann::json;

// Runs handlers on the thread that called Create() (ZW3D's main/UI thread) via a message-only window.
// Execute() is called from the socket thread and blocks until the handler finished, timed out, or shutdown began.
class Dispatcher {
 public:
  bool Create();          // main thread only
  void Shutdown();        // main thread: fails pending jobs, wakes waiters
  void Resume() { stopping_ = false; }  // main thread: re-enable after a Shutdown() used only to unblock Server::Stop() (McpStop)
  void Destroy();       // main thread: destroys the window (call after the server thread has been joined)
  json Execute(const std::string& method, const json& params, int timeout_ms);  // any thread; returns the full response body
  DWORD main_thread_id() const { return main_tid_; }
  static DWORD current_main_thread_id();

 private:
  struct Job {
    std::string method;
    json params;
    std::promise<json> done;
    std::atomic<bool> cancelled{false};
  };
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  void Drain();
  json Run(Job& job);

  HWND hwnd_ = nullptr;
  DWORD main_tid_ = 0;
  std::atomic<bool> stopping_{false};
  std::mutex mu_;
  std::deque<std::shared_ptr<Job>> queue_;
};

Dispatcher& dispatcher();
