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

// ---------------------------------------------------------------------------------------------------------------
// Sketches: a real sketch feature in the history tree (geometry + coincident constraints + optional dimensions),
// used as the profile of an extrude/revolve so the part can be edited later from the sketch.
// ---------------------------------------------------------------------------------------------------------------
svxMatrix sketch_frame(const json& plane) {
  svxPoint o = to_point(plane.at("origin"), "plane.origin");
  svxVector x = to_vector(plane.at("x_axis"), "plane.x_axis"), y = to_vector(plane.at("y_axis"), "plane.y_axis");
  auto len = [](const svxVector& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); };
  if (len(x) < 1e-12 || len(y) < 1e-12) invalid_input("plane 的坐标轴不能为零向量");
  x = {x.x / len(x), x.y / len(x), x.z / len(x)};
  svxVector z{x.y * y.z - x.z * y.y, x.z * y.x - x.x * y.z, x.x * y.y - x.y * y.x};
  if (len(z) < 1e-9) invalid_input("plane 的 x_axis 与 y_axis 平行");
  z = {z.x / len(z), z.y / len(z), z.z / len(z)};
  y = {z.y * x.z - z.z * x.y, z.z * x.x - z.x * x.z, z.x * x.y - z.y * x.x};  // make y exactly orthogonal
  svxMatrix m;
  std::memset(&m, 0, sizeof(m));
  m.identity = 0;
  m.xx = x.x; m.xy = x.y; m.xz = x.z;  // first column = x direction (field names are <axis><component>)
  m.yx = y.x; m.yy = y.y; m.yz = y.z;
  m.zx = z.x; m.zy = z.y; m.zz = z.z;
  m.xt = o.x; m.yt = o.y; m.zt = o.z;
  return m;
}

svxPoint2 to_point2(const json& v, const char* name) {
  if (!v.is_array() || v.size() != 2 || !v[0].is_number() || !v[1].is_number()) invalid_input(std::string(name) + " 必须是 [x, y] 两个数字");
  return svxPoint2{v[0].get<double>(), v[1].get<double>()};
}

// Leaves sketch edit mode when it goes out of scope (also on exceptions), so ZW3D is never left inside a sketch.
struct SketchEditGuard {
  bool active = false;
  void leave() {
    if (active) {
      active = false;
      cvxRootExit();
    }
  }
  ~SketchEditGuard() { leave(); }
};

// Best-effort driving dimension; failures are counted, never fatal.
bool add_dimension(evxConsDimType type, int sub, svxPoint a, svxPoint b, svxPoint text, int ent1, int ent2) {
  svxConsDim d;
  std::memset(&d, 0, sizeof(d));
  d.firstPnt = a;
  d.secondPnt = b;
  d.textPnt = text;
  d.type = sub;
  d.firstEnt = ent1;
  d.secondEnt = ent2;
  return cvxSkAddConsDim(type, &d) == 0;
}

int build_sketch(const json& p, json& info) {
  svxMatrix frame = sketch_frame(p.at("plane"));
  const json& segs = p.at("segments");
  if (!segs.is_array() || segs.empty()) invalid_input("segments 不能为空");
  bool want_dims = p.value("dimensions", true);

  SketchEditGuard edit;
  int sk = 0;
  check_api(cvxPartSkNew(&frame, &sk), "cvxPartSkNew");
  edit.active = true;

  int dims_ok = 0, dims_fail = 0;
  // Outer loop first, then any inner loops ("holes"): they are just more closed loops in the same sketch.
  std::vector<json> all_segments(segs.begin(), segs.end());
  if (p.contains("inner_loops") && p["inner_loops"].is_array())
    for (const auto& loop : p["inner_loops"])
      for (const auto& s : loop) all_segments.push_back(s);
  for (const auto& s : all_segments) {
    std::string type = s.at("type").get<std::string>();
    int id = 0;
    if (type == "line") {
      svxPoint2 a = to_point2(s.at("start"), "start"), b = to_point2(s.at("end"), "end");
      check_api(cvxSkLine2pt(&a, &b, &id), "cvxSkLine2pt");
      if (want_dims) {
        double dx = b.x - a.x, dy = b.y - a.y, l = std::sqrt(dx * dx + dy * dy);
        svxPoint mid{(a.x + b.x) / 2 - dy / l * 5, (a.y + b.y) / 2 + dx / l * 5, 0};
        (add_dimension(VX_SK_CONSDIM_LINEAR, 3, {a.x, a.y, 0}, {b.x, b.y, 0}, mid, id, id) ? dims_ok : dims_fail)++;
      }
    } else if (type == "arc") {
      svxPoint2 a = to_point2(s.at("start"), "start"), b = to_point2(s.at("end"), "end"), m = to_point2(s.at("through"), "through");
      check_api(cvxSkArc3pt(&a, &b, &m, &id), "cvxSkArc3pt");  // note: (Start, End, Mid)
    } else if (type == "circle") {
      svxPoint2 c = to_point2(s.at("center"), "center");
      double r = s.at("radius").get<double>();
      if (!(r > 0)) invalid_input("圆半径必须为正");
      check_api(cvxSkCirRad(&c, r, &id), "cvxSkCirRad");
      if (want_dims)
        (add_dimension(VX_SK_CONSDIM_RADIAL, 2, {c.x + r, c.y, 0}, {0, 0, 0}, {c.x + r * 1.5, c.y + r * 0.5, 0}, id, 0) ? dims_ok : dims_fail)++;
    } else {
      invalid_input("未知草图段类型: " + type, "可选: line, arc, circle");
    }
  }
  cvxSkAutoAddCoinCons(-1);  // join touching end points (result intentionally ignored: constraints are a bonus)
  edit.leave();
  info["sketch_id"] = sk;
  info["dimensions_added"] = dims_ok;
  info["dimensions_failed"] = dims_fail;
  return sk;
}

