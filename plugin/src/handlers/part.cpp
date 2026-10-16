// Modeling handlers: primitives, profile features, boolean, fillet.
// All use the cvx* interfaces (deprecated in 2025 but still shipped); see docs/api_notes.md.
#include <algorithm>
#include <cmath>
#include <cstring>
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

svxPntOnEntPath free_point(const svxPoint& p) {
  svxPntOnEntPath e;
  std::memset(&e, 0, sizeof(e));
  e.pntType = VX_FREE_POINT;
  e.pnt = p;
  return e;
}

// One profile segment -> curve id. Arcs/circles are defined by 3 free points, so they work in any plane.
int add_segment(const json& s) {
  std::string type = s.at("type").get<std::string>();
  int id = 0;
  if (type == "line") {
    svxPoint a = to_point(s.at("start"), "start"), b = to_point(s.at("end"), "end");
    check_api(cvxPartLine2pt(&a, &b, &id), "cvxPartLine2pt");
  } else if (type == "arc") {
    svxArcData arc;
    check_api(cvxPartArcInit(&arc), "cvxPartArcInit");
    arc.arcType = VX_ARC_THROUGH;  // two end points + a third point the arc passes through
    arc.startPnt = free_point(to_point(s.at("start"), "start"));
    arc.endPnt = free_point(to_point(s.at("end"), "end"));
    arc.centerPnt = free_point(to_point(s.at("through"), "through"));
    check_api(cvxPartArc(&arc, &id), "cvxPartArc");
  } else if (type == "circle") {
    svxPoint c = to_point(s.at("center"), "center");
    double r = s.at("radius").get<double>();
    if (!(r > 0)) invalid_input("圆半径必须为正");
    svxVector n = s.contains("normal") ? to_vector(s["normal"], "normal") : svxVector{0, 0, 1};
    double nl = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    if (nl < 1e-12) invalid_input("normal 不能为零向量");
    n = {n.x / nl, n.y / nl, n.z / nl};
    // u: any unit vector perpendicular to n; v = n x u
    svxVector t = std::fabs(n.z) < 0.9 ? svxVector{0, 0, 1} : svxVector{1, 0, 0};
    svxVector u{n.y * t.z - n.z * t.y, n.z * t.x - n.x * t.z, n.x * t.y - n.y * t.x};
    double ul = std::sqrt(u.x * u.x + u.y * u.y + u.z * u.z);
    u = {u.x / ul, u.y / ul, u.z / ul};
    svxVector v{n.y * u.z - n.z * u.y, n.z * u.x - n.x * u.z, n.x * u.y - n.y * u.x};
    svxCircleData circ;
    check_api(cvxPartCircleInit(&circ), "cvxPartCircleInit");
    circ.type = VX_CIRCLE_THROUGH;  // 3 boundary points define the circle and its plane
    for (int k = 0; k < 3; ++k) {
      double a = k * 2.0943951023931953;  // 120 deg
      double ca = std::cos(a), sa = std::sin(a);
      circ.bndPnts[k] = free_point({c.x + r * (ca * u.x + sa * v.x), c.y + r * (ca * u.y + sa * v.y), c.z + r * (ca * u.z + sa * v.z)});
    }
    check_api(cvxPartCircle(&circ, &id), "cvxPartCircle");
  } else {
    invalid_input("未知轮廓段类型: " + type, "可选: line, arc, circle");
  }
  return id;
}

// Profile (closed polygon `points`, or `segments` of lines/arcs/circles) -> curve list id.
// The curves stay in the part (same as the vendor example).
int make_profile(const json& p) {
  std::vector<int> curves;
  if (p.contains("segments") && p["segments"].is_array()) {
    if (p["segments"].empty()) invalid_input("segments 不能为空");
    for (const auto& s : p["segments"]) curves.push_back(add_segment(s));
  } else {
    const json& pts_json = p.at("points");
    if (!pts_json.is_array() || pts_json.size() < 3) invalid_input("轮廓至少需要 3 个顶点");
    std::vector<svxPoint> pts;
    for (size_t i = 0; i < pts_json.size(); ++i) pts.push_back(to_point(pts_json[i], "points[i]"));
    curves.resize(pts.size());
    for (size_t i = 0; i < pts.size(); ++i) {
      svxPoint a = pts[i], b = pts[(i + 1) % pts.size()];
      check_api(cvxPartLine2pt(&a, &b, &curves[i]), "cvxPartLine2pt");
    }
  }
  int list = 0;
  check_api(cvxPartCrvList((int)curves.size(), curves.data(), &list), "cvxPartCrvList");
  return list;
}

