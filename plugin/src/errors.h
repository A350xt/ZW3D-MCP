#pragma once
#include <stdexcept>
#include <string>

// Error carried from handlers back to the protocol layer ({"code","api_code","message","hint"}).
struct ApiError : std::runtime_error {
  std::string code;
  int api_code;
  std::string hint;
  ApiError(std::string code_, const std::string& message, std::string hint_ = "", int api_code_ = 0)
      : std::runtime_error(message), code(std::move(code_)), api_code(api_code_), hint(std::move(hint_)) {}
};

// Throws ApiError for a non-zero evxErrors value; `what` names the failing API call for the message.
void check_api(int err, const char* what);
// Convenience for argument problems detected inside the plugin.
[[noreturn]] void invalid_input(const std::string& message, const std::string& hint = "");