json extrude_impl(const json& p, bool sketch) {
  json info = json::object();
  evxBoolType combine = parse_combine(p);
  double dist = p.at("distance").get<double>();
  double start = p.value("start", 0.0);
  if (!(dist > start)) invalid_input("distance 必须大于 start");
  DispGuard guard;
  int profile = sketch ? build_sketch(p, info) : make_profile(p);
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
  json out = creation_result(combine, id);
  for (auto it = info.begin(); it != info.end(); ++it) out[it.key()] = it.value();
  return out;
}
json h_extrude_polygon(const json& p) { return extrude_impl(p, false); }
json h_sketch_extrude(const json& p) { return extrude_impl(p, true); }

json revolve_impl(const json& p, bool sketch) {
  json info = json::object();
  evxBoolType combine = parse_combine(p);
  double angle = p.at("angle").get<double>(), start = p.value("start_angle", 0.0);
  if (!(angle > start) || angle - start > 360.0) invalid_input("angle - start_angle 必须在 (0, 360] 内");
  DispGuard guard;
  int profile = sketch ? build_sketch(p, info) : make_profile(p);
  svxRevolveData rev;
  check_api(cvxPartRevolveInit(&rev), "cvxPartRevolveInit");
  rev.Combine = combine;
  rev.idProfile = profile;
  if (sketch) {  // axis given in sketch coordinates: map it to global with the sketch frame
    svxMatrix f = sketch_frame(p.at("plane"));
    svxPoint2 ap = to_point2(p.at("axis_point"), "axis_point"), ad = to_point2(p.at("axis_direction"), "axis_direction");
    if (ad.x * ad.x + ad.y * ad.y < 1e-18) invalid_input("axis_direction 不能为零向量");
    rev.Axis.Pnt = {f.xt + ap.x * f.xx + ap.y * f.yx, f.yt + ap.x * f.xy + ap.y * f.yy, f.zt + ap.x * f.xz + ap.y * f.yz};
    rev.Axis.Dir = {ad.x * f.xx + ad.y * f.yx, ad.x * f.xy + ad.y * f.yy, ad.x * f.xz + ad.y * f.yz};
  } else {
    rev.Axis.Pnt = to_point(p.at("axis_point"), "axis_point");
    rev.Axis.Dir = to_vector(p.at("axis_direction"), "axis_direction");
  }
  rev.StartType = VX_INPU_VALUE;
  rev.EndType = VX_INPU_VALUE;
  rev.Start = start;
  rev.End = angle;
  rev.OffsetTyp = VX_OFFSET_NONE;
  int id = 0;
  check_api(cvxPartRevolve(&rev, &id), "cvxPartRevolve");
  json out = creation_result(combine, id);
  for (auto it = info.begin(); it != info.end(); ++it) out[it.key()] = it.value();
  return out;
}
json h_revolve_polygon(const json& p) { return revolve_impl(p, false); }
json h_sketch_revolve(const json& p) { return revolve_impl(p, true); }

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


