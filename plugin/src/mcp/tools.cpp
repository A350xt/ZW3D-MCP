#include "tools.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include "../encoding.h"
#include "../errors.h"
#include "validate.h"

namespace mcp {
using val::bad;

// Built-in usage guide: sent to every client as the `instructions` of initialize and offered as the prompt "zw3d_guide".
const char* const kInstructions = R"GUIDE(Controls a running ZW3D 2025+ (not ZWCAD) through its API. Units: millimetres and degrees. Points are [x, y, z].

# Workflow
1. ping to check the connection. new_part creates a file (.Z3PRT) and makes it active; nothing can be modeled before that.
2. Model with ONE tool call per design step. Every modeling tool returns the affected shape_id(s) with bounding box and volume: check them against your intent before the next step instead of calling extra queries.
3. After a few steps verify with get_bounding_box / get_mass_props and look with screenshot. When something is wrong, undo (whole tool calls) or fix it with the editing tools, do not pile new geometry on top.
4. save_part when done (give a path for Save As); export_step for exchange files.

# Choosing a tool
- Simple blocks: create_box, create_cylinder (axis +Z from the bottom centre), create_sphere.
- Profile-based parts (prismatic, turned, with holes): ALWAYS DRAW A SKETCH FIRST. Use sketch_extrude / sketch_revolve whenever the outline lies in a plane (lines, arcs, circles, inner loops = holes); the sketch stays in the feature tree and the part remains editable. Do NOT build loose curves and extrude them: extrude_polygon / revolve_polygon / extrude_profile / revolve_profile exist only as a fallback for outlines a sketch cannot express (e.g. a profile on an arbitrary 3D plane that sketch_extrude rejects), and they leave stray wireframe with no editable sketch. Plain blocks, cylinders and spheres: create_box / create_cylinder / create_sphere.
- Combine solids with boolean (add / subtract / intersect) or the `combine` argument of the profile tools (none = new body, add, remove, intersect). Subtracting a tool that does not overlap the base is refused.
- Finishing: fillet_edges, chamfer_edges, shell_shape, draft_faces, create_hole (needs a planar face_id from list_faces), pattern_linear, pattern_circular, mirror, sweep, loft.
- Reshaping: move_shapes (translate / rotate), scale_shapes, create_rib, thicken_faces, trim_shape and split_shape (plane or cutter faces), extend_face. Cleanup: delete_shapes, delete_features, suppress_features, rename_feature. Look: set_color (face ids), screenshot with view / display_mode, set_view. Measure: measure_distance (0 = touching / interfering).
- Existing files: open_file for .Z3PRT/.Z3/.Z3ASM, import_file for STEP / Parasolid / SAT / IGES / STL, then list_shapes and modify.
- Assemblies: new_assembly, insert_component, list_components, add_constraint (lock needs only component ids; face constraints use pick paths from list_component_faces, experimental). Drawings: new_drawing, list_drawing_views, auto_dimension, add_bom (experimental).
- Springs and threads: create_hole with a `thread` object for tapped holes; helical_sweep (tooth/wire profile + axis + turns + pitch; combine=remove/add on a body for real threads); add_thread for a cosmetic thread on a hole/shaft face.
- Change an existing model without rebuilding: list_features -> get_feature_data -> set_feature_value (field ids come from get_feature_data); part variables via list_variables / add_variable / set_variable; sketch variables via list_sketch_variables / set_sketch_variable.
- Inspect: list_shapes, list_faces, list_edges (face_id / edge_id for holes, fillets, shells), get_bounding_box, get_mass_props.
- Assemblies and drawings (basic): new_assembly, insert_component (saved parts only), list_components, new_drawing (of a saved part).

# Rules that avoid wasted calls
- Use only ids you got from the most recent response. Regenerating the part (set_variable, set_feature_value, ...) RENUMBERS shape, face and edge ids; call list_shapes / list_faces / list_edges again afterwards.
- Profiles must be closed and planar; give the plane/normal explicitly. Distances, radii and sizes must be positive (a negative distance would silently reverse a feature, so it is refused).
- Fillet/chamfer radii must be smaller than the adjacent faces allow; try a smaller value if the call fails.
- Failed calls are rolled back completely and return `[CODE] message  Hint: ...`. Read the code and hint and correct the arguments; do not retry the identical call.
- Do not guess ids or field names: list first, then act. Prefer the typed tools; run_command is a disabled-by-default escape hatch.
- The ZW3D UI is shared with a human: avoid unnecessary new_part calls (they open new files) and tell the user what you changed.
)GUIDE";

namespace {

// ---------------------------------------------------------------- schema builders
json t_num(const char* d = nullptr) { json j = {{"type", "number"}}; if (d) j["description"] = d; return j; }
json t_int(const char* d = nullptr) { json j = {{"type", "integer"}}; if (d) j["description"] = d; return j; }
json t_str(const char* d = nullptr) { json j = {{"type", "string"}}; if (d) j["description"] = d; return j; }
json t_bool(const char* d = nullptr) { json j = {{"type", "boolean"}}; if (d) j["description"] = d; return j; }
json t_obj(const char* d = nullptr) { json j = {{"type", "object"}}; if (d) j["description"] = d; return j; }
json t_arr(json items, const char* d = nullptr, int mn = -1, int mx = -1) {
  json j = {{"type", "array"}, {"items", std::move(items)}};
  if (mn >= 0) j["minItems"] = mn;
  if (mx >= 0) j["maxItems"] = mx;
  if (d) j["description"] = d;
  return j;
}
json t_pt(const char* d = nullptr) { return t_arr(t_num(), d, 3, 3); }
json t_pt2(const char* d = nullptr) { return t_arr(t_num(), d, 2, 2); }
json t_ids(const char* d = nullptr) { return t_arr(t_int(), d); }
json t_enum(std::initializer_list<const char*> vals, const char* def = nullptr, const char* d = nullptr) {
  json e = json::array();
  for (const char* v : vals) e.push_back(v);
  json j = {{"type", "string"}, {"enum", e}};
  if (def) j["default"] = def;
  if (d) j["description"] = d;
  return j;
}
json t_combine() {
  return t_enum({"none", "add", "remove", "intersect"}, "none",
                "'none' (default) creates a separate new solid and returns its shape_id; 'add'/'remove'/'intersect' merge into / cut from / "
                "intersect with existing solids (shape_id is then null, see 'shapes' / 'shape_details').");
}
json t_plane() {
  return {{"anyOf", json::array({t_enum({"XY", "YZ", "ZX"}), t_obj()})},
          {"description", "'XY' (normal +Z), 'YZ' (normal +X), 'ZX' (normal +Y); or {\"name\":\"XY\",\"origin\":[0,0,20]} for an offset plane; "
                          "or a full frame {\"origin\":[..],\"x_axis\":[..],\"y_axis\":[..]} (normal = x_axis x y_axis)."}};
}
json t_segments(const char* d) { return t_arr(t_obj(), d); }

using Props = std::vector<std::pair<std::string, json>>;
json schema(const Props& props, std::vector<std::string> required = {}) {
  json p = json::object();
  for (const auto& kv : props) p[kv.first] = kv.second;
  json s = {{"type", "object"}, {"properties", p}};
  if (!required.empty()) s["required"] = required;
  return s;
}

// ---------------------------------------------------------------- argument access
const json& arg(const json& a, const char* k) {
  static const json null_json;
  if (!a.is_object()) return null_json;
  auto it = a.find(k);
  return it == a.end() ? null_json : *it;
}
bool has(const json& a, const char* k) { return !arg(a, k).is_null(); }
const json& req(const json& a, const char* k) {
  if (!has(a, k)) bad(std::string("missing required parameter '") + k + "'");
  return arg(a, k);
}
double num_or(const json& a, const char* k, double def) { return has(a, k) ? val::number(arg(a, k), k) : def; }
bool bool_or(const json& a, const char* k, bool def) {
  if (!has(a, k)) return def;
  if (!arg(a, k).is_boolean()) bad(std::string(k) + " must be true or false");
  return arg(a, k).get<bool>();
}
int int_or(const json& a, const char* k, int def) {
  if (!has(a, k)) return def;
  const json& v = arg(a, k);
  if (!v.is_number_integer() && !(v.is_number_float() && v.get<double>() == std::floor(v.get<double>())))
    bad(std::string(k) + " must be an integer");
  return v.get<int>();
}
std::string str_req(const json& a, const char* k) {
  const json& v = req(a, k);
  if (!v.is_string()) bad(std::string(k) + " must be a string");
  return v.get<std::string>();
}
std::string trim(const std::string& s) {
  size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
  return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}
std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  return s;
}
bool ends_with(const std::string& s, const std::string& suf) { return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0; }

std::string combine_of(const json& a) {
  if (!has(a, "combine")) return "none";
  const json& c = arg(a, "combine");
  std::string s = c.is_string() ? c.get<std::string>() : "";
  if (s != "none" && s != "add" && s != "remove" && s != "intersect") bad("combine must be one of none, add, remove, intersect");
  return s;
}

