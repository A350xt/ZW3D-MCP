// Modeling handlers: primitives, profile features, boolean, fillet.
// All use the cvx* interfaces (deprecated in 2025 but still shipped); see docs/api_notes.md.
#include <algorithm>
#include <cmath>
#include "handlers.h"
#include "../errors.h"

namespace {

json h_create_box(const json& p) {
  evxBoolType combine = parse_combine(p);
  svxPoint c = to_point(p.at("center"), "center");
  svxPoint s = to_point(p.at("size"), "size");
  if (s.x <= 0 || s.y <= 0 || s.z <= 0) invalid_input("长方体尺寸必须为正", "单位毫米，三个方向都要 > 0。");
  DispGuard guard;
  svxBoxData box;
  check_api(cvxPartBoxInit(&box), "cvxPartBoxInit");
  box.Combine = combine;
  box.Center = c;  // svxBoxData.Center is the centre of the box volume
  box.X = s.x;
  box.Y = s.y;
  box.Z = s.z;
  int id = 0;
  check_api(cvxPartBox(&box, &id), "cvxPartBox");
  return creation_result(combine, id);
}

json h_create_cylinder(const json& p) {
  evxBoolType combine = parse_combine(p);
  svxPoint c = to_point(p.at("base_center"), "base_center");
  double r = p.at("radius").get<double>(), h = p.at("height").get<double>();
  if (!(r > 0) || !(h > 0)) invalid_input("圆柱半径和高度必须为正");
  DispGuard guard;
  svxCylData cyl;
  check_api(cvxPartCylInit(&cyl), "cvxPartCylInit");
  cyl.Combine = combine;
  cyl.Center = c;  // centre of the bottom face; Length runs along +Z of the XY plane (idPlane = 0)
  cyl.Radius = r;
  cyl.Length = h;
  int id = 0;
  check_api(cvxPartCyl(&cyl, &id), "cvxPartCyl");
  return creation_result(combine, id);
}

json h_create_sphere(const json& p) {
  evxBoolType combine = parse_combine(p);
  svxPoint c = to_point(p.at("center"), "center");
  double r = p.at("radius").get<double>();
  if (!(r > 0)) invalid_input("球半径必须为正");
  DispGuard guard;
  svxSphereData sph;
  check_api(cvxPartSphereInit(&sph), "cvxPartSphereInit");
  sph.Combine = combine;
  sph.Center = c;
  sph.Radius = r;
  int id = 0;
  check_api(cvxPartSphere(&sph, &id), "cvxPartSphere");
  return creation_result(combine, id);
}

// Closed polyline -> curve list id. Lines stay in the part (same as the vendor example).
int make_profile(const json& pts_json) {
  if (!pts_json.is_array() || pts_json.size() < 3) invalid_input("轮廓至少需要 3 个顶点");
  std::vector<svxPoint> pts;
  for (size_t i = 0; i < pts_json.size(); ++i) pts.push_back(to_point(pts_json[i], "points[i]"));
  std::vector<int> lines(pts.size());
  for (size_t i = 0; i < pts.size(); ++i) {
    svxPoint a = pts[i], b = pts[(i + 1) % pts.size()];
    check_api(cvxPartLine2pt(&a, &b, &lines[i]), "cvxPartLine2pt");
  }
  int list = 0;
  check_api(cvxPartCrvList((int)lines.size(), lines.data(), &list), "cvxPartCrvList");
  return list;
}

json h_extrude_polygon(const json& p) {
  evxBoolType combine = parse_combine(p);
  double dist = p.at("distance").get<double>();
  double start = p.value("start", 0.0);
  if (!(dist > start)) invalid_input("distance 必须大于 start");
  DispGuard guard;
  int profile = make_profile(p.at("points"));
  svxExtrudeData ext;
  check_api(cvxPartExtrudeInit(&ext), "cvxPartExtrudeInit");
  ext.Combine = combine;
  ext.idProfile = profile;
  ext.StartType = VX_INPU_VALUE;
  ext.EndType = VX_INPU_VALUE;
  ext.Start = start;
  ext.End = dist;
  ext.Draft = p.value("draft_angle", 0.0);
  if (p.contains("direction") && !p["direction"].is_null()) {
    ext.UseDirection = 1;
    ext.Direction = to_vector(p["direction"], "direction");
  }
  ext.OffsetTyp = VX_OFFSET_NONE;
  int id = 0;
  check_api(cvxPartExtrude(&ext, &id), "cvxPartExtrude");
  return creation_result(combine, id);
}

json h_revolve_polygon(const json& p) {
  evxBoolType combine = parse_combine(p);
  double angle = p.at("angle").get<double>(), start = p.value("start_angle", 0.0);
  if (!(angle > start) || angle - start > 360.0) invalid_input("angle - start_angle 必须在 (0, 360] 内");
  DispGuard guard;
  int profile = make_profile(p.at("points"));
  svxRevolveData rev;
  check_api(cvxPartRevolveInit(&rev), "cvxPartRevolveInit");
  rev.Combine = combine;
  rev.idProfile = profile;
  rev.Axis.Pnt = to_point(p.at("axis_point"), "axis_point");
  rev.Axis.Dir = to_vector(p.at("axis_direction"), "axis_direction");
  rev.StartType = VX_INPU_VALUE;
  rev.EndType = VX_INPU_VALUE;
  rev.Start = start;
  rev.End = angle;
  rev.OffsetTyp = VX_OFFSET_NONE;
  int id = 0;
  check_api(cvxPartRevolve(&rev, &id), "cvxPartRevolve");
  return creation_result(combine, id);
}

json h_boolean(const json& p) {
  std::string op = p.at("operation").get<std::string>();
  evxBoolType type;
  if (op == "add") type = VX_BOOL_ADD;
  else if (op == "subtract") type = VX_BOOL_REMOVE;
  else if (op == "intersect") type = VX_BOOL_INTERSECT;
  else invalid_input("operation 取值无效: " + op, "可选: add, subtract, intersect");
  int base = p.at("base_id").get<int>();
  std::vector<int> tools = p.at("tool_ids").get<std::vector<int>>();
  if (tools.empty()) invalid_input("tool_ids 不能为空");
  if (std::find(tools.begin(), tools.end(), base) != tools.end()) invalid_input("base_id 不能同时出现在 tool_ids 中");
  require_shape(base);
  for (int t : tools) require_shape(t);
  DispGuard guard;
  check_api(cvxPartBool(type, base, (int)tools.size(), tools.data(), p.value("keep_tools", false) ? 1 : 0), "cvxPartBool");
  // The base id may change after a boolean, so report what exists now.
  json out = {{"shape", describe_shape_or_null(base)}, {"shapes", inq_shape_ids()}};
  if (out["shape"].is_object()) {
    out["shape_id"] = base;
    out["bounding_box"] = out["shape"]["bounding_box"];
    out["volume"] = out["shape"]["volume"];
    out["area"] = out["shape"]["area"];
  }
  return out;
}

json h_fillet_edges(const json& p) {
  int shape = p.at("shape_id").get<int>();
  double r = p.at("radius").get<double>();
  if (!(r > 0)) invalid_input("圆角半径必须为正");
  require_shape(shape);
  std::vector<int> edges;
  if (p.contains("edge_ids") && !p["edge_ids"].is_null()) {
    edges = p["edge_ids"].get<std::vector<int>>();
  } else {
    int n = 0;
    int* ids = nullptr;
    check_api(cvxPartInqShapeEdges(shape, &n, &ids), "cvxPartInqShapeEdges");
    edges.assign(ids, ids + n);
    if (ids) cvxMemFree(reinterpret_cast<void**>(&ids));
  }
  if (edges.empty()) invalid_input("没有可倒圆角的边");
  DispGuard guard;
  check_api(cvxPartFillet((int)edges.size(), edges.data(), r), "cvxPartFillet");
  json out = {{"edge_count", edges.size()}, {"shapes", inq_shape_ids()}};
  json d = describe_shape_or_null(shape);
  if (d.is_object()) {
    for (auto it = d.begin(); it != d.end(); ++it) out[it.key()] = it.value();
  }
  return out;
}

}  // namespace

void register_part_handlers(std::map<std::string, Handler>& m) {
  m["create_box"] = h_create_box;
  m["create_cylinder"] = h_create_cylinder;
  m["create_sphere"] = h_create_sphere;
  m["extrude_polygon"] = h_extrude_polygon;
  m["revolve_polygon"] = h_revolve_polygon;
  m["boolean"] = h_boolean;
  m["fillet_edges"] = h_fillet_edges;
}
