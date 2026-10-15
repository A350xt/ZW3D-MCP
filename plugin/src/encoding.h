#pragma once
#include <string>

// UTF-8 <-> system ANSI code page (CP_ACP, GBK on Chinese Windows). JSON is UTF-8; ZW3D APIs take ANSI.
std::string utf8_to_ansi(const std::string& s);
std::string ansi_to_utf8(const std::string& s);
std::wstring utf8_to_wide(const std::string& s);
std::string wide_to_utf8(const std::wstring& s);
