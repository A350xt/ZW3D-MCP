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

  // cvxPartBool "succeeds" even when the tool does not touch the base: subtract then just consumes the tool and leaves the base
  // unchanged (a sphere beside a box vanished with no message). Refuse that up front when the bounding boxes are disjoint ...
  svxBndBox bb{};
  check_api(cvxPartInqShapeBox(base, nullptr, &bb), "cvxPartInqShapeBox");
  if (type != VX_BOOL_ADD) {
    for (int t : tools) {
      svxBndBox tb{};
      check_api(cvxPartInqShapeBox(t, nullptr, &tb), "cvxPartInqShapeBox");
      auto overlap = [](const svxLimit& a, const svxLimit& b) { return std::min(a.max, b.max) - std::max(a.min, b.min) > 1e-9; };
      if (!(overlap(bb.X, tb.X) && overlap(bb.Y, tb.Y) && overlap(bb.Z, tb.Z)))
        throw ApiError("BOOLEAN_NO_OVERLAP", "工具体 " + std::to_string(t) + " 与基体 " + std::to_string(base) + " 的包围盒不相交，" + op + " 不会有任何效果",
                       "先用 get_bounding_box 比较两者的位置和尺寸，把工具体移到与基体重叠的位置。两个实体都没有被修改。");
    }
  }
  double vol_before = 0;
  {
    svxMassProp mp{};
    if (cvxPartInqShapeMass(base, 0.0, &mp) == 0) vol_before = mp.Volume;
  }

  DispGuard guard;
  check_api(cvxPartBool(type, base, (int)tools.size(), tools.data(), p.value("keep_tools", false) ? 1 : 0), "cvxPartBool");
  // ... and when the boxes overlap but the real shapes do not (e.g. a sphere touching only the corner region of a box's bounding box).
  if (type == VX_BOOL_REMOVE && vol_before > 0) {
    svxMassProp mp{};
    std::vector<int> now_ids = inq_shape_ids();
    bool still = std::find(now_ids.begin(), now_ids.end(), base) != now_ids.end();
    if (still && cvxPartInqShapeMass(base, 0.0, &mp) == 0 && std::fabs(mp.Volume - vol_before) <= 1e-9 * std::max(1.0, vol_before))
      throw ApiError("BOOLEAN_NO_EFFECT", "subtract 没有改变基体 " + std::to_string(base) + " 的体积：工具体与基体的实际形状并不相交（只是包围盒重叠）",
                     "检查两个实体的真实位置和形状。已回滚，两个实体都没有被修改。");
  }
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
  // thread: {"major_diameter": mm, "pitch": mm, "length": mm?}  -> ZW3D thread hole with a custom thread; `diameter` is then the drilled (minor) diameter.
  bool threaded = p.contains("thread") && p["thread"].is_object();
  check_api(cvxPartHoleInit(threaded ? VX_THREAD_HOLE : VX_GENERAL_HOLE, (int)type, &h), "cvxPartHoleInit");
  if (threaded) {
    const json& t = p["thread"];
    double major = t.at("major_diameter").get<double>(), pitch = t.at("pitch").get<double>();
    if (!(major > 0) || !(pitch > 0)) invalid_input("thread.major_diameter 和 thread.pitch 必须为正");
    if (!(major > dia)) invalid_input("thread.major_diameter 必须大于底孔直径 diameter", "螺纹孔：diameter 是底孔（小径），major_diameter 是螺纹大径，例如 M8x1.25: diameter≈6.8, major_diameter=8。");
    h.ThreadType = VX_THREAD_CUSTOM;
    h.ThreadSize[0] = 0;
    h.ThreadDiameter = major;
    h.ThreadPitch = pitch;
    h.ThreadLength = t.contains("length") && !t["length"].is_null() ? t["length"].get<double>() : (through ? 0.0 : depth);
  }
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

// Helical (spiral) sweep: a closed profile is swept along a helix about `axis`, `turns` turns with `pitch` mm of advance per turn.
// With combine=remove/add on an existing solid this makes real thread geometry; with combine=none it makes a spring / coil.
//
// svxSpiralSweepData.axis is only a direction: the header gives no axis position. The sweep is therefore always built about the line through the
// world origin, and the requested axis_point is realised by shifting the profile there and moving the finished solid back. For whole turns the
// swept solid is symmetric about its axis, which is checked: if ZW3D used a different axis the call fails (HELIX_AXIS_MISMATCH) instead of
// returning a coil in the wrong place. A boolean (combine != none) is done afterwards against base_shape_id.
void shift_segments(json& segs, const svxPoint& d) {
  for (auto& s : segs)
    for (const char* k : {"start", "end", "through", "center"})
      if (s.contains(k) && s[k].is_array() && s[k].size() == 3) {
        s[k][0] = s[k][0].get<double>() - d.x;
        s[k][1] = s[k][1].get<double>() - d.y;
        s[k][2] = s[k][2].get<double>() - d.z;
      }
}