// ---------------------------------------------------------------------------------------------------------------
// Part features: hole, pattern, mirror, draft, sweep, loft (read from zwapi_cmd_shape*.h, see docs/api_notes.md)
// ---------------------------------------------------------------------------------------------------------------
std::vector<int> ids_of(const json& v, const char* name) {
  if (!v.is_array()) invalid_input(std::string(name) + " 必须是 id 列表");
  return v.get<std::vector<int>>();
}

json shapes_summary() {
  json details = json::array();
  std::vector<int> ids = inq_shape_ids();
  for (int sid : ids) details.push_back(describe_shape(sid));
  return {{"shapes", ids}, {"shape_details", details}};
}

// Datum plane through `origin` with the given normal. cvxPartPlaneNew(THREE_POINTS) failed on a real ZW3D (-152) for every
// plane, so several creation routes are tried in order and the first that works is used.
int make_datum_plane(const json& plane) {
  svxPoint o = to_point(plane.at("origin"), "plane.origin");
  svxVector n = to_vector(plane.at("normal"), "plane.normal");
  double nl = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
  if (nl < 1e-12) invalid_input("plane.normal 不能为零向量");
  n = {n.x / nl, n.y / nl, n.z / nl};
  svxVector t = std::fabs(n.z) < 0.9 ? svxVector{0, 0, 1} : svxVector{1, 0, 0};
  svxVector u{n.y * t.z - n.z * t.y, n.z * t.x - n.x * t.z, n.x * t.y - n.y * t.x};
  double ul = std::sqrt(u.x * u.x + u.y * u.y + u.z * u.z);
  u = {u.x / ul, u.y / ul, u.z / ul};
  svxVector v{n.y * u.z - n.z * u.y, n.z * u.x - n.x * u.z, n.x * u.y - n.y * u.x};
  int id = 0, last_err = 0;
  std::string tried;
  auto attempt = [&](const char* what, int err) {
    if (err == 0 && id > 0) return true;
    last_err = err ? err : ZW_API_GET_NEW_OBJ_FAIL;
    tried += std::string(what) + "(" + std::to_string(err) + ") ";
    id = 0;
    return false;
  };

  // 1) standard XY / XZ / YZ datum plane with an offset (axis-aligned requests)
  double av[3] = {std::fabs(n.x), std::fabs(n.y), std::fabs(n.z)};
  for (int k = 0; k < 3; ++k) {
    if (av[k] > 1 - 1e-9) {
      svxPlaneData pd;
      std::memset(&pd, 0, sizeof(pd));
      pd.method = k == 2 ? VX_PLANE_XY : k == 1 ? VX_PLANE_XZ : VX_PLANE_YZ;  // normals: Z, Y, X
      pd.inpUnion.xyz.Offset = k == 0 ? o.x : k == 1 ? o.y : o.z;
      if (attempt("standard-plane", cvxPartPlaneNew(&pd, &id))) return id;
    }
  }
  // 2) dynamic plane (position + explicit axes)
  {
    svxNewPlaneData nd;
    if (cvxPartPlaneAddInit(VX_NEW_PLANE_DYNAMIC, &nd) == 0) {
      nd.inpUnion.dynamic.posPnt = o;
      nd.inpUnion.dynamic.xAxis = u;
      nd.inpUnion.dynamic.yAxis = v;
      nd.inpUnion.dynamic.zAxis = n;
      nd.inpUnion.dynamic.autoOrient = 0;
      if (attempt("dynamic", cvxPartPlaneAdd(&nd, &id))) return id;
    }
  }
  // 3) three points through the newer command template
  {
    svxNewPlaneData nd;
    if (cvxPartPlaneAddInit(VX_NEW_PLANE_THREE_POINTS, &nd) == 0) {
      nd.inpUnion.threePnts.orgPnt = o;
      nd.inpUnion.threePnts.xPnt = {o.x + u.x * 10, o.y + u.y * 10, o.z + u.z * 10};
      nd.inpUnion.threePnts.yPnt = {o.x + v.x * 10, o.y + v.y * 10, o.z + v.z * 10};
      if (attempt("three-points-add", cvxPartPlaneAdd(&nd, &id))) return id;
    }
  }
  // 4) the original route
  {
    svxPlaneData pd;
    std::memset(&pd, 0, sizeof(pd));
    pd.method = VX_PLANE_THREE_POINTS;
    pd.inpUnion.threePnts.OrgPnt = o;
    pd.inpUnion.threePnts.XPnt = {o.x + u.x * 10, o.y + u.y * 10, o.z + u.z * 10};
    pd.inpUnion.threePnts.YPnt = {o.x + v.x * 10, o.y + v.y * 10, o.z + v.z * 10};
    if (attempt("three-points-new", cvxPartPlaneNew(&pd, &id))) return id;
  }
  try {
    check_api(last_err, "创建镜像基准面");
  } catch (ApiError& e) {
    e.hint = "已尝试的创建方式及错误码: " + tried + "。可先在中望3D里手动建一个基准面，或改用 create_* 重新建出对称的几何。原提示: " + e.hint;
    throw;
  }
  return 0;
}