// A string argument restricted to the given choices.
std::string enum_of(const json& a, const char* k, std::initializer_list<const char*> choices) {
  const json& v = req(a, k);
  std::string s = v.is_string() ? v.get<std::string>() : "";
  std::string list;
  for (const char* c : choices) {
    if (s == c) return s;
    list += (list.empty() ? "" : ", ") + std::string(c);
  }
  bad(std::string(k) + " must be one of " + list);
  return s;
}

json pts_json(const std::vector<val::V3>& pts) {
  json out = json::array();
  for (const auto& p : pts) out.push_back(val::to_json(p));
  return out;
}

ToolCall call(const char* method, json params = json::object(), int timeout_ms = 60000) {
  ToolCall c;
  c.method = method;
  c.params = std::move(params);
  c.timeout_ms = timeout_ms;
  return c;
}

json opt_vector(const json& a, const char* k) { return has(a, k) ? val::to_json(val::vector(arg(a, k), k)) : json(nullptr); }

void check_angles(double angle, double start_angle) {
  if (!(0 < angle - start_angle && angle - start_angle <= 360)) bad("angle - start_angle must be in (0, 360]");
}

// shape_id XOR feature_ids (pattern / mirror targets)
std::pair<bool, json> target_of(const json& a) {
  bool s = has(a, "shape_id"), f = has(a, "feature_ids");
  if (s == f)
    bad("Give exactly one of shape_id (pattern/mirror a solid) or feature_ids (pattern/mirror features, e.g. a hole's feature_id)");
  if (s) return {true, {{"shape_id", val::one_id(arg(a, "shape_id"), "shape_id")}}};
  return {false, {{"feature_ids", val::id_list(arg(a, "feature_ids"), "feature_ids")}}};
}

// ---------------------------------------------------------------- registry
std::vector<Tool> g_tools;

const char* kCombineHelp = " combine: 'none' (default) creates a separate new solid and returns its shape_id; 'add'/'remove'/'intersect' merge into / cut from / "
                           "intersect with existing solids (shape_id is then null, see 'shapes' / 'shape_details').";

void add(const char* name, std::string description, json input_schema, std::function<ToolCall(const json&)> prepare, bool combine_help = false) {
  if (combine_help) description += kCombineHelp;
  g_tools.push_back({name, std::move(description), std::move(input_schema), std::move(prepare)});
}