json h_helical_sweep(const json& p) {
  std::string comb = p.value("combine", std::string("none"));
  parse_combine(p);  // validates the value
  int base = p.contains("base_shape_id") && !p["base_shape_id"].is_null() ? p["base_shape_id"].get<int>() : 0;
  if (comb != "none") {
    if (base <= 0) invalid_input("combine=" + comb + " 需要 base_shape_id（被切/被加的实体）", "先 list_shapes 取得实体 id；或用 combine=none 得到独立的螺旋体。");
    require_shape(base);
  }
  svxPoint ap{0, 0, 0};
  if (p.contains("axis_point") && !p["axis_point"].is_null()) ap = to_point(p["axis_point"], "axis_point");
  svxVector axis = to_vector(p.at("axis_direction"), "axis_direction");
  double al = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
  if (al < 1e-12) invalid_input("axis_direction 不能为零向量");
  DispGuard guard;
  std::vector<int> before = inq_shape_ids();
  json prof = p.at("profile");
  shift_segments(prof, ap);
  int first = 0;
  int list = make_curvelist(prof, &first);
  svxSpiralSweepData d;
  check_api(cvxPartSpiralSweepInit(&d), "cvxPartSpiralSweepInit");
  d.pntOnPro.pntType = VX_START_POINT;
  d.pntOnPro.entPath.Count = 2;
  d.pntOnPro.entPath.Id[0] = -2;  // path starts at the active root object
  d.pntOnPro.entPath.Id[1] = list;
  d.axis = axis;
  d.turns = p.at("turns").get<double>();
  d.dst = p.at("pitch").get<double>();
  d.combine = VX_BOOL_NONE;
  d.leadType = VX_LEAD_NONE;
  d.capType = VX_END_CAP_BOTH;
  d.offsetType = VX_OFFSET_NONE;
  d.taperAng = p.value("taper_angle", 0.0);
  d.isClockwise = p.value("clockwise", false) ? 1 : 0;
  d.isFlipDir = p.value("flip", false) ? 1 : 0;
  int id = 0;
  check_api(cvxPartSpiralSweep(&d, &id), "cvxPartSpiralSweep");
  std::vector<int> now = inq_shape_ids();
  if (id <= 0 || std::find(now.begin(), now.end(), id) == now.end()) {
    id = 0;
    for (int s : now)
      if (std::find(before.begin(), before.end(), s) == before.end()) id = s;
  }
  if (id <= 0) throw ApiError("HELIX_NO_RESULT", "螺旋扫掠没有产生实体", "确认 profile 是封闭轮廓、位于轴的一侧且不与轴相交；turns 和 pitch 为正。");

  double turns = d.turns;
  if (std::fabs(turns - std::round(turns)) < 1e-9) {
    svxBndBox bb{};
    check_api(cvxPartInqShapeBox(id, nullptr, &bb), "cvxPartInqShapeBox");
    double c[3] = {(bb.X.min + bb.X.max) / 2, (bb.Y.min + bb.Y.max) / 2, (bb.Z.min + bb.Z.max) / 2};
    double u[3] = {axis.x / al, axis.y / al, axis.z / al};
    double along = c[0] * u[0] + c[1] * u[1] + c[2] * u[2];
    double off[3] = {c[0] - along * u[0], c[1] - along * u[1], c[2] - along * u[2]};
    double offl = std::sqrt(off[0] * off[0] + off[1] * off[1] + off[2] * off[2]);
    double diag = std::sqrt(std::pow(bb.X.max - bb.X.min, 2) + std::pow(bb.Y.max - bb.Y.min, 2) + std::pow(bb.Z.max - bb.Z.min, 2));
    if (offl > 0.05 * diag + 1e-6)
      throw ApiError("HELIX_AXIS_MISMATCH", "螺旋体没有围绕预期的轴生成（偏离 " + std::to_string(offl) + " mm）",
                     "中望3D 的螺旋扫掠接口只有轴方向、没有轴位置，实际使用的轴与“过原点”的假设不符。已回滚；把此信息反馈给开发者。");
  }

  if (ap.x != 0 || ap.y != 0 || ap.z != 0) {
    svxMatrix m{};
    m.identity = 0;
    m.xx = m.yy = m.zz = 1.0;
    m.xt = ap.x;
    m.yt = ap.y;
    m.zt = ap.z;
    check_api(cvxPartMoveTransformList(1, &id, &m), "cvxPartMoveTransformList");
  }
  std::vector<int> after = inq_shape_ids();
  int tool = 0;
  for (int s : after)
    if (std::find(before.begin(), before.end(), s) == before.end()) tool = s;
  if (tool <= 0) tool = id;

  if (comb == "none") return creation_result(VX_BOOL_NONE, tool);
  json b = {{"operation", comb == "add" ? "add" : comb == "remove" ? "subtract" : "intersect"}, {"base_id", base}, {"tool_ids", json::array({tool})}, {"keep_tools", false}};
  return h_boolean(b);
}

