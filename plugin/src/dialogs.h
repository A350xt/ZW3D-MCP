#pragma once
// Detection and dismissal of ZW3D pop-up dialogs. Everything here may run on any thread EXCEPT that nothing may be called from inside a
// ZW3D API call: it only uses Win32 / UI Automation, never the ZW3D API, so it works while the main thread is blocked in a modal dialog.
#include <string>
#include <vector>
#include "json.hpp"

using json = nlohmann::json;

struct DialogInfo {
  unsigned long long hwnd = 0;
  std::string title;
  std::string window_class;
  bool modal = false;                // the main window (or another window of ZW3D) is disabled behind it
  std::vector<std::string> texts;    // labels / message text (UI Automation, best effort)
  std::vector<std::string> buttons;  // button captions (UI Automation or native buttons)
};

// Open pop-up dialogs of this process, topmost first. with_contents = also read texts and buttons (a bit slower).
std::vector<DialogInfo> find_dialogs(bool with_contents);
bool modal_dialog_open();
json dialogs_to_json(const std::vector<DialogInfo>& d);
// One-line description for error messages.
std::string dialogs_summary(const std::vector<DialogInfo>& d);
// Clicks a button of the topmost dialog. action: "accept" (OK / Yes / Continue ...), "cancel" (Cancel / No / Close ...); button: caption (substring)
// overrides the action. Returns {"clicked": caption|null, "method": ..., "remaining": [...]}; throws std::runtime_error when no dialog is open.
json dismiss_dialog(const std::string& action, const std::string& button);
