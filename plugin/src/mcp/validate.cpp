#include "validate.h"
#include <algorithm>
#include <cmath>
#include <sstream>

namespace val {

[[noreturn]] void bad(const std::string& message, const std::string& hint) { throw ApiError("INVALID_INPUT", message, hint); }

namespace {
V3 sub(const V3& a, const V3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 add(const V3& a, const V3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 cross(const V3& a, const V3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
double norm(const V3& a) { return std::sqrt(dot(a, a)); }

bool is_num(const json& v) { return v.is_number() && !v.is_boolean(); }

std::string show(const json& v) {
  std::string s = v.dump(-1, ' ', false, json::error_handler_t::replace);
  return s.size() > 60 ? s.substr(0, 57) + "..." : s;
}

std::string fmt(double d) {
  std::ostringstream o;
  o << d;
  return o.str();
}
}  // namespace

json to_json(const V3& v) { return json::array({v.x, v.y, v.z}); }

V3 point(const json& v, const std::string& name) {
  if (!v.is_array() || v.size() != 3) bad(name + " must be [x, y, z]", "Use three numbers in millimetres.");
  double c[3];
  for (int i = 0; i < 3; ++i) {
    if (!is_num(v[i]) || !std::isfinite(v[i].get<double>()))
      bad(name + " contains a non-finite or non-numeric value: " + show(v[i]));
    c[i] = v[i].get<double>();
  }
  return {c[0], c[1], c[2]};
}

V3 vector(const json& v, const std::string& name) {
  V3 p = point(v, name);
  if (norm(p) < 1e-9) bad(name + " must not be the zero vector");
  return p;
}

double number(const json& v, const std::string& name) {
  if (!is_num(v) || !std::isfinite(v.get<double>())) bad(name + " must be a finite number, got " + show(v));
  return v.get<double>();
}

double positive(const json& v, const std::string& name) {
  if (!is_num(v) || !std::isfinite(v.get<double>()) || v.get<double>() <= 0)
    bad(name + " must be a positive number, got " + show(v), "Dimensions are in millimetres and must be > 0.");
  return v.get<double>();
}

std::vector<int> id_list(const json& ids, const std::string& name, int minimum) {
  if (!ids.is_array() || (int)ids.size() < minimum) bad(name + " must be a list of at least " + std::to_string(minimum) + " integer id(s)");
  std::vector<int> out;
  for (const auto& v : ids) {
    if (!v.is_number_integer() || v.get<long long>() < 1)
      bad(name + " contains an invalid id: " + show(v), "Ids are positive integers returned by earlier calls.");
    out.push_back(v.get<int>());
  }
  return out;
}

int one_id(const json& id, const std::string& name) { return id_list(json::array({id}), name)[0]; }

V3 polygon_normal(const std::vector<V3>& pts) {
  V3 n{};
  for (size_t i = 0; i < pts.size(); ++i) n = add(n, cross(pts[i], pts[(i + 1) % pts.size()]));
  double l = norm(n);
  return {n.x / l, n.y / l, n.z / l};
}

std::vector<V3> polygon(const json& points, const std::string& name) {
  if (!points.is_array() || points.size() < 3)
    bad(name + " needs at least 3 vertices", "Give the closed outline once; do not repeat the first point at the end.");
  std::vector<V3> pts;
  for (size_t i = 0; i < points.size(); ++i) pts.push_back(point(points[i], name + "[" + std::to_string(i) + "]"));
  if (norm(sub(pts.front(), pts.back())) < 1e-6) {
    pts.pop_back();  // tolerate an explicitly closed outline
    if (pts.size() < 3) bad(name + " needs at least 3 distinct vertices");
  }
  for (size_t i = 0; i < pts.size(); ++i) {
    size_t j = (i + 1) % pts.size();
    if (norm(sub(pts[i], pts[j])) < 1e-6)
      bad(name + ": vertices " + std::to_string(i) + " and " + std::to_string(j) + " coincide", "Remove duplicate points.");
  }
  V3 n{};
  for (size_t i = 0; i < pts.size(); ++i) n = add(n, cross(pts[i], pts[(i + 1) % pts.size()]));
  double area2 = norm(n);
  if (area2 < 1e-6) bad(name + " is degenerate (zero area, vertices are collinear)");
  V3 unit{n.x / area2, n.y / area2, n.z / area2};
  double scale = 0;
  for (const auto& p : pts) scale = std::max(scale, norm(sub(p, pts[0])));
  for (size_t i = 0; i < pts.size(); ++i)
    if (std::fabs(dot(unit, sub(pts[i], pts[0]))) > 1e-6 * std::max(scale, 1.0) + 1e-6)
      bad(name + " is not planar (vertex " + std::to_string(i) + " is off the plane)", "All vertices must lie in one plane.");
  return pts;
}

// ------------------------------------------------------------------ segments
json segments(const json& segs, const std::string& name) {
  if (!segs.is_array() || segs.empty())
    bad(name + " must be a non-empty list of segments",
        "Each segment is {\"type\":\"line\",\"start\":p,\"end\":p}, {\"type\":\"arc\",\"start\":p,\"through\":p,\"end\":p} "
        "or {\"type\":\"circle\",\"center\":p,\"radius\":r,\"normal\":[0,0,1]}.");
  json out = json::array();
  bool has_circle = false;
  for (size_t i = 0; i < segs.size(); ++i) {
    const json& sg = segs[i];
    std::string nm = name + "[" + std::to_string(i) + "]";
    std::string t = sg.is_object() && sg.contains("type") && sg["type"].is_string() ? sg["type"].get<std::string>() : "";
    if (t != "line" && t != "arc" && t != "circle") bad(nm + ".type must be 'line', 'arc' or 'circle'");
    json item = {{"type", t}};
    auto get = [&](const char* k) -> const json& {
      static const json null_json;
      auto it = sg.find(k);
      return it == sg.end() ? null_json : *it;
    };
    if (t == "circle") {
      has_circle = true;
      item["center"] = to_json(point(get("center"), nm + ".center"));
      item["radius"] = positive(get("radius"), nm + ".radius");
      item["normal"] = get("normal").is_null() ? json::array({0.0, 0.0, 1.0}) : to_json(vector(get("normal"), nm + ".normal"));
    } else {
      V3 s = point(get("start"), nm + ".start"), e = point(get("end"), nm + ".end");
      item["start"] = to_json(s);
      item["end"] = to_json(e);
      if (t == "arc") {
        V3 m = point(get("through"), nm + ".through");
        item["through"] = to_json(m);
        if (norm(cross(sub(m, s), sub(e, s))) < 1e-9) bad(nm + ": start, through and end are collinear", "Pick a through point off the chord.");
      } else if (norm(sub(s, e)) < 1e-9) {
        bad(nm + ": zero-length line");
      }
    }
    out.push_back(item);
  }
  if (has_circle) {
    if (out.size() != 1)
      bad("A circle must be the only segment of a profile", "Make one profile call per circle, or combine with combine='remove' for holes.");
    return out;
  }
  for (size_t i = 0; i < out.size(); ++i) {
    size_t j = (i + 1) % out.size();
    if (norm(sub(point(out[i]["end"], "end"), point(out[j]["start"], "start"))) > 1e-6)
      bad(name + ": segment " + std::to_string(i) + " does not end where segment " + std::to_string(j) + " starts",
          "List segments in order around the outline; the last end must equal the first start.");
  }
  return out;
}

json segments2d(const json& segs, const std::string& name) {
  auto up = [&](const json& p) -> json {
    if (!p.is_array() || p.size() != 2) bad(name + ": sketch points must be [x, y]");
    return json::array({p[0], p[1], 0.0});
  };
  json conv = json::array();
  if (segs.is_array()) {
    for (const auto& sg : segs) {
      if (!sg.is_object()) {
        conv.push_back(sg);
        continue;
      }
      json item = sg;
      for (const char* k : {"start", "end", "through", "center"})
        if (item.contains(k) && !item[k].is_null()) item[k] = up(item[k]);
      item.erase("normal");
      conv.push_back(item);
    }
  }
  json checked = segments(conv, name);
  json out = json::array();
  for (const auto& sg : checked) {
    json o = {{"type", sg["type"]}};
    for (const char* k : {"start", "end", "through", "center"})
      if (sg.contains(k)) o[k] = json::array({sg[k][0], sg[k][1]});
    if (sg.contains("radius")) o["radius"] = sg["radius"];
    out.push_back(o);
  }
  return out;
}

json inner_loops2d(const json& loops, const std::string& name) {
  json out = json::array();
  if (loops.is_null()) return out;
  if (!loops.is_array()) bad(name + " must be a list of loops (each loop is a segment list)");
  for (size_t i = 0; i < loops.size(); ++i) out.push_back(segments2d(loops[i], name + "[" + std::to_string(i) + "]"));
  return out;
}

json path_segments(const json& segs, const std::string& name) {
  if (!segs.is_array() || segs.empty()) bad(name + " must be a non-empty list of line/arc segments");
  json out = json::array();
  for (size_t i = 0; i < segs.size(); ++i) {
    const json& sg = segs[i];
    std::string nm = name + "[" + std::to_string(i) + "]";
    std::string t = sg.is_object() && sg.contains("type") && sg["type"].is_string() ? sg["type"].get<std::string>() : "";
    if (t != "line" && t != "arc") bad(nm + ".type must be 'line' or 'arc'");
    auto get = [&](const char* k) -> const json& {
      static const json null_json;
      auto it = sg.find(k);
      return it == sg.end() ? null_json : *it;
    };
    V3 s = point(get("start"), nm + ".start"), e = point(get("end"), nm + ".end");
    json item = {{"type", t}, {"start", to_json(s)}, {"end", to_json(e)}};
    if (t == "arc") {
      V3 m = point(get("through"), nm + ".through");
      item["through"] = to_json(m);
      if (norm(cross(sub(m, s), sub(e, s))) < 1e-9) bad(nm + ": start, through and end are collinear");
    } else if (norm(sub(s, e)) < 1e-9) {
      bad(nm + ": zero-length line");
    }
    out.push_back(item);
  }
  for (size_t i = 0; i + 1 < out.size(); ++i)
    if (norm(sub(point(out[i]["end"], "end"), point(out[i + 1]["start"], "start"))) > 1e-6)
      bad(name + ": segment " + std::to_string(i) + " does not end where segment " + std::to_string(i + 1) + " starts",
          "List path segments in order; each start must equal the previous end.");
  return out;
}

// ------------------------------------------------------------------ planes
V3 plane_normal(const Frame& f) {
  V3 n = cross(f.x_axis, f.y_axis);
  double l = norm(n);
  return {n.x / l, n.y / l, n.z / l};
}

json frame_json(const Frame& f) {
  return {{"origin", to_json(f.origin)}, {"x_axis", to_json(f.x_axis)}, {"y_axis", to_json(f.y_axis)}};
}

Frame sketch_plane(const json& plane) {
  V3 origin{0, 0, 0};
  std::string name;
  if (plane.is_string()) {
    name = plane.get<std::string>();
  } else if (plane.is_object()) {
    bool has_name = plane.contains("name") && !plane["name"].is_null();
    if (plane.contains("origin") && !plane["origin"].is_null()) origin = point(plane["origin"], "plane.origin");
    if (!has_name) {
      V3 x = vector(plane.contains("x_axis") ? plane["x_axis"] : json(), "plane.x_axis");
      V3 y = vector(plane.contains("y_axis") ? plane["y_axis"] : json(), "plane.y_axis");
      if (norm(cross(x, y)) < 1e-9 * norm(x) * norm(y)) bad("plane.x_axis and plane.y_axis must not be parallel");
      return {origin, x, y};
    }
    name = plane["name"].is_string() ? plane["name"].get<std::string>() : plane["name"].dump();
  } else {
    bad("plane must be 'XY', 'YZ', 'ZX' or a frame dict");
  }
  std::string key = name;
  std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return (char)std::toupper(c); });
  // right-handed frames: normal = x_axis x y_axis
  if (key == "XY") return {origin, {1, 0, 0}, {0, 1, 0}};  // normal +Z
  if (key == "YZ") return {origin, {0, 1, 0}, {0, 0, 1}};  // normal +X
  if (key == "ZX") return {origin, {0, 0, 1}, {1, 0, 0}};  // normal +Y
  bad("unknown plane '" + name + "'", "Use 'XY', 'YZ', 'ZX', or give origin + x_axis + y_axis.");
}

json mirror_plane(const json& plane) {
  if (plane.is_object() && plane.contains("normal") && !plane["normal"].is_null()) {
    V3 o = plane.contains("origin") && !plane["origin"].is_null() ? point(plane["origin"], "plane.origin") : V3{0, 0, 0};
    return {{"origin", to_json(o)}, {"normal", to_json(vector(plane["normal"], "plane.normal"))}};
  }
  Frame f = sketch_plane(plane);
  return {{"origin", to_json(f.origin)}, {"normal", to_json(plane_normal(f))}};
}

// ------------------------------------------------------------------ revolve axis check
std::vector<V3> profile_points(const json& segs) {
  std::vector<V3> pts;
  for (const auto& sg : segs) {
    if (sg["type"] == "circle") continue;
    pts.push_back(point(sg["start"], "start"));
    pts.push_back(point(sg["end"], "end"));
    if (sg["type"] == "arc") pts.push_back(point(sg["through"], "through"));
  }
  return pts;
}

std::vector<V3> profile_points2d(const json& segs) {
  std::vector<V3> pts;
  auto p2 = [](const json& p) { return V3{p[0].get<double>(), p[1].get<double>(), 0}; };
  for (const auto& sg : segs) {
    if (sg["type"] == "circle") continue;
    pts.push_back(p2(sg["start"]));
    pts.push_back(p2(sg["end"]));
    if (sg["type"] == "arc") pts.push_back(p2(sg["through"]));
  }
  return pts;
}

static void reject_if_crossed(const std::vector<double>& side) {
  if (side.empty()) return;
  const double tol = 1e-7;
  double scale = 1.0, lo = side[0], hi = side[0];
  for (double s : side) {
    scale = std::max(scale, std::fabs(s));
    lo = std::min(lo, s);
    hi = std::max(hi, s);
  }
  if (lo < -tol * scale && hi > tol * scale)
    bad("The profile crosses the rotation axis (it has points on both sides of it)",
        "Keep the whole profile on one side of the axis, or revolve each half separately.");
}

void check_axis_not_crossed_2d(const std::vector<V3>& pts, double apx, double apy, double adx, double ady) {
  std::vector<double> side;
  for (const auto& p : pts) side.push_back(adx * (p.y - apy) - ady * (p.x - apx));
  reject_if_crossed(side);
}

void check_axis_not_crossed_3d(const std::vector<V3>& pts, const V3& axis_point, const V3& axis_dir) {
  if (pts.size() < 3) return;
  V3 n = polygon_normal(pts);
  if (!std::isfinite(n.x)) return;
  double ad = norm(axis_dir);
  if (std::fabs(dot(axis_dir, n)) > 1e-6 * ad || std::fabs(dot(sub(axis_point, pts[0]), n)) > 1e-6 * std::max(1.0, ad))
    return;  // axis not in the profile plane: crossing is not meaningful here
  V3 w = cross(n, axis_dir);
  std::vector<double> side;
  for (const auto& p : pts) side.push_back(dot(sub(p, axis_point), w));
  reject_if_crossed(side);
}

std::string check_expression(const std::string& name, const json& expression) {
  if (!expression.is_string() || expression.get<std::string>().find_first_not_of(" \t\r\n") == std::string::npos)
    bad("expression must be a non-empty string");
  const std::string e = expression.get<std::string>();
  auto word = [](unsigned char c) { return std::isalnum(c) || c == '_'; };
  for (size_t pos = e.find(name); !name.empty() && pos != std::string::npos; pos = e.find(name, pos + 1)) {
    bool left = pos == 0 || !word((unsigned char)e[pos - 1]);
    bool right = pos + name.size() >= e.size() || !word((unsigned char)e[pos + name.size()]);
    if (left && right)
      bad("expression '" + e + "' refers to the variable '" + name + "' itself (circular reference)",
          "Use a literal value, or reference a different variable.");
  }
  return e;
}

}  // namespace val