// Curve list from 3D segments; also reports the first curve id (loft/sweep reference "a curve + its parent list").
int make_curvelist(const json& segments, int* first_curve) {
  if (!segments.is_array() || segments.empty()) invalid_input("segments 不能为空");
  std::vector<int> curves;
  for (const auto& s : segments) curves.push_back(add_segment(s));
  int list = 0;
  check_api(cvxPartCrvList((int)curves.size(), curves.data(), &list), "cvxPartCrvList");
  if (first_curve) *first_curve = curves[0];
  return list;
}

json h_create_hole(const json& p) {
  int face = p.at("face_id").get<int>();
  const json& pts_json = p.at("points");
  if (!pts_json.is_array() || pts_json.empty()) invalid_input("points 不能为空");
  std::vector<svxPoint> pts;
  for (const auto& q : pts_json) pts.push_back(to_point(q, "points[i]"));
  double dia = p.at("diameter").get<double>();
  bool through = p.value("through", false);
  double depth = p.contains("depth") && !p["depth"].is_null() ? p["depth"].get<double>() : 0.0;
  if (!(dia > 0)) invalid_input("孔径必须为正");
  if (!through && !(depth > 0)) invalid_input("盲孔需要 depth > 0（或设 through=true）");
  std::string kind = p.value("hole_type", std::string("simple"));
  evxHoleType type = kind == "simple" ? VX_HOLE_SIMPLE : kind == "counterbore" ? VX_HOLE_COUNTER_BORE
                   : kind == "countersink" ? VX_HOLE_COUNTER_SUNK : (evxHoleType)-99;
  if ((int)type == -99) invalid_input("hole_type 取值无效: " + kind, "可选: simple, counterbore, countersink");
  DispGuard guard;
  svxHoleData h;
  check_api(cvxPartHoleInit(VX_GENERAL_HOLE, (int)type, &h), "cvxPartHoleInit");
  h.idInsFace = face;
  h.Count = (int)pts.size();
  h.Points = pts.data();
  h.Diameter1 = dia;
  h.Depth1 = depth;
  h.End = through ? VX_THROUGH_ALL : VX_BLIND;
  if (type != VX_HOLE_SIMPLE) {
    h.Diameter2 = p.value("diameter2", dia * 1.6);
    h.Depth2 = p.value("depth2", dia * 0.6);
  }
  int op = 0;
  check_api(cvxPartHole(&h, &op), "cvxPartHole");
  json out = shapes_summary();
  out["feature_id"] = op;
  return out;
}

svxPtnData make_pattern(const json& p, evxBoolType combine) {
  std::string kind = p.at("kind").get<std::string>();
  svxPtnData pd;
  if (kind == "linear") {
    check_api(cvxPtnDataInit(VX_PATTERN_LINEAR, &pd), "cvxPtnDataInit");
    pd.combine = combine;
    pd.ptnData.linear.dir[0] = to_vector(p.at("direction"), "direction");
    pd.ptnData.linear.num[0] = p.at("count").get<int>();
    pd.ptnData.linear.spacing[0] = p.at("spacing").get<double>();
    if (p.contains("direction2") && !p["direction2"].is_null()) {
      pd.ptnData.linear.useSecondDir = 1;
      pd.ptnData.linear.dir[1] = to_vector(p["direction2"], "direction2");
      pd.ptnData.linear.num[1] = p.at("count2").get<int>();
      pd.ptnData.linear.spacing[1] = p.at("spacing2").get<double>();
    }
  } else if (kind == "circular") {
    check_api(cvxPtnDataInit(VX_PATTERN_CIRCULAR, &pd), "cvxPtnDataInit");
    pd.combine = combine;
    pd.ptnData.circular.axis.Pnt = to_point(p.at("axis_point"), "axis_point");
    pd.ptnData.circular.axis.Dir = to_vector(p.at("axis_direction"), "axis_direction");
    pd.ptnData.circular.num[0] = p.at("count").get<int>();
    pd.ptnData.circular.angle = p.at("angle").get<double>();  // degrees between neighbouring instances
  } else {
    invalid_input("kind 取值无效: " + kind, "可选: linear, circular");
  }
  return pd;
}

