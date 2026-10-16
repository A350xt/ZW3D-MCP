#include "dispatcher.h"
#include <chrono>
#include "errors.h"
#include "handlers/handlers.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

static const UINT WM_APP_MCP_JOB = WM_APP + 0x4D;
static const wchar_t* kClassName = L"ZW3DMcpDispatcherWindow";

Dispatcher& dispatcher() {
  static Dispatcher d;
  return d;
}

DWORD Dispatcher::current_main_thread_id() { return dispatcher().main_tid_; }

bool Dispatcher::Create() {
  HINSTANCE inst = reinterpret_cast<HINSTANCE>(&__ImageBase);
  WNDCLASSW wc{};
  wc.lpfnWndProc = &Dispatcher::WndProc;
  wc.hInstance = inst;
  wc.lpszClassName = kClassName;
  RegisterClassW(&wc);  // fails harmlessly with ERROR_CLASS_ALREADY_EXISTS on reload
  stopping_ = false;
  main_tid_ = GetCurrentThreadId();
  hwnd_ = CreateWindowExW(0, kClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
  return hwnd_ != nullptr;
}

void Dispatcher::Shutdown() {
  stopping_ = true;
  std::deque<std::shared_ptr<Job>> pending;
  {
    std::lock_guard<std::mutex> lk(mu_);
    pending.swap(queue_);
  }
  for (auto& j : pending) {
    j->cancelled = true;
    try {
      j->done.set_value(json{{"ok", false},
                             {"error", {{"code", "SHUTDOWN"}, {"api_code", nullptr}, {"message", "插件正在卸载"}, {"hint", ""}}}});
    } catch (const std::future_error&) {}
  }
}

void Dispatcher::Destroy() {
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
  UnregisterClassW(kClassName, reinterpret_cast<HINSTANCE>(&__ImageBase));
}

json Dispatcher::Execute(const std::string& method, const json& params, int timeout_ms) {
  auto error = [](const char* code, const char* msg, const char* hint) {
    return json{{"ok", false}, {"error", {{"code", code}, {"api_code", nullptr}, {"message", msg}, {"hint", hint}}}};
  };
  if (stopping_ || !hwnd_) return error("SHUTDOWN", "插件未运行", "");

  auto job = std::make_shared<Job>();
  job->method = method;
  job->params = params;
  auto fut = job->done.get_future();
  {
    std::lock_guard<std::mutex> lk(mu_);
    queue_.push_back(job);
  }
  PostMessageW(hwnd_, WM_APP_MCP_JOB, 0, 0);

  // Wait in short slices so Shutdown() (called on the main thread during Exit) can never deadlock against us.
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (fut.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready) {
    if (stopping_) return error("SHUTDOWN", "插件正在卸载", "");
    if (std::chrono::steady_clock::now() >= deadline) {
      job->cancelled = true;  // if it has not started yet, the main thread will skip it
      return error("TIMEOUT", "等待中望3D主线程执行超时",
                   "中望3D可能正忙或弹出了模态对话框：关闭对话框、按 Esc 结束当前命令后重试。");
    }
  }
  return fut.get();
}

LRESULT CALLBACK Dispatcher::WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
  if (msg == WM_APP_MCP_JOB) {
    dispatcher().Drain();
    return 0;
  }
  return DefWindowProcW(h, msg, w, l);
}

void Dispatcher::Drain() {
  for (;;) {
    std::shared_ptr<Job> job;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (queue_.empty()) return;
      job = queue_.front();
      queue_.pop_front();
    }
    if (job->cancelled) continue;
    json out = Run(*job);  // never throws
    try {
      job->done.set_value(std::move(out));
    } catch (const std::future_error&) {}
  }
}

json Dispatcher::Run(Job& job) {
  auto fail = [](const std::string& code, const std::string& msg, const std::string& hint, int api) {
    return json{{"ok", false}, {"error", {{"code", code}, {"api_code", api ? json(api) : json(nullptr)}, {"message", msg}, {"hint", hint}}}};
  };
  try {
    const auto& reg = handler_registry();
    auto it = reg.find(job.method);
    if (it == reg.end())
      return fail("UNKNOWN_METHOD", "未知方法: " + job.method, "检查插件版本与 MCP 服务端版本是否匹配。", 0);
    return json{{"ok", true}, {"result", invoke_handler(job.method, it->second, job.params)}};
  } catch (const ApiError& e) {
    return fail(e.code, e.what(), e.hint, e.api_code);
  } catch (const json::exception& e) {
    return fail("INVALID_INPUT", std::string("参数格式错误: ") + e.what(), "检查参数名称与类型。", 0);
  } catch (const std::exception& e) {
    return fail("INTERNAL", std::string("内部异常: ") + e.what(), "", 0);
  } catch (...) {
    return fail("INTERNAL", "未知内部异常", "", 0);
  }
}
