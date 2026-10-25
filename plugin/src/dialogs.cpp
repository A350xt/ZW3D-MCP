#include "dialogs.h"
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <uiautomation.h>
#include <wrl/client.h>
#include <algorithm>
#include <stdexcept>
#include "encoding.h"

using Microsoft::WRL::ComPtr;

namespace {

std::wstring window_text(HWND h) {
  wchar_t buf[512] = {0};
  GetWindowTextW(h, buf, 511);
  return buf;
}

std::wstring window_class(HWND h) {
  wchar_t buf[256] = {0};
  GetClassNameW(h, buf, 255);
  return buf;
}

std::wstring strip_amp(std::wstring s) {
  s.erase(std::remove(s.begin(), s.end(), L'&'), s.end());
  return s;
}

std::wstring lower(std::wstring s) {
  std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
  return s;
}

BOOL CALLBACK collect_top(HWND h, LPARAM lp) {
  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);
  if (pid == GetCurrentProcessId() && IsWindowVisible(h)) reinterpret_cast<std::vector<HWND>*>(lp)->push_back(h);
  return TRUE;
}

// Visible top-level windows of this process that look like pop-up dialogs.
std::vector<HWND> dialog_windows(std::vector<HWND>* disabled_out) {
  std::vector<HWND> tops;
  EnumWindows(collect_top, reinterpret_cast<LPARAM>(&tops));
  std::vector<HWND> disabled, found;
  for (HWND h : tops)
    if (!IsWindowEnabled(h)) disabled.push_back(h);
  auto add = [&](HWND h) {
    if (h && IsWindowVisible(h) && IsWindowEnabled(h) && std::find(found.begin(), found.end(), h) == found.end()) found.push_back(h);
  };
  for (HWND d : disabled) {
    HWND p = GetLastActivePopup(d);
    if (p && p != d) add(p);
  }
  for (HWND h : tops) {
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if (ex & WS_EX_NOACTIVATE) continue;
    RECT r{};
    GetWindowRect(h, &r);
    if (r.right - r.left < 60 || r.bottom - r.top < 40) continue;
    std::wstring cls = window_class(h);
    HWND owner = GetWindow(h, GW_OWNER);
    bool owned_by_disabled = owner && std::find(disabled.begin(), disabled.end(), owner) != disabled.end();
    if (cls == L"#32770" || owned_by_disabled) add(h);
    // a parentless Qt modal dialog: enabled window with a title while another window of ours is disabled
    else if (!disabled.empty() && !window_text(h).empty() && (ex & WS_EX_TOOLWINDOW) == 0) add(h);
  }
  if (disabled_out) *disabled_out = disabled;
  return found;
}

struct ComInit {
  bool ok = false;
  ComInit() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ok = SUCCEEDED(hr);  // RPC_E_CHANGED_MODE: COM is already usable on this thread, just do not uninitialize
  }
  ~ComInit() {
    if (ok) CoUninitialize();
  }
};

std::string bstr_utf8(BSTR b) { return b ? wide_to_utf8(std::wstring(b, SysStringLen(b))) : std::string(); }

struct UiaButton {
  std::wstring name;
  ComPtr<IUIAutomationElement> el;
};

// Reads texts and buttons through UI Automation (Qt exposes its widgets this way); native child windows are used as a fallback.
void read_contents(HWND h, DialogInfo& d, std::vector<UiaButton>* buttons) {
  ComPtr<IUIAutomation> uia;
  if (SUCCEEDED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation), reinterpret_cast<void**>(uia.GetAddressOf()))) && uia) {
    ComPtr<IUIAutomationElement> root;
    ComPtr<IUIAutomationCondition> cond;
    ComPtr<IUIAutomationElementArray> all;
    if (SUCCEEDED(uia->ElementFromHandle(h, &root)) && root && SUCCEEDED(uia->CreateTrueCondition(&cond)) &&
        SUCCEEDED(root->FindAll(TreeScope_Descendants, cond.Get(), &all)) && all) {
      int n = 0;
      all->get_Length(&n);
      for (int i = 0; i < n && i < 300; ++i) {
        ComPtr<IUIAutomationElement> e;
        if (FAILED(all->GetElement(i, &e)) || !e) continue;
        CONTROLTYPEID type = 0;
        e->get_CurrentControlType(&type);
        BSTR name = nullptr;
        e->get_CurrentName(&name);
        std::wstring w = name ? std::wstring(name, SysStringLen(name)) : std::wstring();
        SysFreeString(name);
        if (w.empty()) continue;
        if (type == UIA_ButtonControlTypeId) {
          d.buttons.push_back(wide_to_utf8(strip_amp(w)));
          if (buttons) buttons->push_back({strip_amp(w), e});
        } else if (type == UIA_TextControlTypeId) {
          d.texts.push_back(wide_to_utf8(w));
        }
      }
    }
  }
  if (d.buttons.empty() && d.texts.empty()) {
    // Native dialog (message box etc.)
    struct Ctx { DialogInfo* d; } ctx{&d};
    EnumChildWindows(h, [](HWND c, LPARAM lp) -> BOOL {
      auto* x = reinterpret_cast<Ctx*>(lp);
      std::wstring cls = window_class(c), t = window_text(c);
      if (t.empty()) return TRUE;
      if (cls == L"Button") x->d->buttons.push_back(wide_to_utf8(strip_amp(t)));
      else if (cls == L"Static") x->d->texts.push_back(wide_to_utf8(t));
      return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
  }
}

bool matches_any(const std::wstring& caption, const std::vector<std::wstring>& names) {
  std::wstring c = lower(caption);
  for (const auto& n : names)
    if (c == lower(n)) return true;
  return false;
}

}  // namespace