json h_pattern_shape(const json& p) {
  evxBoolType combine = parse_combine(p);
  int shape = p.at("shape_id").get<int>();
  require_shape(shape);
  svxPtnData pd = make_pattern(p, combine);
  DispGuard guard;
  std::vector<int> before = inq_shape_ids();
  svxPtnGeom g;
  std::memset(&g, 0, sizeof(g));
  g.inpData = &pd;
  int ent = shape, pattern = 0;
  check_api(cvxPartPatternGeom(1, &ent, &g, &pattern), "cvxPartPatternGeom");
  json out = shapes_summary();
  out["pattern_id"] = pattern;
  json added = json::array();
  for (int s : out["shapes"].get<std::vector<int>>())
    if (std::find(before.begin(), before.end(), s) == before.end()) added.push_back(s);
  out["added_shape_ids"] = added;
  return out;
}

json h_pattern_feature(const json& p) {
  evxBoolType combine = parse_combine(p);
  std::vector<int> ftrs = ids_of(p.at("feature_ids"), "feature_ids");
  if (ftrs.empty()) invalid_input("feature_ids 不能为空");
  svxPtnData pd = make_pattern(p, combine);
  DispGuard guard;
  svxPtnFtr f;
  std::memset(&f, 0, sizeof(f));
  f.inpData = &pd;
  int pattern = 0;
  check_api(cvxPartPatternFtr((int)ftrs.size(), ftrs.data(), &f, &pattern), "cvxPartPatternFtr");
  json out = shapes_summary();
  out["pattern_id"] = pattern;
  return out;
}

json h_mirror_shape(const json& p) {
  evxBoolType combine = parse_combine(p);  // none = keep the mirrored copy as a separate body, add = merge into the original
  int shape = p.at("shape_id").get<int>();
  require_shape(shape);
  DispGuard guard;
  int plane = make_datum_plane(p.at("plane"));
  svxMirrorGeom m;
  check_api(cvxPartMirrorGeominit(&m), "cvxPartMirrorGeominit");
  int ent = shape;
  m.numEnts = 1;
  m.idEnts = &ent;
  m.idPlane = plane;
  m.combine = combine;
  if (combine != VX_BOOL_NONE) {
    m.numShapes = 1;
    m.idShapes = &ent;
  }
  m.keep = 1;
  std::vector<int> before = inq_shape_ids();
  std::vector<std::string> sig_before = shapes_signature();
  int n = 0;
  int* ids = nullptr;
  check_api(cvxPartMirrorGeom(&m, &n, &ids), "cvxPartMirrorGeom");
  if (ids) cvxMemFree(reinterpret_cast<void**>(&ids));
  json out = shapes_summary();
  json added = json::array();
  for (int s : out["shapes"].get<std::vector<int>>())
    if (std::find(before.begin(), before.end(), s) == before.end()) added.push_back(s);
  out["added_shape_ids"] = added;
  // The call "succeeds" even when the mirror image coincides with the original (plane through / on the solid): nothing is produced.
  bool nothing = combine == VX_BOOL_NONE ? added.empty() : sig_before == shapes_signature();
  if (nothing)
    throw ApiError("MIRROR_NO_RESULT", "镜像没有生成任何新实体",
                   "镜像平面很可能与原件重合或穿过实体，镜像像与原件完全重叠，被中望3D忽略。换一个不穿过实体的平面（例如偏移的 YZ 平面），"
                   "并检查 plane 的 origin / normal。已回滚。");
  return out;
}

json h_mirror_feature(const json& p) {
  evxBoolType combine = parse_combine(p);
  std::vector<int> ftrs = ids_of(p.at("feature_ids"), "feature_ids");
  if (ftrs.empty()) invalid_input("feature_ids 不能为空");
  DispGuard guard;
  int plane = make_datum_plane(p.at("plane"));
  svxMirrorFtr m;
  check_api(cvxPartMirrorFtrInit(&m), "cvxPartMirrorFtrInit");
  m.numFtrs = (int)ftrs.size();
  m.idFtrs = ftrs.data();
  m.idPlane = plane;
  m.combine = combine;
  m.keep = 1;
  int id = 0;
  std::vector<std::string> sig_before = shapes_signature();
  check_api(cvxPartMirrorFtr(&m, &id), "cvxPartMirrorFtr");
  json out = shapes_summary();
  out["feature_id"] = id;
  if (sig_before == shapes_signature())
    throw ApiError("MIRROR_NO_RESULT", "特征镜像没有改变任何实体",
                   "镜像平面很可能与特征重合或特征本身关于该平面对称。换一个平面；已回滚。");
  return out;
}

