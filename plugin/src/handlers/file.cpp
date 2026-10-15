// File-level handlers: new/save/export/screenshot and the optional raw-command escape hatch.
#include <windows.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include "handlers.h"
#include "../encoding.h"
#include "../errors.h"

namespace {

// ZW3D path/name arguments are vxLongPath (char[512]) in the system ANSI code page.
void to_ansi_path(const std::string& utf8, char (&out)[512], const char* what) {
  std::string a = utf8_to_ansi(utf8);
  if (a.empty()) invalid_input(std::string(what) + " 不能为空");
  if (a.size() >= sizeof(out)) throw ApiError("PATH_TOO_LONG", std::string(what) + " 过长（上限 511 字节）", "缩短路径。", ZW_API_PATH_LEN_OVER_RANGE);
  std::memcpy(out, a.c_str(), a.size() + 1);
}

std::string file_stem(const std::string& path) {
  size_t s = path.find_last_of("\\/");
  std::string name = s == std::string::npos ? path : path.substr(s + 1);
  size_t dot = name.find_last_of('.');
  return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}

json h_new_part(const json& p) {
  std::string name = p.at("name").get<std::string>();
  std::string part = p.contains("part_name") && p["part_name"].is_string() ? p["part_name"].get<std::string>() : file_stem(name);
  if (name.empty() || part.empty()) invalid_input("name 不能为空");

  char file[512];
  to_ansi_path(name, file, "name");
  // Single-root part file (.Z3PRT), the ZW3D 2025 default for parts. cvxFileNew would create a multi-root .Z3 file.
  // The root name of a single-root file is fixed, so part_name is accepted but not applied.
  check_api(cvxFileNewSingle(file, VX_FILE_PART, VX_NORMAL_PART, nullptr, nullptr), "cvxFileNewSingle");
  return {{"file", current_file_utf8()}, {"part", current_root_utf8()}};
}

json h_save_part(const json& p) {
  if (current_file_utf8().empty()) throw ApiError("WRONG_ROOT_ENV", "没有已打开的文件", "先调用 new_part。", ZW_API_ROOT_OBJ_ACT_FAIL);
  if (p.contains("path") && p["path"].is_string() && !p["path"].get<std::string>().empty()) {
    char path[512];
    to_ansi_path(p["path"].get<std::string>(), path, "path");
    try {
      check_api(cvxFileSaveAs(path), "cvxFileSaveAs");
    } catch (ApiError& e) {
      e.hint = "另存为失败：确认目录存在、扩展名为 .Z3PRT、文件未被占用。原提示：" + e.hint;
      throw;
    }
  } else {
    check_api(cvxFileSave(0), "cvxFileSave");
  }
  return {{"saved", true}, {"file", current_file_utf8()}};
}

json h_export_step(const json& p) {
  char path[512];
  to_ansi_path(p.at("path").get<std::string>(), path, "path");
  svxSTEPData data;
  check_api(cvxFileExportInit(VX_EXPORT_TYPE_STEP, 0, &data), "cvxFileExportInit");
  check_api(cvxFileExport(VX_EXPORT_TYPE_STEP, path, &data), "cvxFileExport");
  return {{"path", p["path"]}};
}

json h_screenshot(const json& p) {
  int w = p.value("width", 800), h = p.value("height", 600);
  if (w < 64 || h < 64 || w > 4096 || h > 4096) invalid_input("width/height 必须在 64..4096 之间");

  wchar_t tmp[MAX_PATH + 1];
  DWORD n = GetTempPathW(MAX_PATH, tmp);
  if (n == 0 || n > MAX_PATH) throw ApiError("INTERNAL", "无法获取临时目录");
  std::wstring dir = std::wstring(tmp) + L"zw3d_mcp";
  CreateDirectoryW(dir.c_str(), nullptr);  // ok if it already exists
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  std::string path_utf8 = wide_to_utf8(dir) + "\\shot_" + std::to_string(ms) + ".png";
  char path[512];
  to_ansi_path(path_utf8, path, "截图路径");

  if (p.value("zoom_all", true)) cvxDispZoomAll(1);
  svxImgData img;
  check_api(cvxFileExportInit(VX_EXPORT_TYPE_IMG, VX_EXPORT_IMG_TYPE_PNG, &img), "cvxFileExportInit");
  img.Type = VX_EXPORT_IMG_TYPE_PNG;
  img.RangeMode = VX_EXPORT_IMG_RANGE_MODE_NORMAL;
  img.ColorMode = VX_EXPORT_IMG_COLOR_MODE_24BITS;
  img.BkgndMode = VX_EXPORT_IMG_BKGND_MODE_CURRENT;
  img.Quality = 90;
  img.Width = (unsigned)w;
  img.Height = (unsigned)h;
  check_api(cvxFileExport(VX_EXPORT_TYPE_IMG, path, &img), "cvxFileExport");

  if (GetFileAttributesW(utf8_to_wide(path_utf8).c_str()) == INVALID_FILE_ATTRIBUTES)
    throw ApiError("EXPORT_FAILED", "截图接口返回成功但没有生成文件", "确认中望3D有可见的图形窗口（未最小化）。");
  return {{"path", path_utf8}, {"width", w}, {"height", h}};
}

json h_run_command(const json& p) {
  const char* en = std::getenv("ZW3D_MCP_ENABLE_RUN_COMMAND");
  if (!en || std::string(en) != "1")
    throw ApiError("DISABLED", "run_command 默认关闭", "在启动中望3D前设置环境变量 ZW3D_MCP_ENABLE_RUN_COMMAND=1。");
  std::string cmd = utf8_to_ansi(p.at("command").get<std::string>());
  cvxCmdSend(const_cast<char*>(cmd.c_str()));  // signature takes char*, it does not modify the string
  return {{"sent", p["command"]}};
}

}  // namespace

void register_file_handlers(std::map<std::string, Handler>& m) {
  m["new_part"] = h_new_part;
  m["save_part"] = h_save_part;
  m["export_step"] = h_export_step;
  m["screenshot"] = h_screenshot;
  m["run_command"] = h_run_command;
}
