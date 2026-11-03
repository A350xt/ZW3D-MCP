// File-level handlers: new/save/export/screenshot and the optional raw-command escape hatch.
#include <windows.h>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <cctype>
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

json h_close_file(const json& p) {
  std::string mode = p.value("save", std::string("ask"));
  int option = mode == "save" ? 1 : mode == "discard" ? 2 : 0;
  if (mode != "save" && mode != "discard" && mode != "ask") invalid_input("save 取值无效: " + mode, "可选: save, discard, ask");
  std::string closing = current_file_utf8();
  char name[512] = {0};
  if (p.contains("path") && p["path"].is_string() && !p["path"].get<std::string>().empty()) {
    to_ansi_path(p["path"].get<std::string>(), name, "path");
    closing = p["path"].get<std::string>();
  } else if (closing.empty()) {
    throw ApiError("WRONG_ROOT_ENV", "没有已打开的文件", "先调用 new_part 或 open_file。");
  }
  // option 0 lets ZW3D ask "save changes?": that dialog is reported as DIALOG_OPEN and answered with dismiss_dialog.
  cvxFileClose2(name, option);
  return {{"closed", closing}, {"save", mode}, {"active_file", current_file_utf8()},
          {"note", "关闭后文件内的实体/特征 id 全部失效；如还有其他打开的文件，当前激活文件见 active_file。"}};
}

// Generic export. The format comes from `format` or the file extension; each format has its own data struct, initialised by cvxFileExportInit.
json h_export_file(const json& p) {
  std::string path_utf8 = p.at("path").get<std::string>();
  std::string fmt = p.value("format", std::string());
  std::string ext;
  size_t dot = path_utf8.find_last_of('.');
  if (dot != std::string::npos) ext = path_utf8.substr(dot + 1);
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  if (fmt.empty()) {
    if (ext == "step" || ext == "stp") fmt = "step";
    else if (ext == "iges" || ext == "igs") fmt = "iges";
    else if (ext == "x_t") fmt = "parasolid";
    else if (ext == "x_b") fmt = "parasolid_binary";
    else if (ext == "stl") fmt = "stl";
    else if (ext == "obj") fmt = "obj";
    else if (ext == "pdf") fmt = "pdf";
    else if (ext == "dwg") fmt = "dwg";
    else if (ext == "dxf") fmt = "dxf";
    else if (ext == "jt") fmt = "jt";
    else invalid_input("无法从扩展名判断导出格式: ." + ext, "给出 format，或使用 .step .iges .x_t .x_b .stl .obj .pdf .dwg .dxf .jt。");
  }
  union ExportBuf {
    svxSTEPData step; svxIGESData iges; svxPARAData para; svxSTLData stl; svxOBJData obj; svxPdfData pdf; svxDWGData dwg; svxJTData jt;
  };
  ExportBuf* buf = new ExportBuf();
  std::memset(buf, 0, sizeof(*buf));
  struct Free { ExportBuf* b; ~Free() { delete b; } } guard{buf};
  evxExportType type;
  if (fmt == "step") type = VX_EXPORT_TYPE_STEP;
  else if (fmt == "iges") type = VX_EXPORT_TYPE_IGES;
  else if (fmt == "parasolid") type = VX_EXPORT_TYPE_PARA_TEXT;
  else if (fmt == "parasolid_binary") type = VX_EXPORT_TYPE_PARA_BINARY;
  else if (fmt == "stl") type = VX_EXPORT_TYPE_STL;
  else if (fmt == "obj") type = VX_EXPORT_TYPE_OBJ;
  else if (fmt == "pdf") type = VX_EXPORT_TYPE_PDF;
  else if (fmt == "dwg" || fmt == "dxf") type = VX_EXPORT_TYPE_DWG;
  else if (fmt == "jt") type = VX_EXPORT_TYPE_JT;
  else invalid_input("format 取值无效: " + fmt, "可选: step, iges, parasolid, parasolid_binary, stl, obj, pdf, dwg, dxf, jt");

  char path[512];
  to_ansi_path(path_utf8, path, "path");
  check_api(cvxFileExportInit(type, 0, buf), "cvxFileExportInit");
  if (type == VX_EXPORT_TYPE_STL) buf->stl.FileFormat = p.value("ascii", false) ? 1 : 0;
  if (type == VX_EXPORT_TYPE_DWG) buf->dwg.FileType = fmt == "dxf" ? 1 : 0;
  try {
    check_api(cvxFileExport(type, path, buf), "cvxFileExport");
  } catch (ApiError& e) {
    e.hint = "导出 " + fmt + " 失败：确认目录存在且可写、文件未被占用；工程图格式(pdf/dwg/dxf)需要先激活工程图，3D 格式需要激活零件或装配。原提示：" + e.hint;
    throw;
  }
  WIN32_FILE_ATTRIBUTE_DATA fa{};
  std::wstring wpath = utf8_to_wide(path_utf8);
  if (!GetFileAttributesExW(wpath.c_str(), GetFileExInfoStandard, &fa))
    throw ApiError("EXPORT_FAILED", "导出接口返回成功但没有生成文件", "确认路径和扩展名正确，当前文件类型适合该格式。");
  unsigned long long size = ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
  return {{"path", path_utf8}, {"format", fmt}, {"size_bytes", size}};
}

