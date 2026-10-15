#include <algorithm>
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
    out["note"] = "combine 不是 none：新体已并入/切除/求交到现有实体，见 shapes。";
  }
  out["shapes"] = inq_shape_ids();
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
