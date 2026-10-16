#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cmath>
#include "handlers.h"
#include "../encoding.h"
#include "../errors.h"

svxPoint to_point(const json& v, const char* name) {
  if (!v.is_array() || v.size() != 3 || !v[0].is_number() || !v[1].is_number() || !v[2].is_number())
    invalid_input(std::string(name) + " 必须是 [x, y, z] 三个数字");
  svxPoint p{v[0].get<double>(), v[1].get<double>(), v[2].get<double>()};
  if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) invalid_input(std::string(name) + " 含非有限数值");
  return p;
}

svxVector to_vector(const json& v, const char* name) {
  svxPoint p = to_point(v, name);
  return svxVector{p.x, p.y, p.z};
}

json from_point(const svxPoint& p) { return json::array({p.x, p.y, p.z}); }

evxBoolType parse_combine(const json& params) {
  std::string c = params.value("combine", std::string("none"));
  if (c == "none") return VX_BOOL_NONE;
  if (c == "add") return VX_BOOL_ADD;
  if (c == "remove") return VX_BOOL_REMOVE;
  if (c == "intersect") return VX_BOOL_INTERSECT;
  invalid_input("combine 取值无效: " + c, "可选: none, add, remove, intersect");
}

// ---- undo transactions -----------------------------------------------------------------------------------------
// cvxXnUndoCount() is not reliable in a never-saved file (it lags behind the real log, so bundling "from the count"
// merged the whole history into one step). Markers are the documented tool: cvxUndoRedoMarker flushes the log and adds a
// uniquely named transaction. Every modeling call is bracketed by a start and an end marker, and undo/redo go through
// cvxUndoRedoTo(start) / cvxUndoRedoTo(end), so one call is exactly one step whatever ZW3D logs inside it.
struct TxRecord {
  std::string start, end;
};
static std::vector<TxRecord> g_done, g_undone;
static std::string g_tx_scope;

static void tx_check_scope() {
  std::string scope = current_file_utf8() + "|" + current_root_utf8();
  if (scope != g_tx_scope) {  // another file/part became active: our marker names no longer apply
    g_done.clear();
    g_undone.clear();
    g_tx_scope = scope;
  }
}

static bool tx_marker(std::string& out) {
  vxName name;
  std::memset(name, 0, sizeof(name));
  if (cvxUndoRedoMarker(name) != 0) return false;
  out = name;
  return true;
}

static void restore_root(const std::string& root_utf8) {
  if (root_utf8.empty() || current_root_utf8() == root_utf8) return;
  vxRootName r;
  std::memset(r, 0, sizeof(r));
  std::string a = utf8_to_ansi(root_utf8);
  if (a.size() >= sizeof(r)) return;
  std::memcpy(r, a.c_str(), a.size() + 1);
  cvxRootActivate(r);  // a failed command can leave no root active; later calls would then fail with ROOT_ACT_FAIL
}

bool tx_undo_redo(bool undo, bool* used_markers) {
  tx_check_scope();
  *used_markers = true;
  auto to_name = [](const std::string& s, vxName& n) { std::memset(n, 0, sizeof(n)); std::memcpy(n, s.c_str(), std::min(s.size(), sizeof(n) - 1)); };
  if (undo && !g_done.empty()) {
    TxRecord rec = g_done.back();
    vxName n;
    to_name(rec.start, n);
    if (cvxUndoRedoTo(1, n) != 0) return false;
    g_done.pop_back();
    g_undone.push_back(rec);
    return true;
  }
  if (!undo && !g_undone.empty()) {
    TxRecord rec = g_undone.back();
    vxName n;
    to_name(rec.end, n);
    if (cvxUndoRedoTo(0, n) != 0) return false;
    g_undone.pop_back();
    g_done.push_back(rec);
    return true;
  }
  *used_markers = false;  // nothing of ours to undo/redo (e.g. plugin reloaded): fall back to a raw single step
  return cvxUndoRedo(undo ? 1 : 0) == 0;
}

int tx_steps_available() { return (int)g_done.size(); }

bool is_modeling_method(const std::string& m) {
  static const char* names[] = {"create_box", "create_cylinder", "create_sphere", "extrude_polygon", "revolve_polygon",
                                "extrude_profile", "revolve_profile", "sketch_extrude", "sketch_revolve", "boolean",
                                "fillet_edges", "chamfer_edges", "shell_shape", "create_hole", "pattern_shape", "pattern_feature",
                                "mirror_shape", "mirror_feature", "draft_faces", "sweep", "loft", "set_variable",
                                "add_variable", "set_sketch_variable", "set_feature_value"};
  for (const char* n : names) if (m == n) return true;
  return false;
}