json h_draft_faces(const json& p) {
  std::vector<int> faces = ids_of(p.at("face_ids"), "face_ids");
  std::vector<int> neutral = ids_of(p.at("neutral_face_ids"), "neutral_face_ids");
  if (faces.empty() || neutral.empty()) invalid_input("face_ids 与 neutral_face_ids 都不能为空");
  double angle = p.at("angle").get<double>();
  if (!(angle > 0) || angle >= 89) invalid_input("拔模角必须在 (0, 89) 度之间");
  svxVector dir = to_vector(p.at("pull_direction"), "pull_direction");
  DispGuard guard;
  svxDraftData d;
  check_api(cvxPartDraftInit(VX_FACE_DRAFT, &d), "cvxPartDraftInit");
  d.Type = VX_STATION_SYM_FACE_DRAFT;  // faces tilt about the neutral (stationary) faces
  d.iCntDataList = (int)faces.size();
  d.idEnt = faces.data();
  d.AngleA1 = angle;
  d.StationEnt.stationaryFaces.cnt = (int)neutral.size();
  d.StationEnt.stationaryFaces.idEnt = neutral.data();
  d.DirP = dir;
  int id = 0;
  check_api(cvxPartDraft(&d, &id), "cvxPartDraft");
  json out = shapes_summary();
  out["feature_id"] = id;
  return out;
}

json h_sweep(const json& p) {
  evxBoolType combine = parse_combine(p);
  DispGuard guard;
  int profile = make_profile(json{{"segments", p.at("profile")}});
  int first = 0;
  int path_list = make_curvelist(p.at("path"), &first);
  svxSweepData sw;
  check_api(cvxPartSweepInit(&sw), "cvxPartSweepInit");
  sw.Combine = combine;
  sw.idProfile = profile;
  sw.idPath = first;
  sw.idParent = path_list;
  sw.UseParent = 1;  // the whole curve list is the path, not just its first curve
  sw.OffsetTyp = VX_OFFSET_NONE;
  int id = 0;
  check_api(cvxPartSweep(&sw, nullptr, &id), "cvxPartSweep");
  return creation_result(combine, id);
}

json h_loft(const json& p) {
  evxBoolType combine = parse_combine(p);
  const json& sections = p.at("sections");
  if (!sections.is_array() || sections.size() < 2) invalid_input("放样至少需要 2 个截面");
  DispGuard guard;
  std::vector<svxLoftProfile> profs(sections.size());
  std::memset(profs.data(), 0, sizeof(svxLoftProfile) * profs.size());
  for (size_t i = 0; i < sections.size(); ++i) {
    int first = 0;
    int list = make_curvelist(sections[i], &first);
    profs[i].idParent = list;
    profs[i].idProfile = first;
    profs[i].profDir = 0;
    profs[i].useStartPnt = 0;
  }
  svxLoftData l;
  check_api(cvxPartLoftInit(&l), "cvxPartLoftInit");
  l.combine = combine;
  l.type = VX_LOFT_PROFS;
  l.numProfs = (int)profs.size();
  l.profs = profs.data();
  int id = 0;
  check_api(cvxPartLoft(&l, &id), "cvxPartLoft");
  return creation_result(combine, id);
}

json h_list_features(const json&) {
  int n = 0;
  int* ids = nullptr;
  check_api(cvxPartInqFtrList(&n, &ids), "cvxPartInqFtrList");
  std::vector<int> v(ids, ids + n);
  if (ids) cvxMemFree(reinterpret_cast<void**>(&ids));
  return {{"count", v.size()}, {"feature_ids", v}};
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
  m["create_hole"] = h_create_hole;
  m["pattern_shape"] = h_pattern_shape;
  m["pattern_feature"] = h_pattern_feature;
  m["mirror_shape"] = h_mirror_shape;
  m["mirror_feature"] = h_mirror_feature;
  m["draft_faces"] = h_draft_faces;
  m["sweep"] = h_sweep;
  m["loft"] = h_loft;
  m["list_features"] = h_list_features;
  m["sketch_extrude"] = h_sketch_extrude;
  m["sketch_revolve"] = h_sketch_revolve;
  m["extrude_profile"] = h_extrude_polygon;
  m["revolve_profile"] = h_revolve_polygon;
}
