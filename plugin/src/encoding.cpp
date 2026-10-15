#include "encoding.h"
#include <windows.h>

static std::wstring to_wide(const std::string& s, UINT cp) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), nullptr, 0);
  if (n <= 0) return {};
  std::wstring w(n, L'\0');
  MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), &w[0], n);
  return w;
}

static std::string from_wide(const std::wstring& w, UINT cp) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(cp, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  if (n <= 0) return {};
  std::string s(n, '\0');
  WideCharToMultiByte(cp, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
  return s;
}

std::wstring utf8_to_wide(const std::string& s) { return to_wide(s, CP_UTF8); }
std::string wide_to_utf8(const std::wstring& w) { return from_wide(w, CP_UTF8); }
std::string utf8_to_ansi(const std::string& s) { return from_wide(to_wide(s, CP_UTF8), CP_ACP); }
std::string ansi_to_utf8(const std::string& s) { return from_wide(to_wide(s, CP_ACP), CP_UTF8); }