json h_extrude_polygon(const json& p) {
  evxBoolType combine = parse_combine(p);
  double dist = p.at("distance").get<double>();
  double start = p.value("start", 0.0);
  if (!(dist > start)) invalid_input("distance 必须大于 start");
  DispGuard guard;
  int profile = make_profile(p);
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
  int profile = make_profile(p);
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

std::vector<int> edges_of(const json& p, int shape) {
  if (p.contains("edge_ids") && !p["edge_ids"].is_null()) return p["edge_ids"].get<std::vector<int>>();
  int n = 0;
  int* ids = nullptr;
  check_api(cvxPartInqShapeEdges(shape, &n, &ids), "cvxPartInqShapeEdges");
  std::vector<int> edges(ids, ids + n);
  if (ids) cvxMemFree(reinterpret_cast<void**>(&ids));
  return edges;
}

void merge_shape_info(json& out, int shape) {
  json d = describe_shape_or_null(shape);
  if (d.is_object()) for (auto it = d.begin(); it != d.end(); ++it) out[it.key()] = it.value();
}

json h_chamfer_edges(const json& p) {
  int shape = p.at("shape_id").get<int>();
  double d1 = p.at("distance").get<double>();
  double d2 = p.contains("distance2") && !p["distance2"].is_null() ? p["distance2"].get<double>() : 0.0;
  if (!(d1 > 0)) invalid_input("倒角距离必须为正");
  if (d2 < 0) invalid_input("distance2 不能为负");
  require_shape(shape);
  std::vector<int> edges = edges_of(p, shape);
  if (edges.empty()) invalid_input("没有可倒角的边");
  DispGuard guard;
  svxChamFlltData ch;
  check_api(cvxPartChamferAllInit(&ch), "cvxPartChamferAllInit");
  ch.type = d2 > 0 ? VX_CHAMFLLT_ASYMMETRIC : VX_CHAMFLLT_SYMMETRIC;
  ch.nEdges = (int)edges.size();
  ch.idEdge = edges.data();
  ch.setback1 = d1;
  ch.setback2 = d2;
  int id = 0;
  check_api(cvxPartChamferAll(&ch, &id), "cvxPartChamferAll");
  json out = {{"edge_count", edges.size()}, {"shapes", inq_shape_ids()}};
  merge_shape_info(out, shape);
  return out;
}

json h_shell_shape(const json& p) {
  int shape = p.at("shape_id").get<int>();
  double t = p.at("thickness").get<double>();
  if (!(t > 0)) invalid_input("壁厚必须为正");
  std::vector<int> open = p.contains("open_face_ids") && !p["open_face_ids"].is_null()
                              ? p["open_face_ids"].get<std::vector<int>>() : std::vector<int>();
  require_shape(shape);
  DispGuard guard;
  svxShellData sh;
  check_api(cvxPartShellInit(&sh), "cvxPartShellInit");
  sh.idShape = shape;
  // Measured on ZW3D 2025 SP2: a positive dThick grows the wall OUTWARD, so inward (default) uses a negative value.
  sh.dThick = p.value("inward", true) ? -t : t;
  sh.cntOpenFace = (int)open.size();
  sh.pIdOpenFace = open.empty() ? nullptr : open.data();
  int id = 0;
  check_api(cvxPartShell(&sh, &id), "cvxPartShell");
  json out = {{"shapes", inq_shape_ids()}};
  merge_shape_info(out, shape);
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
  m["chamfer_edges"] = h_chamfer_edges;
  m["shell_shape"] = h_shell_shape;
  m["extrude_profile"] = h_extrude_polygon;
  m["revolve_profile"] = h_revolve_polygon;
}