json invoke_handler(const std::string& method, Handler h, const json& params) {
  if (!is_modeling_method(method)) return h(params);
  tx_check_scope();
  std::string root = current_root_utf8();
  std::string start;
  bool marked = tx_marker(start);
  json out;
  try {
    out = h(params);
  } catch (...) {
    if (marked) {
      vxName n;
      std::memset(n, 0, sizeof(n));
      std::memcpy(n, start.c_str(), std::min(start.size(), sizeof(n) - 1));
      cvxUndoRedoTo(1, n);  // undo everything this call created (curves, sketches, partial features)
    }
    restore_root(root);
    throw;
  }
  restore_root(root);
  std::string end;
  if (marked && tx_marker(end)) {
    g_done.push_back({start, end});
    g_undone.clear();  // a new operation invalidates the redo history
  }
  return out;
}

DispGuard::DispGuard() { cvxDispState(0, state); }
DispGuard::~DispGuard() {
  cvxDispState(1, state);  // Option 1 is the safe restore after errors (see zwapi_display.h)
  cvxDispRedraw();
}

std::vector<int> inq_shape_ids() {
  int n = 0;
  int* ids = nullptr;
  check_api(cvxPartInqShapes(nullptr, nullptr, &n, &ids), "cvxPartInqShapes");
  std::vector<int> out(ids, ids + n);
  if (ids) cvxMemFree(reinterpret_cast<void**>(&ids));
  return out;
}

json bbox_json(const svxBndBox& b) {
  json mn = {b.X.min, b.Y.min, b.Z.min};
  json mx = {b.X.max, b.Y.max, b.Z.max};
  return {{"min", mn}, {"max", mx}, {"size", {b.X.max - b.X.min, b.Y.max - b.Y.min, b.Z.max - b.Z.min}}};
}

void require_shape(int id) {
  auto ids = inq_shape_ids();
  if (std::find(ids.begin(), ids.end(), id) == ids.end())
    invalid_input("shape_id " + std::to_string(id) + " 不存在", "调用 list_shapes 查看当前有效 id（布尔运算会消耗工具体）。");
}

json describe_shape(int id) {
  svxBndBox box{};
  check_api(cvxPartInqShapeBox(id, nullptr, &box), "cvxPartInqShapeBox");
  json out = {{"shape_id", id}, {"bounding_box", bbox_json(box)}, {"volume", nullptr}, {"area", nullptr}};
  svxMassProp mp{};
  if (cvxPartInqShapeMass(id, 0.0, &mp) == 0) {  // best effort: bbox alone is still useful
    out["volume"] = mp.Volume;
    out["area"] = mp.Area;
  }
  return out;
}

json describe_shape_or_null(int id) {
  auto ids = inq_shape_ids();
  if (std::find(ids.begin(), ids.end(), id) == ids.end()) return nullptr;
  return describe_shape(id);
}

json creation_result(evxBoolType combine, int id) {
  json out = json::object();
  if (combine == VX_BOOL_NONE) {
    if (id <= 0) throw ApiError("NEW_OBJ_FAIL", "未能取得新生成实体的 id", "调用 list_shapes 查看结果。", ZW_API_GET_NEW_OBJ_FAIL);
    out = describe_shape(id);
  } else {
    out["shape_id"] = nullptr;
    out["note"] = "combine 不是 none：新体已并入/切除/求交到现有实体，见 shapes / shape_details。";
  }
  out["shapes"] = inq_shape_ids();
  if (combine != VX_BOOL_NONE) {
    json details = json::array();
    for (int sid : out["shapes"].get<std::vector<int>>()) details.push_back(describe_shape(sid));
    out["shape_details"] = details;  // bbox + volume of everything that exists now, so the result can be checked at once
  }
  return out;
}

std::string current_file_utf8() {
  char buf[512] = {0};
  cvxFileInqActive(buf, sizeof(buf));
  return ansi_to_utf8(buf);
}

std::string current_root_utf8() {
  char buf[256] = {0};
  cvxRootInqActive(buf, sizeof(buf));
  return ansi_to_utf8(buf);
}

std::vector<std::string> shapes_signature() {
  auto r = [](double x) { return std::fabs(x) < 5e-4 ? 0.0 : x; };  // avoids "-0.000" vs "0.000" noise
  std::vector<std::string> sig;
  for (int id : inq_shape_ids()) {
    svxBndBox b{};
    if (cvxPartInqShapeBox(id, nullptr, &b) != 0) continue;
    svxMassProp mp{};
    double vol = 0, area = 0;
    if (cvxPartInqShapeMass(id, 0.0, &mp) == 0) { vol = mp.Volume; area = mp.Area; }
    char buf[320];
    std::snprintf(buf, sizeof(buf), "%.3f|%.3f|%.3f,%.3f,%.3f|%.3f,%.3f,%.3f", r(vol), r(area), r(b.X.min), r(b.Y.min), r(b.Z.min), r(b.X.max), r(b.Y.max), r(b.Z.max));
    sig.push_back(buf);
  }
  std::sort(sig.begin(), sig.end());
  return sig;
}