json h_export_step(const json& p) {
  char path[512];
  to_ansi_path(p.at("path").get<std::string>(), path, "path");
  svxSTEPData data;
  check_api(cvxFileExportInit(VX_EXPORT_TYPE_STEP, 0, &data), "cvxFileExportInit");
  check_api(cvxFileExport(VX_EXPORT_TYPE_STEP, path, &data), "cvxFileExport");
  return {{"path", p["path"]}};
}

std::wstring screenshot_dir() {
  wchar_t tmp[MAX_PATH + 1];
  DWORD n = GetTempPathW(MAX_PATH, tmp);
  if (n == 0 || n > MAX_PATH) throw ApiError("INTERNAL", "无法获取临时目录");
  return std::wstring(tmp) + L"zw3d_mcp";
}

}  // namespace

// Deletes shot_*.png in %TEMP%\zw3d_mcp. older_than_s < 0 removes all of them (and the folder if it ends up empty).
// Only files matching our own name pattern are touched. Best effort: never throws.
void cleanup_screenshots(int older_than_s) {
  try {
    std::wstring dir = screenshot_dir();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\shot_*.png").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
      FILETIME now_ft;
      GetSystemTimeAsFileTime(&now_ft);
      ULARGE_INTEGER now;
      now.LowPart = now_ft.dwLowDateTime;
      now.HighPart = now_ft.dwHighDateTime;
      do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        ULARGE_INTEGER w;
        w.LowPart = fd.ftLastWriteTime.dwLowDateTime;
        w.HighPart = fd.ftLastWriteTime.dwHighDateTime;
        bool old = older_than_s < 0 || (now.QuadPart > w.QuadPart && (now.QuadPart - w.QuadPart) / 10000000ULL >= (unsigned)older_than_s);
        if (old) DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
      } while (FindNextFileW(h, &fd));
      FindClose(h);
    }
    if (older_than_s < 0) RemoveDirectoryW(dir.c_str());  // fails harmlessly if something else lives there
  } catch (...) {}
}