std::vector<DialogInfo> find_dialogs(bool with_contents) {
  std::vector<HWND> disabled;
  std::vector<HWND> wins = dialog_windows(&disabled);
  std::vector<DialogInfo> out;
  ComInit com;
  for (HWND h : wins) {
    DialogInfo d;
    d.hwnd = reinterpret_cast<unsigned long long>(h);
    d.title = wide_to_utf8(window_text(h));
    d.window_class = wide_to_utf8(window_class(h));
    d.modal = !disabled.empty();
    if (with_contents) read_contents(h, d, nullptr);
    out.push_back(std::move(d));
  }
  return out;
}

bool modal_dialog_open() {
  std::vector<HWND> disabled;
  std::vector<HWND> wins = dialog_windows(&disabled);
  return !disabled.empty() && !wins.empty();
}

json dialogs_to_json(const std::vector<DialogInfo>& d) {
  json a = json::array();
  for (const auto& x : d)
    a.push_back({{"title", x.title}, {"class", x.window_class}, {"modal", x.modal}, {"texts", x.texts}, {"buttons", x.buttons}});
  return a;
}

std::string dialogs_summary(const std::vector<DialogInfo>& d) {
  if (d.empty()) return "无";
  const DialogInfo& x = d.front();
  std::string s = "标题「" + x.title + "」";
  if (!x.texts.empty()) {
    s += "，内容「";
    for (size_t i = 0; i < x.texts.size() && i < 4; ++i) s += (i ? " / " : "") + x.texts[i];
    s += "」";
  }
  if (!x.buttons.empty()) {
    s += "，按钮 [";
    for (size_t i = 0; i < x.buttons.size(); ++i) s += (i ? ", " : "") + x.buttons[i];
    s += "]";
  }
  if (d.size() > 1) s += "（共 " + std::to_string(d.size()) + " 个对话框）";
  return s;
}

json dismiss_dialog(const std::string& action, const std::string& button) {
  std::vector<HWND> disabled;
  std::vector<HWND> wins = dialog_windows(&disabled);
  if (wins.empty()) throw std::runtime_error("当前没有弹出的对话框");
  HWND h = wins.front();
  ComInit com;
  DialogInfo info;
  std::vector<UiaButton> ubtns;
  info.title = wide_to_utf8(window_text(h));
  read_contents(h, info, &ubtns);

  static const std::vector<std::wstring> accept_names = {L"ok", L"yes", L"continue", L"accept", L"apply", L"retry", L"确定", L"是", L"确认", L"继续", L"应用", L"好", L"是(y)"};
  static const std::vector<std::wstring> cancel_names = {L"cancel", L"no", L"close", L"abort", L"ignore", L"取消", L"否", L"关闭", L"不", L"忽略", L"否(n)"};
  const auto& wanted = action == "cancel" ? cancel_names : accept_names;
  std::wstring want_caption = utf8_to_wide(button);

  json out = {{"dialog", {{"title", info.title}, {"texts", info.texts}, {"buttons", info.buttons}}}, {"clicked", nullptr}, {"method", nullptr}};

  // 1. UI Automation: Invoke the matching button
  for (const auto& b : ubtns) {
    bool hit = !want_caption.empty() ? lower(b.name).find(lower(want_caption)) != std::wstring::npos : matches_any(b.name, wanted);
    if (!hit) continue;
    ComPtr<IUIAutomationInvokePattern> inv;
    if (SUCCEEDED(b.el->GetCurrentPatternAs(UIA_InvokePatternId, __uuidof(IUIAutomationInvokePattern), reinterpret_cast<void**>(inv.GetAddressOf()))) && inv &&
        SUCCEEDED(inv->Invoke())) {
      out["clicked"] = wide_to_utf8(b.name);
      out["method"] = "ui_automation";
      break;
    }
  }
  // 2. native buttons
  if (out["clicked"].is_null()) {
    struct Ctx { const std::vector<std::wstring>* wanted; std::wstring caption; HWND hit = nullptr; std::wstring hit_text; } ctx{&wanted, want_caption};
    EnumChildWindows(h, [](HWND c, LPARAM lp) -> BOOL {
      auto* x = reinterpret_cast<Ctx*>(lp);
      if (x->hit || window_class(c) != L"Button") return TRUE;
      std::wstring t = strip_amp(window_text(c));
      bool ok = !x->caption.empty() ? lower(t).find(lower(x->caption)) != std::wstring::npos : matches_any(t, *x->wanted);
      if (ok && IsWindowEnabled(c)) {
        x->hit = c;
        x->hit_text = t;
      }
      return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    if (ctx.hit) {
      PostMessageW(ctx.hit, BM_CLICK, 0, 0);
      out["clicked"] = wide_to_utf8(ctx.hit_text);
      out["method"] = "native_button";
    }
  }
  // 3. keyboard / close (only when no button caption was demanded)
  if (out["clicked"].is_null() && want_caption.empty()) {
    WORD vk = action == "cancel" ? VK_ESCAPE : VK_RETURN;
    PostMessageW(h, WM_KEYDOWN, vk, 0);
    PostMessageW(h, WM_KEYUP, vk, 0xC0000000);
    out["method"] = action == "cancel" ? "key_escape" : "key_enter";
    Sleep(300);
    if (action == "cancel" && !find_dialogs(false).empty() && IsWindow(h) && IsWindowVisible(h)) {
      PostMessageW(h, WM_CLOSE, 0, 0);
      out["method"] = "wm_close";
    }
  }
  Sleep(400);
  auto rest = find_dialogs(true);
  out["remaining"] = dialogs_to_json(rest);
  out["dismissed"] = rest.empty() || rest.front().hwnd != reinterpret_cast<unsigned long long>(h);
  return out;
}