// Thread attribute on a cylindrical face (hole wall or shaft): what drawings and the UI show as a thread callout, without modeled geometry.
json h_add_thread(const json& p) {
  int face = p.at("face_id").get<int>();
  svxPoint at = to_point(p.at("point"), "point");
  svxThreadData t;
  check_api(cvxPartThreadAtInit(&t), "cvxPartThreadAtInit");
  t.Type = VX_THREAD_CUSTOM;
  t.Diameter = p.at("diameter").get<double>();
  t.Length = p.at("length").get<double>();
  t.Pitch = p.at("pitch").get<double>();
  t.PerUnit = 0;
  t.EndChamfer = 0;
  DispGuard guard;
  check_api(cvxPartThreadAt(face, &at, &t), "cvxPartThreadAt");
  return {{"face_id", face}, {"diameter", t.Diameter}, {"pitch", t.Pitch}, {"length", t.Length}};
}

// ---------------------------------------------------------------------------------------------------------------
// Transform and shape-editing features: move/rotate, scale, rib, thicken, trim, split, extend face
// ---------------------------------------------------------------------------------------------------------------
// Several of these report success without doing anything (like boolean/mirror), so compare the geometry fingerprint.
void require_geometry_change(const std::vector<std::string>& before, const char* code, const std::string& what, const std::string& hint) {
  if (before == shapes_signature()) throw ApiError(code, what, hint + " 已回滚。");
}