void build() {
  // ============================================================ session / files
  add("ping", "Check the plugin connection. Returns plugin version, ZW3D active file and active root (part) name.", schema({}),
      [](const json&) { return call("ping", json::object(), 10000); });

  add("new_part",
      "Create a new ZW3D file containing an empty part and make it active.\n\nname: file name (without folder, e.g. \"bracket\") or a full path. "
      "part_name: accepted for compatibility; single-root part files (.Z3PRT) have a fixed root name, so it is not applied. "
      "Call this first when ping shows no active part.",
      schema({{"name", t_str()}, {"part_name", t_str()}}, {"name"}), [](const json& a) {
        std::string n = trim(str_req(a, "name"));
        if (n.empty()) bad("name must not be empty");
        return call("new_part", {{"name", n}, {"part_name", has(a, "part_name") ? arg(a, "part_name") : json(nullptr)}});
      });

  add("save_part",
      "Save the active file. Give `path` (full path, e.g. C:\\work\\a.Z3PRT) to Save As, required for a file that was never saved.",
      schema({{"path", t_str()}}), [](const json& a) {
        json p = json::object();
        if (has(a, "path") && arg(a, "path").is_string() && !arg(a, "path").get<std::string>().empty()) p["path"] = arg(a, "path");
        return call("save_part", p);
      });

  add("export_step", "Export the active file to a STEP file at `path` (full path ending in .step or .stp).", schema({{"path", t_str()}}, {"path"}),
      [](const json& a) {
        std::string p = str_req(a, "path"), l = lower(p);
        if (!ends_with(l, ".step") && !ends_with(l, ".stp")) bad("path must end with .step or .stp");
        return call("export_step", {{"path", p}});
      });

  // ============================================================ primitives
  add("create_box",
      "Create an axis-aligned box. Give EITHER center + size [dx, dy, dz] OR two opposite corners (corner1, corner2). Units mm.",
      schema({{"center", t_pt()}, {"size", t_pt()}, {"corner1", t_pt()}, {"corner2", t_pt()}, {"combine", t_combine()}}),
      [](const json& a) {
        val::V3 c, s;
        if (has(a, "corner1") || has(a, "corner2")) {
          if (!has(a, "corner1") || !has(a, "corner2") || has(a, "center") || has(a, "size"))
            bad("Use either (corner1, corner2) or (center, size), not a mix");
          val::V3 p = val::point(arg(a, "corner1"), "corner1"), q = val::point(arg(a, "corner2"), "corner2");
          s = {std::fabs(p.x - q.x), std::fabs(p.y - q.y), std::fabs(p.z - q.z)};
          c = {(p.x + q.x) / 2, (p.y + q.y) / 2, (p.z + q.z) / 2};
        } else {
          if (!has(a, "center") || !has(a, "size")) bad("Provide center and size, or corner1 and corner2");
          c = val::point(arg(a, "center"), "center");
          s = val::point(arg(a, "size"), "size");
        }
        val::positive(s.x, "size.x");
        val::positive(s.y, "size.y");
        val::positive(s.z, "size.z");
        return call("create_box", {{"center", val::to_json(c)}, {"size", val::to_json(s)}, {"combine", combine_of(a)}});
      }, true);

  add("create_cylinder",
      "Create a cylinder whose axis is parallel to +Z. `base_center` is the centre of the bottom face; the cylinder extends `height` along +Z. "
      "Units mm. Use combine='remove' to drill a hole.",
      schema({{"base_center", t_pt()}, {"radius", t_num()}, {"height", t_num()}, {"combine", t_combine()}}, {"base_center", "radius", "height"}),
      [](const json& a) {
        return call("create_cylinder", {{"base_center", val::to_json(val::point(req(a, "base_center"), "base_center"))},
                                        {"radius", val::positive(req(a, "radius"), "radius")},
                                        {"height", val::positive(req(a, "height"), "height")}, {"combine", combine_of(a)}});
      }, true);

  add("create_sphere", "Create a sphere. Units mm.",
      schema({{"center", t_pt()}, {"radius", t_num()}, {"combine", t_combine()}}, {"center", "radius"}), [](const json& a) {
        return call("create_sphere", {{"center", val::to_json(val::point(req(a, "center"), "center"))},
                                      {"radius", val::positive(req(a, "radius"), "radius")}, {"combine", combine_of(a)}});
      }, true);

  // ============================================================ profile features
  add("extrude_polygon",
      "FALLBACK - prefer sketch_extrude (a real, editable sketch); use this only when a sketch cannot express the outline. Extrude a closed planar polygon of loose wireframe into a solid.\n\npoints: vertices [[x,y,z],...] in order around the outline (>=3, planar, do not repeat the "
      "first point). distance: end offset in mm from the profile plane along the extrusion direction (> start). start: start offset in mm "
      "(default 0). direction: optional extrusion direction vector; by default the profile normal is used. Pass it explicitly (e.g. [0,0,1]) when "
      "the sign of the normal matters. draft_angle in degrees. Check the returned bounding box to confirm the extrusion went the intended way.",
      schema({{"points", t_arr(t_pt())}, {"distance", t_num()}, {"start", t_num()}, {"direction", t_pt()}, {"draft_angle", t_num()}, {"combine", t_combine()}},
             {"points", "distance"}),
      [](const json& a) {
        auto pts = val::polygon(req(a, "points"));
        double distance = val::number(req(a, "distance"), "distance"), start = num_or(a, "start", 0.0);
        if (distance <= start) bad("distance must be greater than start", "distance is the END offset; use start for an offset start.");
        return call("extrude_polygon", {{"points", pts_json(pts)}, {"distance", distance}, {"start", start}, {"draft_angle", num_or(a, "draft_angle", 0.0)},
                                        {"combine", combine_of(a)}, {"direction", opt_vector(a, "direction")}});
      }, true);

  add("revolve_polygon",
      "FALLBACK - prefer sketch_revolve (a real, editable sketch). Revolve a closed planar polygon of loose wireframe around an axis.\n\npoints: profile vertices [[x,y,z],...] (>=3, planar, same plane as the axis). "
      "axis_point/axis_direction define the rotation axis; the profile must not cross the axis. angle: end angle in degrees (default 360, full turn), "
      "start_angle in degrees (default 0).",
      schema({{"points", t_arr(t_pt())}, {"axis_point", t_pt()}, {"axis_direction", t_pt()}, {"angle", t_num()}, {"start_angle", t_num()}, {"combine", t_combine()}},
             {"points", "axis_point", "axis_direction"}),
      [](const json& a) {
        auto pts = val::polygon(req(a, "points"));
        double angle = num_or(a, "angle", 360.0), sa = num_or(a, "start_angle", 0.0);
        check_angles(angle, sa);
        val::V3 ap = val::point(req(a, "axis_point"), "axis_point"), ad = val::vector(req(a, "axis_direction"), "axis_direction");
        val::check_axis_not_crossed_3d(pts, ap, ad);
        return call("revolve_polygon", {{"points", pts_json(pts)}, {"axis_point", val::to_json(ap)}, {"axis_direction", val::to_json(ad)},
                                        {"angle", angle}, {"start_angle", sa}, {"combine", combine_of(a)}});
      }, true);

  add("extrude_profile",
      "FALLBACK - prefer sketch_extrude (a real, editable sketch that also takes lines, arcs and circles). Extrude a closed loose-wireframe profile of lines, arcs and circles.\n\n"
      "segments (mm), in order around the outline; the last end must equal the first start:\n"
      "  {\"type\":\"line\",\"start\":[x,y,z],\"end\":[x,y,z]}\n"
      "  {\"type\":\"arc\",\"start\":[..],\"through\":[..],\"end\":[..]}   (through = any point on the arc between start and end)\n"
      "  {\"type\":\"circle\",\"center\":[..],\"radius\":r,\"normal\":[0,0,1]}   (a circle is the ONLY segment of its profile)\n"
      "Other parameters as in extrude_polygon. Hole example: create the solid, then extrude a circle with combine='remove' and a distance that goes through it.",
      schema({{"segments", t_segments("Profile segments, see the tool description.")}, {"distance", t_num()}, {"start", t_num()}, {"direction", t_pt()},
              {"draft_angle", t_num()}, {"combine", t_combine()}}, {"segments", "distance"}),
      [](const json& a) {
        json segs = val::segments(req(a, "segments"));
        double distance = val::number(req(a, "distance"), "distance"), start = num_or(a, "start", 0.0);
        if (distance <= start) bad("distance must be greater than start");
        return call("extrude_profile", {{"segments", segs}, {"distance", distance}, {"start", start}, {"draft_angle", num_or(a, "draft_angle", 0.0)},
                                        {"combine", combine_of(a)}, {"direction", opt_vector(a, "direction")}});
      }, true);

  add("revolve_profile",
      "FALLBACK - prefer sketch_revolve (a real, editable sketch). Revolve a closed loose-wireframe profile of lines/arcs (same segment format as extrude_profile) around an axis. Typical use: a half cross-section of a shaft, "
      "pulley or bowl. Angles in degrees.",
      schema({{"segments", t_segments("Profile segments (3D), see extrude_profile.")}, {"axis_point", t_pt()}, {"axis_direction", t_pt()}, {"angle", t_num()},
              {"start_angle", t_num()}, {"combine", t_combine()}}, {"segments", "axis_point", "axis_direction"}),
      [](const json& a) {
        json segs = val::segments(req(a, "segments"));
        double angle = num_or(a, "angle", 360.0), sa = num_or(a, "start_angle", 0.0);
        check_angles(angle, sa);
        val::V3 ap = val::point(req(a, "axis_point"), "axis_point"), ad = val::vector(req(a, "axis_direction"), "axis_direction");
        val::check_axis_not_crossed_3d(val::profile_points(segs), ap, ad);
        return call("revolve_profile", {{"segments", segs}, {"axis_point", val::to_json(ap)}, {"axis_direction", val::to_json(ad)},
                                        {"angle", angle}, {"start_angle", sa}, {"combine", combine_of(a)}});
      }, true);

  add("sketch_extrude",
      "Create a real SKETCH on a plane and extrude it (the sketch stays in the feature tree, so the part can be edited from the sketch later; "
      "extrude_profile/extrude_polygon only draw loose wireframe instead).\n\n"
      "plane: 'XY' (normal +Z), 'YZ' (normal +X), 'ZX' (normal +Y); or {\"name\":\"XY\",\"origin\":[0,0,20]} for an offset plane; or a full frame "
      "{\"origin\":[..],\"x_axis\":[..],\"y_axis\":[..]} (normal = x_axis x y_axis).\n"
      "segments: 2D points [x, y] in the sketch frame (mm), one closed loop of lines/arcs, or a single circle:\n"
      "  {\"type\":\"line\",\"start\":[x,y],\"end\":[x,y]} | {\"type\":\"arc\",\"start\":[..],\"through\":[..],\"end\":[..]} | {\"type\":\"circle\",\"center\":[x,y],\"radius\":r}\n"
      "distance: extrusion depth along the plane normal (mm); start: start offset; symmetric: extrude distance/2 to both sides; reverse: go against the normal. "
      "dimensions=true adds driving dimensions (line lengths, circle diameters) to the sketch (best effort; the result reports dimensions_added / dimensions_failed).\n"
      "holes: optional inner loops (each a segment list in the same 2D format, e.g. a circle) cut out of the profile, so one call makes a plate with holes or a "
      "washer. Inner loops must lie inside the outer loop and not touch it.",
      schema({{"plane", t_plane()}, {"segments", t_segments("2D segments, see the tool description.")}, {"distance", t_num()}, {"start", t_num()},
              {"symmetric", t_bool()}, {"reverse", t_bool()}, {"draft_angle", t_num()}, {"dimensions", t_bool()}, {"combine", t_combine()},
              {"holes", t_arr(t_segments(nullptr), "Inner loops (lists of 2D segments).")}}, {"plane", "segments", "distance"}),
      [](const json& a) {
        val::Frame frame = val::sketch_plane(req(a, "plane"));
        json segs = val::segments2d(req(a, "segments"));
        double distance = val::number(req(a, "distance"), "distance");
        if (distance <= 0) bad("distance must be positive");
        double start, end;
        if (bool_or(a, "symmetric", false)) { start = -distance / 2.0; end = distance / 2.0; }
        else { start = num_or(a, "start", 0.0); end = start + distance; }
        val::V3 n = val::plane_normal(frame);
        json params = {{"plane", val::frame_json(frame)}, {"segments", segs}, {"distance", end}, {"start", start}, {"draft_angle", num_or(a, "draft_angle", 0.0)},
                       {"direction", bool_or(a, "reverse", false) ? val::to_json({-n.x, -n.y, -n.z}) : json(nullptr)},
                       {"dimensions", bool_or(a, "dimensions", true)}, {"combine", combine_of(a)}};
        json inner = val::inner_loops2d(arg(a, "holes"));
        if (!inner.empty()) params["inner_loops"] = inner;
        return call("sketch_extrude", params);
      }, true);

  add("sketch_revolve",
      "Create a real sketch on a plane and revolve it. plane/segments as in sketch_extrude; axis_point and axis_direction are 2D ([x, y]) in the sketch frame "
      "and the profile must stay on ONE side of the axis. Angles in degrees.",
      schema({{"plane", t_plane()}, {"segments", t_segments("2D segments, see sketch_extrude.")}, {"axis_point", t_pt2()}, {"axis_direction", t_pt2()},
              {"angle", t_num()}, {"start_angle", t_num()}, {"dimensions", t_bool()}, {"combine", t_combine()}},
             {"plane", "segments", "axis_point", "axis_direction"}),
      [](const json& a) {
        val::Frame frame = val::sketch_plane(req(a, "plane"));
        json segs = val::segments2d(req(a, "segments"));
        double angle = num_or(a, "angle", 360.0), sa = num_or(a, "start_angle", 0.0);
        check_angles(angle, sa);
        const json &ap = req(a, "axis_point"), &ad = req(a, "axis_direction");
        auto ok2 = [](const json& v) { return v.is_array() && v.size() == 2 && v[0].is_number() && v[1].is_number(); };
        if (!ok2(ap) || !ok2(ad) || (ad[0].get<double>() == 0 && ad[1].get<double>() == 0))
          bad("axis_point and axis_direction must be 2D [x, y] (direction not zero)");
        val::check_axis_not_crossed_2d(val::profile_points2d(segs), ap[0].get<double>(), ap[1].get<double>(), ad[0].get<double>(), ad[1].get<double>());
        return call("sketch_revolve", {{"plane", val::frame_json(frame)}, {"segments", segs}, {"axis_point", json::array({ap[0], ap[1]})},
                                       {"axis_direction", json::array({ad[0], ad[1]})}, {"angle", angle}, {"start_angle", sa},
                                       {"dimensions", bool_or(a, "dimensions", true)}, {"combine", combine_of(a)}});
      }, true);

  // ============================================================ modify
  add("boolean",
      "Combine solids: base (+|-|intersect) tools. Returns the surviving shape ids and the base solid's bbox/volume. keep_tools=true keeps the tool solids "
      "instead of consuming them. subtract / intersect fail (BOOLEAN_NO_OVERLAP / BOOLEAN_NO_EFFECT, nothing modified) when the tool does not actually overlap "
      "the base.",
      schema({{"operation", t_enum({"add", "subtract", "intersect"})}, {"base_id", t_int()}, {"tool_ids", t_ids()}, {"keep_tools", t_bool()}},
             {"operation", "base_id", "tool_ids"}),
      [](const json& a) {
        std::string op = req(a, "operation").is_string() ? arg(a, "operation").get<std::string>() : "";
        if (op != "add" && op != "subtract" && op != "intersect") bad("operation must be one of add, subtract, intersect");
        return call("boolean", {{"operation", op}, {"base_id", val::one_id(req(a, "base_id"), "base_id")},
                                {"tool_ids", val::id_list(req(a, "tool_ids"), "tool_ids")}, {"keep_tools", bool_or(a, "keep_tools", false)}});
      });

  add("fillet_edges",
      "Fillet edges of a solid with a constant radius (mm). Without edge_ids ALL edges of the shape are filleted. Get edge ids with list_edges. "
      "A radius that is too large fails with a modeling error; try a smaller one.",
      schema({{"shape_id", t_int()}, {"radius", t_num()}, {"edge_ids", t_ids()}}, {"shape_id", "radius"}), [](const json& a) {
        json p = {{"shape_id", val::one_id(req(a, "shape_id"), "shape_id")}, {"radius", val::positive(req(a, "radius"), "radius")}};
        if (has(a, "edge_ids")) p["edge_ids"] = val::id_list(arg(a, "edge_ids"), "edge_ids");
        return call("fillet_edges", p);
      });

  add("chamfer_edges",
      "Chamfer edges of a solid. distance (mm) is the symmetric setback; give distance2 for an asymmetric chamfer. Without edge_ids ALL edges are chamfered. "
      "Use list_edges (it returns each edge's bounding box) to pick edges.",
      schema({{"shape_id", t_int()}, {"distance", t_num()}, {"edge_ids", t_ids()}, {"distance2", t_num()}}, {"shape_id", "distance"}), [](const json& a) {
        json p = {{"shape_id", val::one_id(req(a, "shape_id"), "shape_id")}, {"distance", val::positive(req(a, "distance"), "distance")}};
        if (has(a, "edge_ids")) p["edge_ids"] = val::id_list(arg(a, "edge_ids"), "edge_ids");
        if (has(a, "distance2")) p["distance2"] = val::positive(arg(a, "distance2"), "distance2");
        return call("chamfer_edges", p);
      });

  add("shell_shape",
      "Hollow out a solid with a uniform wall `thickness` (mm). open_face_ids are faces that are removed (left open), e.g. the top face of a box to make an "
      "open container; get them from list_faces (planar faces report plane_normal). inward=true (default) keeps the outer size and cuts the cavity inside; "
      "inward=false grows the wall outward.",
      schema({{"shape_id", t_int()}, {"thickness", t_num()}, {"open_face_ids", t_ids()}, {"inward", t_bool()}}, {"shape_id", "thickness"}), [](const json& a) {
        json p = {{"shape_id", val::one_id(req(a, "shape_id"), "shape_id")}, {"thickness", val::positive(req(a, "thickness"), "thickness")},
                  {"inward", bool_or(a, "inward", true)}};
        if (has(a, "open_face_ids") && !arg(a, "open_face_ids").empty()) p["open_face_ids"] = val::id_list(arg(a, "open_face_ids"), "open_face_ids");
        return call("shell_shape", p);
      });

  add("undo",
      "Undo the last `steps` operations (1..50); one modeling tool call is one step. Returns the remaining shapes plus removed_shape_ids / added_shape_ids so you "
      "can see what actually changed.",
      schema({{"steps", t_int()}}), [](const json& a) {
        int steps = int_or(a, "steps", 1);
        if (steps < 1 || steps > 50) bad("steps must be between 1 and 50");
        return call("undo", {{"steps", steps}});
      });

  add("redo", "Redo `steps` previously undone operations (1..50).", schema({{"steps", t_int()}}), [](const json& a) {
    int steps = int_or(a, "steps", 1);
    if (steps < 1 || steps > 50) bad("steps must be between 1 and 50");
    return call("redo", {{"steps", steps}});
  });

  // ============================================================ features
  add("create_hole",
      "Drill hole(s) into a planar face, perpendicular to it. face_id from list_faces; points: [[x,y,z],...] locations (projected onto the face); diameter in mm; "
      "give depth (blind) or through=true. counterbore/countersink use diameter2 (default 1.6 x diameter) and depth2 (default 0.6 x diameter). Returns feature_id "
      "(usable by pattern_linear / pattern_circular / mirror) and the resulting shapes with bbox and volume.",
      schema({{"face_id", t_int()}, {"points", t_arr(t_pt())}, {"diameter", t_num()}, {"depth", t_num()}, {"through", t_bool()},
              {"hole_type", t_enum({"simple", "counterbore", "countersink"}, "simple")}, {"diameter2", t_num()}, {"depth2", t_num()},
              {"thread", t_obj("Makes a threaded hole: {\"major_diameter\":mm,\"pitch\":mm,\"length\":mm?}. `diameter` is then the drilled (minor) diameter, e.g. M8x1.25: diameter 6.8, major_diameter 8, pitch 1.25.")}},
             {"face_id", "points", "diameter"}),
      [](const json& a) {
        const json& pts = req(a, "points");
        if (!pts.is_array() || pts.empty()) bad("points must list at least one hole location");
        json plist = json::array();
        for (const auto& p : pts) plist.push_back(val::to_json(val::point(p, "points[i]")));
        bool through = bool_or(a, "through", false);
        std::string ht = has(a, "hole_type") && arg(a, "hole_type").is_string() ? arg(a, "hole_type").get<std::string>() : "simple";
        if (ht != "simple" && ht != "counterbore" && ht != "countersink") bad("hole_type must be one of simple, counterbore, countersink");
        json p = {{"face_id", val::one_id(req(a, "face_id"), "face_id")}, {"points", plist}, {"diameter", val::positive(req(a, "diameter"), "diameter")},
                  {"through", through}, {"hole_type", ht}};
        if (!through) {
          if (!has(a, "depth")) bad("A blind hole needs depth (or set through=true)");
          p["depth"] = val::positive(arg(a, "depth"), "depth");
        }
        if (has(a, "diameter2")) p["diameter2"] = val::positive(arg(a, "diameter2"), "diameter2");
        if (has(a, "depth2")) p["depth2"] = val::positive(arg(a, "depth2"), "depth2");
        if (has(a, "thread")) {
          const json& t = arg(a, "thread");
          if (!t.is_object()) bad("thread must be {major_diameter, pitch, length?}");
          json tj = {{"major_diameter", val::positive(req(t, "major_diameter"), "thread.major_diameter")}, {"pitch", val::positive(req(t, "pitch"), "thread.pitch")}};
          if (has(t, "length")) tj["length"] = val::positive(arg(t, "length"), "thread.length");
          p["thread"] = tj;
        }
        return call("create_hole", p);
      });

  add("pattern_linear",
      "Linear pattern of a solid (shape_id) or of features (feature_ids, e.g. a hole). count includes the original. spacing in mm along direction; add "
      "count2/spacing2/direction2 for a second direction (grid). For solids use combine='add' to fuse the copies, 'none' for separate bodies.",
      schema({{"count", t_int()}, {"spacing", t_num()}, {"direction", t_pt()}, {"shape_id", t_int()}, {"feature_ids", t_ids()}, {"count2", t_int()},
              {"spacing2", t_num()}, {"direction2", t_pt()}, {"combine", t_combine()}}, {"count", "spacing", "direction"}),
      [](const json& a) {
        auto tgt = target_of(a);
        int count = int_or(a, "count", 0);
        if (count < 2) bad("count must be an integer >= 2 (it includes the original)");
        json p = {{"kind", "linear"}, {"count", count}, {"spacing", val::positive(req(a, "spacing"), "spacing")},
                  {"direction", val::to_json(val::vector(req(a, "direction"), "direction"))}, {"combine", combine_of(a)}};
        for (auto it = tgt.second.begin(); it != tgt.second.end(); ++it) p[it.key()] = it.value();
        if (has(a, "count2") || has(a, "spacing2") || has(a, "direction2")) {
          int c2 = int_or(a, "count2", 0);
          if (!has(a, "count2") || !has(a, "spacing2") || !has(a, "direction2") || c2 < 2)
            bad("A second direction needs count2 (>=2), spacing2 and direction2 together");
          p["count2"] = c2;
          p["spacing2"] = val::positive(arg(a, "spacing2"), "spacing2");
          p["direction2"] = val::to_json(val::vector(arg(a, "direction2"), "direction2"));
        }
        return call(tgt.first ? "pattern_shape" : "pattern_feature", p);
      }, true);

  add("pattern_circular",
      "Circular pattern around an axis. count includes the original; angle is the step between neighbours in degrees (default 360/count = evenly around the full "
      "circle). Target is a solid (shape_id) or features (feature_ids).",
      schema({{"count", t_int()}, {"axis_point", t_pt()}, {"axis_direction", t_pt()}, {"angle", t_num()}, {"shape_id", t_int()}, {"feature_ids", t_ids()},
              {"combine", t_combine()}}, {"count", "axis_point", "axis_direction"}),
      [](const json& a) {
        auto tgt = target_of(a);
        int count = int_or(a, "count", 0);
        if (count < 2) bad("count must be an integer >= 2 (it includes the original)");
        double step = has(a, "angle") ? val::positive(arg(a, "angle"), "angle") : 360.0 / count;
        json p = {{"kind", "circular"}, {"count", count}, {"angle", step}, {"axis_point", val::to_json(val::point(req(a, "axis_point"), "axis_point"))},
                  {"axis_direction", val::to_json(val::vector(req(a, "axis_direction"), "axis_direction"))}, {"combine", combine_of(a)}};
        for (auto it = tgt.second.begin(); it != tgt.second.end(); ++it) p[it.key()] = it.value();
        return call(tgt.first ? "pattern_shape" : "pattern_feature", p);
      }, true);

  add("mirror",
      "Mirror a solid (shape_id) or features (feature_ids) across a plane: 'XY', 'YZ', 'ZX' (through the origin), {\"name\":\"YZ\",\"origin\":[x,y,z]} for an offset "
      "plane, or {\"origin\":[..],\"normal\":[..]}. For a solid, combine='add' fuses the mirror image into the original (symmetric part), 'none' keeps it as a "
      "separate body. Fails with MIRROR_NO_RESULT (and rolls back) if the plane coincides with / passes through the solid so that nothing new is produced.",
      schema({{"plane", {{"anyOf", json::array({t_enum({"XY", "YZ", "ZX"}), t_obj()})}}}, {"shape_id", t_int()}, {"feature_ids", t_ids()}, {"combine", t_combine()}},
             {"plane"}),
      [](const json& a) {
        auto tgt = target_of(a);
        json p = {{"plane", val::mirror_plane(req(a, "plane"))}, {"combine", combine_of(a)}};
        for (auto it = tgt.second.begin(); it != tgt.second.end(); ++it) p[it.key()] = it.value();
        return call(tgt.first ? "mirror_shape" : "mirror_feature", p);
      }, true);

  add("draft_faces",
      "Add a draft (taper) to faces: they tilt by `angle` degrees (0 < angle < 89) about the neutral faces, which stay fixed, relative to the pull_direction vector "
      "(the mold opening direction). Use list_faces to pick faces.",
      schema({{"face_ids", t_ids()}, {"neutral_face_ids", t_ids()}, {"angle", t_num()}, {"pull_direction", t_pt()}},
             {"face_ids", "neutral_face_ids", "angle", "pull_direction"}),
      [](const json& a) {
        double angle = val::number(req(a, "angle"), "angle");
        if (!(0 < angle && angle < 89)) bad("angle must be between 0 and 89 degrees");
        return call("draft_faces", {{"face_ids", val::id_list(req(a, "face_ids"), "face_ids")},
                                    {"neutral_face_ids", val::id_list(req(a, "neutral_face_ids"), "neutral_face_ids")}, {"angle", angle},
                                    {"pull_direction", val::to_json(val::vector(req(a, "pull_direction"), "pull_direction"))}});
      });

  add("sweep",
      "Sweep a closed profile along a path. profile: closed loop of 3D segments (same line/arc/circle format as extrude_profile) lying in a plane PERPENDICULAR to the "
      "path at its start. path: connected open chain of 3D {\"type\":\"line\"|\"arc\",...} segments, each start equal to the previous end. Example: a pipe = circle "
      "profile + straight or arced path. Loose wireframe is used for profile and path (no sketch).",
      schema({{"profile", t_segments("Closed profile segments (3D).")}, {"path", t_segments("Connected open chain of line/arc segments (3D).")}, {"combine", t_combine()}},
             {"profile", "path"}),
      [](const json& a) {
        return call("sweep", {{"profile", val::segments(req(a, "profile"), "profile")}, {"path", val::path_segments(req(a, "path"))}, {"combine", combine_of(a)}});
      }, true);

  add("loft",
      "Loft a solid through 2 or more closed cross-sections (each a closed loop of 3D segments, e.g. a circle at z=0 and a square at z=50). Keep the sections in order "
      "and give them the same segment count/orientation for a clean result.",
      schema({{"sections", t_arr(t_segments(nullptr), "Closed cross-sections in order; each a list of 3D segments.")}, {"combine", t_combine()}}, {"sections"}),
      [](const json& a) {
        const json& secs = req(a, "sections");
        if (!secs.is_array() || secs.size() < 2) bad("loft needs at least 2 sections");
        json out = json::array();
        for (size_t i = 0; i < secs.size(); ++i) out.push_back(val::segments(secs[i], "sections[" + std::to_string(i) + "]"));
        return call("loft", {{"sections", out}, {"combine", combine_of(a)}});
      }, true);

  add("helical_sweep",
      "Sweep a closed profile along a helix (ZW3D spiral sweep): springs, coils and REAL modeled threads. profile: closed loop of 3D segments (same format as "
      "extrude_profile), e.g. a triangle / trapezoid thread tooth or a circle for a spring wire, positioned at its radius from the axis. axis_direction: helix axis "
      "direction (the axis is assumed to pass through the world origin, so build the part around the origin or check the result with get_bounding_box). turns: number "
      "of turns; pitch: advance along the axis per turn (mm). Threads: make the tooth profile and use combine='remove' on a bolt/hole body (cut thread) or 'add' "
      "(raised thread); combine='none' gives a free coil. clockwise=true turns the helix clockwise about the axis direction (check handedness in a screenshot), taper_angle for pipe threads, flip reverses the direction. For a thread callout "
      "without geometry use add_thread.",
      schema({{"profile", t_segments("Closed profile segments (3D), e.g. the thread tooth.")}, {"axis_direction", t_pt("Helix axis direction.")},
              {"turns", t_num("Number of turns (> 0).")}, {"pitch", t_num("Advance per turn in mm (> 0).")}, {"clockwise", t_bool()}, {"flip", t_bool()},
              {"taper_angle", t_num("Degrees, 0 = straight.")}, {"combine", t_combine()}},
             {"profile", "axis_direction", "turns", "pitch"}),
      [](const json& a) {
        return call("helical_sweep", {{"profile", val::segments(req(a, "profile"), "profile")}, {"axis_direction", val::to_json(val::vector(req(a, "axis_direction"), "axis_direction"))},
                                      {"turns", val::positive(req(a, "turns"), "turns")}, {"pitch", val::positive(req(a, "pitch"), "pitch")},
                                      {"clockwise", bool_or(a, "clockwise", false)}, {"flip", bool_or(a, "flip", false)},
                                      {"taper_angle", num_or(a, "taper_angle", 0.0)}, {"combine", combine_of(a)}});
      }, true);

  add("add_thread",
      "Attach a thread definition (cosmetic thread, shown in drawings/UI, no modeled geometry) to a cylindrical face such as a hole wall or a shaft. face_id from "
      "list_faces; point: a point on or near the face at the end where the thread starts; diameter / pitch / length in mm. For real thread geometry use helical_sweep.",
      schema({{"face_id", t_int()}, {"point", t_pt()}, {"diameter", t_num()}, {"pitch", t_num()}, {"length", t_num()}}, {"face_id", "point", "diameter", "pitch", "length"}),
      [](const json& a) {
        return call("add_thread", {{"face_id", val::one_id(req(a, "face_id"), "face_id")}, {"point", val::to_json(val::point(req(a, "point"), "point"))},
                                   {"diameter", val::positive(req(a, "diameter"), "diameter")}, {"pitch", val::positive(req(a, "pitch"), "pitch")},
                                   {"length", val::positive(req(a, "length"), "length")}});
      });

  add("set_color",
      "Colour faces or solids with an RGB colour ([r,g,b], 0-255). True colour applies to faces: pass face ids from list_faces to paint a whole solid; solids / other "
      "entities may be mapped to the nearest standard colour.",
      schema({{"ids", t_ids("Face ids (preferred) or shape ids.")}, {"rgb", t_arr(t_int(), "[r, g, b], each 0-255", 3, 3)}}, {"ids", "rgb"}),
      [](const json& a) {
        const json& c = req(a, "rgb");
        if (!c.is_array() || c.size() != 3) bad("rgb must be [r, g, b]");
        for (const auto& v : c)
          if (!v.is_number_integer() || v.get<int>() < 0 || v.get<int>() > 255) bad("each rgb component must be an integer 0-255");
        return call("set_color", {{"ids", val::id_list(req(a, "ids"), "ids")}, {"rgb", c}});
      });

  add("rename_feature", "Rename a history feature (feature_id from list_features; name up to 31 bytes).",
      schema({{"feature_id", t_int()}, {"name", t_str()}}, {"feature_id", "name"}),
      [](const json& a) {
        std::string n = trim(str_req(a, "name"));
        if (n.empty() || n.size() > 31) bad("name must be 1-31 bytes");
        return call("rename_feature", {{"feature_id", val::one_id(req(a, "feature_id"), "feature_id")}, {"name", n}});
      });

  add("move_shapes",
      "Move and/or rotate solids. translate: [dx,dy,dz] mm. rotate: {axis_point, axis_direction, angle} in degrees (right-hand rule); the rotation is applied first, then the "
      "translation. Ids may change: use the returned shapes.",
      schema({{"shape_ids", t_ids()}, {"translate", t_pt()},
              {"rotate", t_obj("{\"axis_point\":[x,y,z],\"axis_direction\":[x,y,z],\"angle\":degrees}")}},
             {"shape_ids"}),
      [](const json& a) {
        json p = {{"shape_ids", val::id_list(req(a, "shape_ids"), "shape_ids")}};
        if (!has(a, "translate") && !has(a, "rotate")) bad("give translate and/or rotate");
        if (has(a, "translate")) p["translate"] = val::to_json(val::point(arg(a, "translate"), "translate"));
        if (has(a, "rotate")) {
          const json& r = arg(a, "rotate");
          if (!r.is_object()) bad("rotate must be {axis_point, axis_direction, angle}");
          p["rotate"] = {{"axis_point", val::to_json(val::point(req(r, "axis_point"), "rotate.axis_point"))},
                         {"axis_direction", val::to_json(val::vector(req(r, "axis_direction"), "rotate.axis_direction"))},
                         {"angle", val::number(req(r, "angle"), "rotate.angle")}};
        }
        return call("move_shapes", p);
      });

  add("scale_shapes",
      "Scale solids about a centre point (default the origin). scale: one positive number for uniform scaling or [sx,sy,sz]. Ids may change: use the returned shapes.",
      schema({{"shape_ids", t_ids()}, {"scale", {{"anyOf", json::array({t_num(), t_pt()})}}}, {"center", t_pt()}}, {"shape_ids", "scale"}),
      [](const json& a) {
        json p = {{"shape_ids", val::id_list(req(a, "shape_ids"), "shape_ids")}};
        const json& s = req(a, "scale");
        if (s.is_number()) p["scale"] = val::positive(s, "scale");
        else {
          json v = json::array();
          if (!s.is_array() || s.size() != 3) bad("scale must be a positive number or [sx, sy, sz]");
          for (size_t i = 0; i < 3; ++i) v.push_back(val::positive(s[i], "scale[i]"));
          p["scale"] = v;
        }
        if (has(a, "center")) p["center"] = val::to_json(val::point(arg(a, "center"), "center"));
        return call("scale_shapes", p);
      });

  add("create_rib",
      "Add a rib (stiffener). profile: an OPEN chain of connected 3D line/arc segments (same format as sweep's path) that touches the solid's surface at both ends; "
      "thickness: rib width (mm). side: both (default, symmetric), first or second; perpendicular=true extrudes perpendicular to the profile plane instead of parallel; "
      "flip reverses the material direction. shape_id is optional (auto-detect). Fails with RIB_NO_EFFECT if nothing was added.",
      schema({{"profile", t_segments("Open chain of line/arc segments (3D).")}, {"thickness", t_num()}, {"shape_id", t_int()},
              {"side", t_enum({"both", "first", "second"}, "both")}, {"perpendicular", t_bool()}, {"flip", t_bool()}},
             {"profile", "thickness"}),
      [](const json& a) {
        json p = {{"profile", val::path_segments(req(a, "profile"), "profile")}, {"thickness", val::positive(req(a, "thickness"), "thickness")},
                  {"perpendicular", bool_or(a, "perpendicular", false)}, {"flip", bool_or(a, "flip", false)}};
        if (has(a, "shape_id")) p["shape_id"] = val::one_id(arg(a, "shape_id"), "shape_id");
        if (has(a, "side")) p["side"] = enum_of(a, "side", {"both", "first", "second"});
        return call("create_rib", p);
      });

  add("thicken_faces",
      "Create a solid by thickening faces (face_ids from list_faces). thickness in mm: positive = outward, negative = inward (not 0).",
      schema({{"face_ids", t_ids()}, {"thickness", t_num()}}, {"face_ids", "thickness"}),
      [](const json& a) {
        double t = val::number(req(a, "thickness"), "thickness");
        if (t == 0) bad("thickness must not be 0");
        return call("thicken_faces", {{"face_ids", val::id_list(req(a, "face_ids"), "face_ids")}, {"thickness", t}});
      });

  add("trim_shape",
      "Cut away one side of a solid with a plane ({origin, normal}, same format as mirror) or with cutter faces/solids (cutter_ids). The side the normal points to is "
      "removed unless flip=true. Fails with TRIM_NO_EFFECT if the cutter does not pass through the solid.",
      schema({{"shape_id", t_int()}, {"plane", t_obj("{\"origin\":[x,y,z],\"normal\":[x,y,z]}")}, {"cutter_ids", t_ids()}, {"flip", t_bool()}, {"keep_cutters", t_bool()}},
             {"shape_id"}),
      [](const json& a) {
        json p = {{"shape_id", val::one_id(req(a, "shape_id"), "shape_id")}, {"flip", bool_or(a, "flip", false)}, {"keep_cutters", bool_or(a, "keep_cutters", false)}};
        if (!has(a, "plane") && !has(a, "cutter_ids")) bad("give plane and/or cutter_ids");
        if (has(a, "plane")) p["plane"] = val::mirror_plane(arg(a, "plane"));
        if (has(a, "cutter_ids")) p["cutter_ids"] = val::id_list(arg(a, "cutter_ids"), "cutter_ids");
        return call("trim_shape", p);
      });

  add("split_shape",
      "Split a solid into separate solids with a plane ({origin, normal}) or cutter faces/solids (cutter_ids). Both halves are kept. Fails with SPLIT_NO_RESULT if the "
      "cutter does not pass through the solid.",
      schema({{"shape_id", t_int()}, {"plane", t_obj("{\"origin\":[x,y,z],\"normal\":[x,y,z]}")}, {"cutter_ids", t_ids()}, {"keep_cutters", t_bool()}}, {"shape_id"}),
      [](const json& a) {
        json p = {{"shape_id", val::one_id(req(a, "shape_id"), "shape_id")}, {"keep_cutters", bool_or(a, "keep_cutters", false)}};
        if (!has(a, "plane") && !has(a, "cutter_ids")) bad("give plane and/or cutter_ids");
        if (has(a, "plane")) p["plane"] = val::mirror_plane(arg(a, "plane"));
        if (has(a, "cutter_ids")) p["cutter_ids"] = val::id_list(arg(a, "cutter_ids"), "cutter_ids");
        return call("split_shape", p);
      });

  add("extend_face",
      "Extend a face outward past all of its edges by distance (mm, > 0). face_id from list_faces.",
      schema({{"face_id", t_int()}, {"distance", t_num()}}, {"face_id", "distance"}),
      [](const json& a) {
        return call("extend_face", {{"face_id", val::one_id(req(a, "face_id"), "face_id")}, {"distance", val::positive(req(a, "distance"), "distance")}});
      });

  add("list_features",
      "List the features of the active part's history: feature_id, name (e.g. 'Extrude1') and template. Use the ids with pattern_*/mirror feature targets and with "
      "get_feature_data / set_feature_value.",
      schema({}), [](const json&) { return call("list_features"); });

  add("get_feature_data",
      "Show the parameters of one feature: for every field its field_id, label, type and value (distances in mm, angles in degrees), plus `variable_name` "
      "(\"<FeatureName:field_id>\") for numeric fields. Pick the field you want to change, then call set_feature_value. Feature ids come from list_features.",
      schema({{"feature_id", t_int()}}, {"feature_id"}),
      [](const json& a) { return call("get_feature_data", {{"feature_id", val::one_id(req(a, "feature_id"), "feature_id")}}); });

  add("set_feature_value",
      "Change a numeric parameter of an existing feature (extrusion depth, fillet radius, hole diameter, ...) and regenerate the model: this is how to resize something "
      "after it was created. Look up field_id with get_feature_data. value is in mm (distance fields), degrees (angle fields) or unitless. Returns the updated shapes "
      "with bbox/volume; if the new value makes the geometry invalid, or the field turns out to be derived and changes nothing (e.g. the length of a box made from "
      "two corner points), the change is rolled back and an error with a hint is returned. Negative distances are refused unless allow_negative=true. IMPORTANT: "
      "regeneration may renumber ALL shape/face/edge ids (the result then has ids_renumbered=true): use only the ids in the returned shape_details and call "
      "list_faces / list_edges again.",
      schema({{"feature_id", t_int()}, {"field_id", t_int()}, {"value", t_num()}, {"allow_negative", t_bool("Allow a negative value for a distance field (default false: it flips the feature).")}},
             {"feature_id", "field_id", "value"}),
      [](const json& a) {
        const json& f = req(a, "field_id");
        if (!f.is_number_integer() || f.get<long long>() < 0) bad("field_id must be a non-negative integer", "Use a field_id from get_feature_data.");
        return call("set_feature_value", {{"feature_id", val::one_id(req(a, "feature_id"), "feature_id")}, {"field_id", f.get<int>()},
                                          {"value", val::number(req(a, "value"), "value")}, {"allow_negative", bool_or(a, "allow_negative", false)}});
      });

  // ============================================================ variables
  add("list_variables", "List the active part's variables (name, kind, value in mm/deg, expression).", schema({}),
      [](const json&) { return call("list_variables"); });

  add("set_variable",
      "Change an existing part variable (give a numeric value in mm/deg, or an expression string) and regenerate the part. Returns the new shapes with bbox/volume so "
      "the effect can be checked; a `note` warns if nothing changed. Regeneration may renumber shape/face/edge ids: use the ids in the returned shape_details.",
      schema({{"name", t_str()}, {"value", t_num()}, {"expression", t_str()}}, {"name"}), [](const json& a) {
        std::string name = str_req(a, "name");
        if (has(a, "value") == has(a, "expression")) bad("Give exactly one of value or expression");
        json p = {{"name", name}};
        if (has(a, "value")) p["value"] = val::number(arg(a, "value"), "value");
        else p["expression"] = val::check_expression(name, arg(a, "expression"));
        return call("set_variable", p);
      });

  add("add_variable", "Create a new part variable (kind: unitless number, distance in mm, angle in degrees).",
      schema({{"name", t_str()}, {"value", t_num()}, {"expression", t_str()}, {"kind", t_enum({"number", "distance", "angle"}, "number")}}, {"name"}),
      [](const json& a) {
        std::string name = str_req(a, "name");
        if (has(a, "value") == has(a, "expression")) bad("Give exactly one of value or expression");
        std::string kind = has(a, "kind") && arg(a, "kind").is_string() ? arg(a, "kind").get<std::string>() : "number";
        if (kind != "number" && kind != "distance" && kind != "angle") bad("kind must be one of number, distance, angle");
        json p = {{"name", name}, {"kind", kind}};
        if (has(a, "value")) p["value"] = val::number(arg(a, "value"), "value");
        else {
          if (!arg(a, "expression").is_string()) bad("expression must be a string");
          p["expression"] = arg(a, "expression");
        }
        return call("add_variable", p);
      });

  add("list_sketch_variables",
      "List the variables of a sketch created by sketch_extrude/sketch_revolve (use its sketch_id). The result also reports the sketch's dimension entities "
      "(`dimensions`) and, if the dimensions are not exposed as variables, a `note` saying so; in that case change the model with set_variable or edit the sketch in ZW3D.",
      schema({{"sketch_id", t_int()}}, {"sketch_id"}),
      [](const json& a) { return call("list_sketch_variables", {{"sketch_id", val::one_id(req(a, "sketch_id"), "sketch_id")}}); });

  add("set_sketch_variable",
      "Edit one sketch dimension and regenerate the model: this is how you resize a feature after the fact. Get names from list_sketch_variables. Returns the updated "
      "shapes with bbox/volume. Regeneration may renumber shape/face/edge ids: use the ids in the returned shape_details.",
      schema({{"sketch_id", t_int()}, {"name", t_str()}, {"value", t_num()}, {"expression", t_str()}}, {"sketch_id", "name"}), [](const json& a) {
        if (has(a, "value") == has(a, "expression")) bad("Give exactly one of value or expression");
        json p = {{"sketch_id", val::one_id(req(a, "sketch_id"), "sketch_id")}, {"name", str_req(a, "name")}};
        if (has(a, "value")) p["value"] = val::number(arg(a, "value"), "value");
        else {
          if (!arg(a, "expression").is_string()) bad("expression must be a string");
          p["expression"] = arg(a, "expression");
        }
        return call("set_sketch_variable", p);
      });

  // ============================================================ assemblies and drawings (basic)
  add("new_assembly",
      "Create a new assembly file (.Z3ASM) and make it active. Then use insert_component. Part modeling tools do not work inside an assembly: switch back with new_part "
      "or by opening a part.",
      schema({{"name", t_str()}}, {"name"}), [](const json& a) {
        std::string n = trim(str_req(a, "name"));
        if (n.empty()) bad("name must not be empty");
        return call("new_assembly", {{"name", n}});
      });

  add("insert_component",
      "Insert a saved part file (full path, e.g. C:\\work\\bolt.Z3PRT) into the active assembly at `position` [x,y,z] mm (default origin). anchor=true fixes it in place. "
      "Returns component_id.",
      schema({{"file", t_str()}, {"position", t_pt()}, {"anchor", t_bool()}, {"part_name", t_str()}}, {"file"}), [](const json& a) {
        std::string f = trim(str_req(a, "file"));
        if (f.empty()) bad("file must be the path of a saved part");
        json p = {{"file", f}, {"anchor", bool_or(a, "anchor", false)}};
        if (has(a, "position")) p["position"] = val::to_json(val::point(arg(a, "position"), "position"));
        if (has(a, "part_name") && arg(a, "part_name").is_string() && !arg(a, "part_name").get<std::string>().empty()) p["part_name"] = arg(a, "part_name");
        return call("insert_component", p);
      });

  add("list_components", "List the component ids in the active assembly.", schema({}), [](const json&) { return call("list_components"); });

  add("list_component_faces",
      "List the faces of an assembly component as pick paths (arrays of ids). Pass these paths unchanged as entity1 / entity2 of add_constraint. component_id from "
      "list_components. EXPERIMENTAL: if faces cannot be read the result carries a note; use add_constraint type 'lock' (needs only component ids) instead.",
      schema({{"component_id", t_int()}}, {"component_id"}),
      [](const json& a) { return call("list_component_faces", {{"component_id", val::one_id(req(a, "component_id"), "component_id")}}); });

  add("add_constraint",
      "Add an assembly constraint. type: lock (fix component2 relative to component1; give component1 / component2 ids), or coincident, concentric, distance, parallel, "
      "perpendicular, tangent, angular between two faces (entity1 / entity2 = pick paths from list_component_faces; value = offset distance in mm for 'distance' or the angle "
      "in degrees for 'angular'; same_direction flips the mating side). EXPERIMENTAL: face constraints are not yet verified in ZW3D.",
      schema({{"type", t_enum({"lock", "coincident", "concentric", "distance", "parallel", "perpendicular", "tangent", "angular"})},
              {"component1", t_int()}, {"component2", t_int()}, {"entity1", t_arr(t_int(), "Pick path from list_component_faces.")}, {"entity2", t_arr(t_int(), "Pick path from list_component_faces.")},
              {"value", t_num()}, {"same_direction", t_bool()}},
             {"type"}),
      [](const json& a) {
        std::string type = enum_of(a, "type", {"lock", "coincident", "concentric", "distance", "parallel", "perpendicular", "tangent", "angular"});
        json p = {{"type", type}, {"value", num_or(a, "value", 0.0)}, {"same_direction", bool_or(a, "same_direction", false)}};
        if (type == "lock") {
          p["component1"] = val::one_id(req(a, "component1"), "component1");
          p["component2"] = val::one_id(req(a, "component2"), "component2");
        } else {
          for (const char* k : {"entity1", "entity2"}) {
            const json& e = req(a, k);
            if (!e.is_array() || e.size() < 2 || e.size() > 400) bad(std::string(k) + " must be a pick path returned by list_component_faces");
            for (const auto& v : e)
              if (!v.is_number_integer()) bad(std::string(k) + " must contain integers only");
            p[k] = e;
          }
        }
        return call("add_constraint", p);
      });

  add("list_drawing_views",
      "List the drawings of the active file with their view ids and view types. Use the view_id with auto_dimension and add_bom.",
      schema({}), [](const json&) { return call("list_drawing_views"); });

  add("auto_dimension",
      "Add automatic dimensions to a drawing view (view_id from list_drawing_views): horizontal and vertical baseline dimensions for the view's geometry. EXPERIMENTAL.",
      schema({{"view_id", t_int()}}, {"view_id"}),
      [](const json& a) { return call("auto_dimension", {{"view_id", val::one_id(req(a, "view_id"), "view_id")}}); });

  add("add_bom",
      "Insert a bill-of-materials table for an assembly drawing view (view_id from list_drawing_views) at sheet position x, y (mm; default top-left area). EXPERIMENTAL.",
      schema({{"view_id", t_int()}, {"x", t_num()}, {"y", t_num()}}, {"view_id"}),
      [](const json& a) { return call("add_bom", {{"view_id", val::one_id(req(a, "view_id"), "view_id")}, {"x", num_or(a, "x", 10.0)}, {"y", num_or(a, "y", 200.0)}}); });

  add("new_drawing",
      "Create a new drawing (.Z3DRW) with the standard multi-view layout of a saved part or assembly (give its full path; save_part first). Dimensions: auto_dimension; "
      "parts list: add_bom; save_part (with a path) stores the drawing.",
      schema({{"name", t_str()}, {"part_file", t_str()}}, {"name", "part_file"}), [](const json& a) {
        std::string n = trim(str_req(a, "name")), f = trim(str_req(a, "part_file"));
        if (n.empty() || f.empty()) bad("name and part_file must not be empty", "part_file must be the full path of a saved part/assembly.");
        return call("new_drawing", {{"name", n}, {"part_file", f}});
      });

  // ============================================================ queries
  add("list_shapes", "List all solid shape ids in the active part, each with bounding box and volume.", schema({}), [](const json&) { return call("list_shapes"); });

  add("list_faces",
      "List the face ids of a shape with bounding box and surface type; planar faces include plane_normal (axis direction, sign not guaranteed to point outward), "
      "cylinders include radius and axis.",
      schema({{"shape_id", t_int()}}, {"shape_id"}), [](const json& a) { return call("list_faces", {{"shape_id", val::one_id(req(a, "shape_id"), "shape_id")}}); });

  add("list_edges",
      "List the edges of a shape: edge_ids plus `edges`, each with bounding_box and `curve` (line: start/end; arc/circle: center/radius). Use with fillet_edges / "
      "chamfer_edges to pick edges by position.",
      schema({{"shape_id", t_int()}}, {"shape_id"}), [](const json& a) { return call("list_edges", {{"shape_id", val::one_id(req(a, "shape_id"), "shape_id")}}); });

  add("get_mass_props",
      "Volume (mm^3), surface area (mm^2), mass_kg and centre of gravity [x,y,z] (mm) of a solid. density (kg/m^3) is optional; omit it to use the part's own material density.",
      schema({{"shape_id", t_int()}, {"density", t_num()}}, {"shape_id"}), [](const json& a) {
        json p = {{"shape_id", val::one_id(req(a, "shape_id"), "shape_id")}};
        if (has(a, "density")) p["density"] = val::positive(arg(a, "density"), "density");
        return call("get_mass_props", p);
      });

  add("get_bounding_box", "Axis-aligned bounding box {min:[x,y,z], max:[x,y,z], size:[dx,dy,dz]} in mm.", schema({{"shape_id", t_int()}}, {"shape_id"}),
      [](const json& a) { return call("get_bounding_box", {{"shape_id", val::one_id(req(a, "shape_id"), "shape_id")}}); });

  add("screenshot",
      "Capture the current ZW3D view as a PNG image (zoomed to fit unless zoom_all=false). view switches to a standard orientation first (front, back, top, bottom, "
      "right, left, iso) so you can inspect every side; display_mode: shaded, shaded_edges, wireframe, hidden (the view and mode stay changed afterwards).",
      schema({{"width", t_int()}, {"height", t_int()}, {"zoom_all", t_bool()},
              {"view", t_enum({"front", "back", "top", "bottom", "right", "left", "iso"})},
              {"display_mode", t_enum({"shaded", "shaded_edges", "wireframe", "hidden"})}}),
      [](const json& a) {
        int w = int_or(a, "width", 800), h = int_or(a, "height", 600);
        if (!(64 <= w && w <= 4096 && 64 <= h && h <= 4096)) bad("width and height must be between 64 and 4096");
        json p = {{"width", w}, {"height", h}, {"zoom_all", bool_or(a, "zoom_all", true)}};
        if (has(a, "view")) p["view"] = enum_of(a, "view", {"front", "back", "top", "bottom", "right", "left", "iso"});
        if (has(a, "display_mode")) p["display_mode"] = enum_of(a, "display_mode", {"shaded", "shaded_edges", "wireframe", "hidden"});
        ToolCall c = call("screenshot", p);
        c.screenshot = true;
        return c;
      });

  add("set_view",
      "Change the ZW3D viewport without taking a picture: view = front, back, top, bottom, right, left or iso; display_mode = shaded, shaded_edges, wireframe or hidden.",
      schema({{"view", t_enum({"front", "back", "top", "bottom", "right", "left", "iso"})},
              {"display_mode", t_enum({"shaded", "shaded_edges", "wireframe", "hidden"})}, {"zoom_all", t_bool()}}),
      [](const json& a) {
        json p = {{"zoom_all", bool_or(a, "zoom_all", true)}};
        if (has(a, "view")) p["view"] = enum_of(a, "view", {"front", "back", "top", "bottom", "right", "left", "iso"});
        if (has(a, "display_mode")) p["display_mode"] = enum_of(a, "display_mode", {"shaded", "shaded_edges", "wireframe", "hidden"});
        if (!has(a, "view") && !has(a, "display_mode")) bad("give view and/or display_mode");
        return call("set_view", p);
      });

  add("open_file",
      "Open an existing ZW3D file (.Z3PRT / .Z3 / .Z3ASM / .Z3DRW) and make it active, so you can inspect or modify an existing model. Returns the solids it contains.",
      schema({{"path", t_str("Full path of the file.")}}, {"path"}),
      [](const json& a) { return call("open_file", {{"path", str_req(a, "path")}}); });

  add("import_file",
      "Import a neutral CAD file: .step/.stp, .x_t (Parasolid), .sat, .igs/.iges, .stl. target 'new_file' (default) opens it as a new ZW3D file, 'current_part' adds "
      "it to the active part. Returns the solids present afterwards.",
      schema({{"path", t_str("Full path of the file to import.")}, {"target", t_enum({"new_file", "current_part"}, "new_file")}}, {"path"}),
      [](const json& a) {
        return call("import_file", {{"path", str_req(a, "path")}, {"target", has(a, "target") ? enum_of(a, "target", {"new_file", "current_part"}) : std::string("new_file")}});
      });

  add("delete_shapes", "Delete solids (shape_ids from list_shapes). Ids are renumbered afterwards, so list_shapes again.",
      schema({{"shape_ids", t_ids()}}, {"shape_ids"}),
      [](const json& a) { return call("delete_shapes", {{"shape_ids", val::id_list(req(a, "shape_ids"), "shape_ids")}}); });

  add("delete_features", "Delete history features (feature_ids from list_features); the part regenerates and ids are renumbered.",
      schema({{"feature_ids", t_ids()}}, {"feature_ids"}),
      [](const json& a) { return call("delete_features", {{"feature_ids", val::id_list(req(a, "feature_ids"), "feature_ids")}}); });

  add("suppress_features", "Suppress (default) or unsuppress history features without deleting them; the part regenerates and ids are renumbered.",
      schema({{"feature_ids", t_ids()}, {"suppress", t_bool("true = suppress (default), false = restore")}}, {"feature_ids"}),
      [](const json& a) {
        return call("suppress_features", {{"feature_ids", val::id_list(req(a, "feature_ids"), "feature_ids")}, {"suppress", bool_or(a, "suppress", true)}});
      });

  add("measure_distance",
      "Minimum distance (mm) between two entities of the active part - solids, faces or edges, ids from list_shapes / list_faces / list_edges - with the closest point on each. "
      "Distance 0 means they touch or overlap (clearance / interference check).",
      schema({{"id1", t_int()}, {"id2", t_int()}}, {"id1", "id2"}),
      [](const json& a) {
        return call("measure_distance", {{"id1", val::one_id(req(a, "id1"), "id1")}, {"id2", val::one_id(req(a, "id2"), "id2")}});
      });

  add("run_command",
      "Escape hatch: send a raw ZW3D command string. Disabled unless ZW3D_MCP_ENABLE_RUN_COMMAND=1 is set in ZW3D's environment; behaviour is not guaranteed.",
      schema({{"command", t_str()}}, {"command"}), [](const json& a) {
        std::string c = str_req(a, "command");
        if (trim(c).empty()) bad("command must not be empty");
        return call("run_command", {{"command", c}});
      });
}