namespace {

// Standard view / display mode by name; empty string = leave as is.
void apply_view(const std::string& view, const std::string& mode) {
  if (!view.empty()) {
    static const std::pair<const char*, evxViewStd> views[] = {{"front", VX_VIEW_FRONT}, {"back", VX_VIEW_BACK}, {"top", VX_VIEW_TOP}, {"bottom", VX_VIEW_BOTTOM},
                                                              {"right", VX_VIEW_RIGHT}, {"left", VX_VIEW_LEFT}, {"iso", VX_VIEW_ISO}};
    bool found = false;
    for (const auto& v : views)
      if (view == v.first) {
        cvxViewStd(v.second, 100.0);
        found = true;
      }
    if (!found) invalid_input("view 取值无效: " + view, "可选: front, back, top, bottom, right, left, iso");
  }
  if (!mode.empty()) {
    evxDispMode m;
    if (mode == "shaded") m = VX_DISP_SHADE;
    else if (mode == "wireframe") m = VX_DISP_WIRE;
    else if (mode == "hidden") m = VX_DISP_HIDD_APPROX;
    else if (mode == "shaded_edges") m = VX_DISP_EDGE;
    else invalid_input("display_mode 取值无效: " + mode, "可选: shaded, shaded_edges, wireframe, hidden");
    cvxDispModeSet(m);
  }
}

json h_set_view(const json& p) {
  apply_view(p.value("view", std::string()), p.value("display_mode", std::string()));
  if (p.value("zoom_all", true)) cvxDispZoomAll(1);
  cvxDispRedraw();
  return {{"view", p.value("view", std::string())}, {"display_mode", p.value("display_mode", std::string())}};
}

// ---- arbitrary view angle ---------------------------------------------------------------------------------------
// A view is described by three orthonormal world vectors: X = screen right, Y = screen up, Z = from the model towards the viewer.
// How cvxViewGet / cvxViewSet store them in an svxMatrix is not documented, so the convention is calibrated once at run time against the
// standard front view (X = +x, Y = +z, Z = -y), trying the four plausible layouts (axes as matrix columns or rows, Z towards or away from the viewer).
struct V3d { double x, y, z; };
V3d v_add(V3d a, V3d b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3d v_mul(V3d a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double v_dot(V3d a, V3d b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3d v_cross(V3d a, V3d b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3d v_unit(V3d a) {
  double l = std::sqrt(v_dot(a, a));
  if (l < 1e-12) invalid_input("方向向量不能为零");
  return v_mul(a, 1.0 / l);
}
V3d v_rot(V3d v, V3d k, double a) {  // Rodrigues, k is a unit axis
  double c = std::cos(a), s = std::sin(a);
  return v_add(v_add(v_mul(v, c), v_mul(v_cross(k, v), s)), v_mul(k, v_dot(k, v) * (1 - c)));
}

struct ViewAxes { V3d x, y, z; };

svxMatrix to_view_matrix(const ViewAxes& a, int layout) {
  V3d X = a.x, Y = a.y, Z = a.z;
  if (layout >= 2) Z = v_mul(Z, -1);  // Z pointing away from the viewer
  svxMatrix m{};
  m.identity = 0;
  if (layout % 2 == 0) {  // axes are the "<axis><component>" fields: x-axis = (xx, xy, xz)
    m.xx = X.x; m.xy = X.y; m.xz = X.z;
    m.yx = Y.x; m.yy = Y.y; m.yz = Y.z;
    m.zx = Z.x; m.zy = Z.y; m.zz = Z.z;
  } else {  // transposed
    m.xx = X.x; m.yx = X.y; m.zx = X.z;
    m.xy = Y.x; m.yy = Y.y; m.zy = Y.z;
    m.xz = Z.x; m.yz = Z.y; m.zz = Z.z;
  }
  return m;
}

ViewAxes from_view_matrix(const svxMatrix& m, int layout) {
  ViewAxes a;
  if (layout % 2 == 0) {
    a.x = {m.xx, m.xy, m.xz}; a.y = {m.yx, m.yy, m.yz}; a.z = {m.zx, m.zy, m.zz};
  } else {
    a.x = {m.xx, m.yx, m.zx}; a.y = {m.xy, m.yy, m.zy}; a.z = {m.xz, m.yz, m.zz};
  }
  if (layout >= 2) a.z = v_mul(a.z, -1);
  return a;
}

bool matrix_close(const svxMatrix& a, const svxMatrix& b) {
  const double* pa[] = {&a.xx, &a.yx, &a.zx, &a.xy, &a.yy, &a.zy, &a.xz, &a.yz, &a.zz};
  const double* pb[] = {&b.xx, &b.yx, &b.zx, &b.xy, &b.yy, &b.zy, &b.xz, &b.yz, &b.zz};
  for (int i = 0; i < 9; ++i)
    if (std::fabs(*pa[i] - *pb[i]) > 1e-4) return false;
  return true;
}

// Index of the matching layout (0..3), calibrated on first use. Leaves the view on the front view; callers set the final view afterwards.
int view_layout() {
  static int layout = -2;
  if (layout != -2) return layout;
  cvxViewStd(VX_VIEW_FRONT, 100.0);
  svxMatrix f{};
  double ext = 0;
  cvxViewGet(&f, &ext);
  ViewAxes front{{1, 0, 0}, {0, 0, 1}, {0, -1, 0}};
  layout = -1;
  for (int l = 0; l < 4; ++l)
    if (matrix_close(f, to_view_matrix(front, l))) {
      layout = l;
      break;
    }
  if (layout < 0)
    throw ApiError("VIEW_CONVENTION", "无法识别视图矩阵的约定（前视图矩阵与任何预期布局都不吻合）",
                   "前视图矩阵: x(" + std::to_string(f.xx) + "," + std::to_string(f.xy) + "," + std::to_string(f.xz) + ") y(" + std::to_string(f.yx) + "," +
                       std::to_string(f.yy) + "," + std::to_string(f.yz) + ") z(" + std::to_string(f.zx) + "," + std::to_string(f.zy) + "," + std::to_string(f.zz) +
                       ")。把它反馈给开发者；改用 screenshot/set_view 的标准视角。");
  return layout;
}

// Orientation from azimuth/elevation: the camera sits in direction (cos el cos az, cos el sin az, sin el) from the target, world +Z up.
// az = -90, el = 0 is the front view, az = 0 the right view, el = 90 the top view; roll turns the picture about the viewing direction.
ViewAxes axes_from_angles(double az_deg, double el_deg, double roll_deg) {
  const double d2r = 3.14159265358979323846 / 180.0;
  double az = az_deg * d2r, el = el_deg * d2r;
  V3d z{std::cos(el) * std::cos(az), std::cos(el) * std::sin(az), std::sin(el)};
  V3d up{0, 0, 1};
  V3d x = v_cross(up, z);
  if (v_dot(x, x) < 1e-12) {  // looking straight down / up: keep screen-up towards world +Y (top view) or -Y (bottom view)
    x = {1, 0, 0};
  }
  x = v_unit(x);
  V3d y = v_cross(z, x);
  ViewAxes a{x, y, z};
  if (roll_deg != 0) {
    double r = roll_deg * d2r;
    a.x = v_unit(v_add(v_mul(x, std::cos(r)), v_mul(y, std::sin(r))));
    a.y = v_unit(v_add(v_mul(y, std::cos(r)), v_mul(x, -std::sin(r))));
  }
  return a;
}

json h_rotate_view(const json& p) {
  std::string mode = p.value("mode", std::string("absolute"));
  if (mode != "absolute" && mode != "relative") invalid_input("mode 必须是 absolute 或 relative");
  // read the current view before the calibration moves it
  svxMatrix cur{};
  double extent = 0;
  cvxViewGet(&cur, &extent);
  int layout = view_layout();

  ViewAxes next;
  if (mode == "relative") {
    ViewAxes c = from_view_matrix(cur, layout);
    double d2r = 3.14159265358979323846 / 180.0;
    double daz = p.value("azimuth", 0.0) * d2r, del = p.value("elevation", 0.0) * d2r, droll = p.value("roll", 0.0) * d2r;
    V3d zaxis{0, 0, 1};
    // turntable about the world Z axis, then tilt about the screen X axis, then roll about the viewing direction
    c = {v_rot(c.x, zaxis, daz), v_rot(c.y, zaxis, daz), v_rot(c.z, zaxis, daz)};
    c = {c.x, v_rot(c.y, c.x, -del), v_rot(c.z, c.x, -del)};
    c = {v_rot(c.x, c.z, droll), v_rot(c.y, c.z, droll), c.z};
    next = {v_unit(c.x), v_unit(c.y), v_unit(c.z)};
  } else if (p.contains("direction") && !p["direction"].is_null()) {
    svxVector d = to_vector(p["direction"], "direction");  // from the model towards the camera
    V3d z = v_unit({d.x, d.y, d.z});
    V3d up{0, 0, 1};
    if (p.contains("up") && !p["up"].is_null()) {
      svxVector u = to_vector(p["up"], "up");
      up = {u.x, u.y, u.z};
    }
    V3d x = v_cross(up, z);
    if (v_dot(x, x) < 1e-12) invalid_input("up 与 direction 平行", "换一个不平行的 up 向量（默认 [0,0,1]）。");
    x = v_unit(x);
    next = {x, v_cross(z, x), z};
  } else {
    next = axes_from_angles(p.value("azimuth", -45.0), p.value("elevation", 35.264), p.value("roll", 0.0));
  }

  svxMatrix m = to_view_matrix(next, layout);
  cvxViewSet(&m, extent > 0 ? extent : 100.0);
  if (p.value("zoom_all", true)) cvxDispZoomAll(1);
  cvxDispRedraw();
  return {{"eye_direction", {next.z.x, next.z.y, next.z.z}}, {"screen_right", {next.x.x, next.x.y, next.x.z}}, {"screen_up", {next.y.x, next.y.y, next.y.z}},
          {"mode", mode}};
}

json h_open_file(const json& p) {
  char path[512];
  to_ansi_path(p.at("path").get<std::string>(), path, "path");
  check_api(cvxFileOpen(path), "cvxFileOpen");
  return {{"file", current_file_utf8()}, {"part", current_root_utf8()}, {"shapes", inq_shape_ids()}};
}

json h_import_file(const json& p) {
  std::string path = p.at("path").get<std::string>();
  std::string ext = path.substr(path.find_last_of('.') == std::string::npos ? path.size() : path.find_last_of('.'));
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  evxImportType type;
  if (ext == ".step" || ext == ".stp") type = VX_IMPORT_TYPE_STEP;
  else if (ext == ".x_t" || ext == ".x_b" || ext == ".xt") type = VX_IMPORT_TYPE_PARASOLID;
  else if (ext == ".sat" || ext == ".sab") type = VX_IMPORT_TYPE_SAT;
  else if (ext == ".igs" || ext == ".iges") type = VX_IMPORT_TYPE_IGES;
  else if (ext == ".stl") type = VX_IMPORT_TYPE_STL;
  else invalid_input("不支持的导入扩展名: " + ext, "支持 .step/.stp .x_t .sat .igs/.iges .stl；.Z3PRT/.Z3 文件请用 open_file。");
  std::string target = p.value("target", std::string("new_file"));
  if (target != "new_file" && target != "current_part") invalid_input("target 必须是 new_file 或 current_part");

  svxImportData d;
  check_api(cvxFileImportInit(&d, type, nullptr), "cvxFileImportInit");
  d.type = type;
  to_ansi_path(path, d.filePath, "path");
  d.importTo = target == "new_file" ? 2 : 0;
  std::vector<int> before = target == "current_part" ? inq_shape_ids() : std::vector<int>();
  check_api(cvxFileImport(&d), "cvxFileImport");
  cvxDispZoomAll(1);
  return {{"file", current_file_utf8()}, {"part", current_root_utf8()}, {"shapes", inq_shape_ids()}, {"shapes_before", before}};
}

json h_screenshot(const json& p) {
  int w = p.value("width", 800), h = p.value("height", 600);
  if (w < 64 || h < 64 || w > 4096 || h > 4096) invalid_input("width/height 必须在 64..4096 之间");
  apply_view(p.value("view", std::string()), p.value("display_mode", std::string()));

  std::wstring dir = screenshot_dir();
  CreateDirectoryW(dir.c_str(), nullptr);  // ok if it already exists
  cleanup_screenshots(600);                // drop leftovers (client crashed / never fetched) older than 10 min
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

json h_undo_redo(const json& p, bool undo) {
  int steps = p.value("steps", 1);
  if (steps < 1 || steps > 50) invalid_input("steps 必须在 1..50 之间");
  std::vector<int> before = inq_shape_ids();
  int done = 0;
  bool fallback = false;
  for (; done < steps; ++done) {
    bool markers = true;
    if (!tx_undo_redo(undo, &markers)) break;
    fallback = fallback || !markers;
  }
  if (done == 0) throw ApiError("NOTHING_TO_DO", undo ? "没有可撤销的操作" : "没有可重做的操作", "");
  std::vector<int> after = inq_shape_ids();
  json removed = json::array(), added = json::array();
  for (int s : before) if (std::find(after.begin(), after.end(), s) == after.end()) removed.push_back(s);
  for (int s : after) if (std::find(before.begin(), before.end(), s) == before.end()) added.push_back(s);
  json out = {{"steps_done", done}, {"shapes", after}, {"removed_shape_ids", removed}, {"added_shape_ids", added},
              {"undo_steps_available", tx_steps_available()}};
  if (fallback) out["note"] = "部分步骤不是由本插件记录的，按单个底层步骤撤销/重做，可能一次只影响一个图元。";
  if (removed.empty() && added.empty() && !out.contains("note"))
    out["note"] = "撤销/重做的步骤没有改变任何实体（可能只涉及线框、草图或特征参数）；用 list_shapes 核对。";
  return out;
}
json h_undo(const json& p) { return h_undo_redo(p, true); }
json h_redo(const json& p) { return h_undo_redo(p, false); }

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
  m["close_file"] = h_close_file;
  m["export_step"] = h_export_step;
  m["export_file"] = h_export_file;
  m["screenshot"] = h_screenshot;
  m["set_view"] = h_set_view;
  m["rotate_view"] = h_rotate_view;
  m["open_file"] = h_open_file;
  m["import_file"] = h_import_file;
  m["run_command"] = h_run_command;
  m["undo"] = h_undo;
  m["redo"] = h_redo;
}