json h_move_shapes(const json& p) {
  std::vector<int> ids = ids_of(p.at("shape_ids"), "shape_ids");
  if (ids.empty()) invalid_input("shape_ids 不能为空");
  for (int id : ids) require_shape(id);
  svxVector t{0, 0, 0};
  bool has_t = p.contains("translate") && !p["translate"].is_null();
  if (has_t) t = to_vector(p["translate"], "translate");
  bool has_r = p.contains("rotate") && p["rotate"].is_object();
  // p' = R p + c, with R a rotation about the axis through `ap` (Rodrigues) followed by the translation.
  double R[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  svxPoint ap{0, 0, 0};
  if (has_r) {
    const json& r = p["rotate"];
    ap = to_point(r.at("axis_point"), "rotate.axis_point");
    svxVector d = to_vector(r.at("axis_direction"), "rotate.axis_direction");
    double dl = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    if (dl < 1e-12) invalid_input("rotate.axis_direction 不能为零向量");
    double kx = d.x / dl, ky = d.y / dl, kz = d.z / dl;
    double a = r.at("angle").get<double>() * 3.14159265358979323846 / 180.0, c = std::cos(a), s = std::sin(a), v = 1 - c;
    R[0][0] = c + kx * kx * v;      R[0][1] = kx * ky * v - kz * s; R[0][2] = kx * kz * v + ky * s;
    R[1][0] = ky * kx * v + kz * s; R[1][1] = c + ky * ky * v;      R[1][2] = ky * kz * v - kx * s;
    R[2][0] = kz * kx * v - ky * s; R[2][1] = kz * ky * v + kx * s; R[2][2] = c + kz * kz * v;
  }
  if (!has_t && !has_r) invalid_input("需要 translate 和/或 rotate");
  if (has_t && t.x == 0 && t.y == 0 && t.z == 0 && !has_r) invalid_input("translate 不能为零向量");
  double rc[3] = {R[0][0] * ap.x + R[0][1] * ap.y + R[0][2] * ap.z, R[1][0] * ap.x + R[1][1] * ap.y + R[1][2] * ap.z,
                  R[2][0] * ap.x + R[2][1] * ap.y + R[2][2] * ap.z};
  svxMatrix m{};
  m.identity = 0;
  // field "<axis><component>": the x-axis (image of the unit x vector) is (xx, xy, xz); xt/yt/zt is the offset.
  m.xx = R[0][0]; m.xy = R[1][0]; m.xz = R[2][0];
  m.yx = R[0][1]; m.yy = R[1][1]; m.yz = R[2][1];
  m.zx = R[0][2]; m.zy = R[1][2]; m.zz = R[2][2];
  m.xt = ap.x - rc[0] + t.x;
  m.yt = ap.y - rc[1] + t.y;
  m.zt = ap.z - rc[2] + t.z;
  std::vector<std::string> sig = shapes_signature();
  DispGuard guard;
  check_api(cvxPartMoveTransformList((int)ids.size(), ids.data(), &m), "cvxPartMoveTransformList");
  json out = shapes_summary();
  out["note"] = "移动/旋转后实体 id 可能改变；以返回的 shapes 为准。";
  // A pure rotation of a symmetric solid about its own centre can legitimately leave the fingerprint unchanged, so only translations are checked.
  if (!has_r) require_geometry_change(sig, "MOVE_NO_EFFECT", "移动没有改变任何实体的位置", "确认 shape_ids 是实体，translate 不为零。");
  return out;
}

json h_scale_shapes(const json& p) {
  std::vector<int> ids = ids_of(p.at("shape_ids"), "shape_ids");
  if (ids.empty()) invalid_input("shape_ids 不能为空");
  for (int id : ids) require_shape(id);
  double sx, sy, sz;
  const json& s = p.at("scale");
  if (s.is_number()) sx = sy = sz = s.get<double>();
  else {
    svxPoint v = to_point(s, "scale");
    sx = v.x; sy = v.y; sz = v.z;
  }
  if (!(sx > 0 && sy > 0 && sz > 0)) invalid_input("缩放系数必须为正");
  svxPoint c{0, 0, 0};
  if (p.contains("center") && !p["center"].is_null()) c = to_point(p["center"], "center");
  std::vector<std::string> sig = shapes_signature();
  DispGuard guard;
  check_api(cvxPartScale2((int)ids.size(), ids.data(), sx, sy, sz, 0, &c), "cvxPartScale2");
  json out = shapes_summary();
  out["note"] = "缩放后实体 id 可能改变；以返回的 shapes 为准。";
  if (sx != 1 || sy != 1 || sz != 1) require_geometry_change(sig, "SCALE_NO_EFFECT", "缩放没有改变任何实体", "确认 shape_ids 是实体且系数不全为 1。");
  return out;
}

json h_create_rib(const json& p) {
  int shape = p.contains("shape_id") && !p["shape_id"].is_null() ? p["shape_id"].get<int>() : -1;
  if (shape != -1) require_shape(shape);
  double w = p.at("thickness").get<double>();
  if (!(w > 0)) invalid_input("thickness 必须为正");
  DispGuard guard;
  int first = 0;
  int list = make_curvelist(p.at("profile"), &first);
  svxRibData r;
  check_api(cvxPartRibInit(&r), "cvxPartRibInit");
  r.idShape = shape;
  r.idParent = list;
  r.idProfile = first;
  r.direction = p.value("perpendicular", false) ? 1 : 0;
  std::string side = p.value("side", std::string("both"));
  r.widthType = side == "first" ? 0 : side == "second" ? 2 : 1;
  r.width = w;
  r.angle = 0;
  r.flipDir = p.value("flip", false) ? 1 : 0;
  std::vector<std::string> sig = shapes_signature();
  int id = 0;
  check_api(cvxPartRib(&r, &id), "cvxPartRib");
  require_geometry_change(sig, "RIB_NO_EFFECT", "加强筋没有改变任何实体",
                          "轮廓线必须是开放的，并且两端要接触到实体表面；用 flip 反转材料方向，或检查 shape_id。");
  json out = shapes_summary();
  out["feature_id"] = id;
  return out;
}

json h_thicken_faces(const json& p) {
  std::vector<int> faces = ids_of(p.at("face_ids"), "face_ids");
  if (faces.empty()) invalid_input("face_ids 不能为空");
  double t = p.at("thickness").get<double>();
  if (t == 0) invalid_input("thickness 不能为 0（正=向外，负=向内）");
  svxThickenData d;
  check_api(cvxPartThickenInit(&d), "cvxPartThickenInit");
  d.type = 1;
  d.thick = t;
  d.numFaces = (int)faces.size();
  d.idFaces = faces.data();
  std::vector<std::string> sig = shapes_signature();
  DispGuard guard;
  int id = 0;
  check_api(cvxPartThicken(&d, &id), "cvxPartThicken");
  json out = shapes_summary();
  out["new_shape_id"] = id;
  return out;
}

// Cutters: either an explicit list of face/shape ids or a plane (given as origin + normal).
std::vector<int> cutter_ids(const json& p, bool* made_plane) {
  *made_plane = false;
  std::vector<int> c;
  if (p.contains("cutter_ids") && !p["cutter_ids"].is_null()) c = ids_of(p["cutter_ids"], "cutter_ids");
  if (p.contains("plane") && !p["plane"].is_null()) {
    c.push_back(make_datum_plane(p["plane"]));
    *made_plane = true;
  }
  if (c.empty()) invalid_input("需要 plane 或 cutter_ids");
  return c;
}

json h_trim_shape(const json& p) {
  int shape = p.at("shape_id").get<int>();
  require_shape(shape);
  DispGuard guard;
  bool mp;
  std::vector<int> cutters = cutter_ids(p, &mp);
  svxTrimData t;
  check_api(cvxPartTrimInit(&t), "cvxPartTrimInit");
  t.numBase = 1;
  t.idBase = &shape;
  t.numTrimm = (int)cutters.size();
  t.idTrimmer = cutters.data();
  t.flip = p.value("flip", false) ? 1 : 0;
  t.trimCap = 1;
  t.keepTrimm = mp ? 1 : (p.value("keep_cutters", false) ? 1 : 0);
  t.extend = 1;
  std::vector<std::string> sig = shapes_signature();
  int n = 0;
  int* ids = nullptr;
  check_api(cvxPartTrim(&t, &n, &ids), "cvxPartTrim");
  if (ids) cvxMemFree(reinterpret_cast<void**>(&ids));
  require_geometry_change(sig, "TRIM_NO_EFFECT", "修剪没有改变实体", "平面必须穿过实体；用 flip 选择保留哪一侧。");
  return shapes_summary();
}

json h_split_shape(const json& p) {
  int shape = p.at("shape_id").get<int>();
  require_shape(shape);
  DispGuard guard;
  bool mp;
  std::vector<int> cutters = cutter_ids(p, &mp);
  svxDivideData d;
  check_api(cvxPartDivideInit(&d), "cvxPartDivideInit");
  d.numBase = 1;
  d.idBase = &shape;
  d.numCutter = (int)cutters.size();
  d.idCutters = cutters.data();
  d.keepCutter = mp ? 1 : (p.value("keep_cutters", false) ? 1 : 0);
  d.trimCap = 1;
  d.extend = 1;
  std::vector<int> before = inq_shape_ids();
  int n = 0;
  int* ids = nullptr;
  check_api(cvxPartDivide(&d, &n, &ids), "cvxPartDivide");
  if (ids) cvxMemFree(reinterpret_cast<void**>(&ids));
  json out = shapes_summary();
  if (inq_shape_ids().size() <= before.size())
    throw ApiError("SPLIT_NO_RESULT", "分割没有产生新的实体", "平面必须穿过实体内部。已回滚。");
  return out;
}

json h_extend_face(const json& p) {
  int face = p.at("face_id").get<int>();
  double dist = p.at("distance").get<double>();
  if (!(dist > 0)) invalid_input("distance 必须为正");
  svxExtendData e;
  check_api(cvxPartExtendFaceInit(&e), "cvxPartExtendFaceInit");
  e.idFace = face;
  e.cntEdge = 0;  // all edges of the face
  e.fDist = dist;
  std::vector<std::string> sig = shapes_signature();
  DispGuard guard;
  int id = 0;
  check_api(cvxPartExtendFace(&e, &id), "cvxPartExtendFace");
  json out = shapes_summary();
  out["feature_id"] = id;
  return out;
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
  m["move_shapes"] = h_move_shapes;
  m["scale_shapes"] = h_scale_shapes;
  m["create_rib"] = h_create_rib;
  m["thicken_faces"] = h_thicken_faces;
  m["trim_shape"] = h_trim_shape;
  m["split_shape"] = h_split_shape;
  m["extend_face"] = h_extend_face;
  m["helical_sweep"] = h_helical_sweep;
  m["add_thread"] = h_add_thread;
  m["list_features"] = h_list_features;
  m["sketch_extrude"] = h_sketch_extrude;
  m["sketch_revolve"] = h_sketch_revolve;
  m["extrude_profile"] = h_extrude_polygon;
  m["revolve_profile"] = h_revolve_polygon;
}