// ---------------------------------------------------------------- base64
std::string base64(const std::string& in) {
  static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8) | (unsigned char)in[i + 2];
    out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63]; out += tbl[v & 63];
  }
  if (i + 1 == in.size()) {
    unsigned v = (unsigned char)in[i] << 16;
    out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63]; out += "==";
  } else if (i + 2 == in.size()) {
    unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8);
    out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63]; out += '=';
  }
  return out;
}

std::wstring normalize_path(std::wstring p) {
  for (auto& c : p) {
    if (c == L'/') c = L'\\';
    c = (wchar_t)towlower(c);
  }
  return p;
}
}  // namespace

const std::vector<Tool>& tools() {
  static const bool built = (build(), true);
  (void)built;
  return g_tools;
}

const Tool* find_tool(const std::string& name) {
  for (const auto& t : tools())
    if (t.name == name) return &t;
  return nullptr;
}

std::string format_error(const std::string& code, const std::string& message, const json& api_code, const std::string& hint) {
  std::string s = "[" + code + "] " + message;
  if (api_code.is_number()) s += " (api_code=" + std::to_string(api_code.get<long long>()) + ")";
  if (!hint.empty()) s += " Hint: " + hint;
  return s;
}

json screenshot_content(const json& r) {
  // The path comes from our own plugin, but only ever read/delete our own screenshot files anyway.
  std::string path_utf8 = r.value("path", std::string());
  std::wstring path = utf8_to_wide(path_utf8);
  wchar_t tmp[MAX_PATH + 1];
  DWORD n = GetTempPathW(MAX_PATH, tmp);
  std::wstring dir = n ? std::wstring(tmp, n) : std::wstring();
  while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
  dir += L"\\zw3d_mcp";
  size_t slash = path.find_last_of(L"\\/");
  std::wstring pdir = slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
  std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
  bool ok = !pdir.empty() && normalize_path(pdir) == normalize_path(dir) && name.rfind(L"shot_", 0) == 0 && name.size() > 9 &&
            name.compare(name.size() - 4, 4, L".png") == 0;
  if (!ok)
    throw ApiError("INTERNAL", "Refusing to read unexpected screenshot path " + path_utf8,
                   "The plugin should write shot_*.png into the zw3d_mcp folder of the system temp directory.");
  std::string data;
  {
    std::ifstream f(path, std::ios::binary);
    if (f) data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  }
  DeleteFileW(path.c_str());     // always recycle the temp file
  RemoveDirectoryW(dir.c_str()); // only succeeds when empty
  if (data.empty()) throw ApiError("INTERNAL", "Plugin reported " + path_utf8 + " but it cannot be read", "Screenshot export may have failed.");
  return {{"type", "image"}, {"data", base64(data)}, {"mimeType", "image/png"}};
}

}  // namespace mcp
