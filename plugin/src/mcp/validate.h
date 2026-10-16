#pragma once
// Parameter validation for the MCP tools (port of the former Python validation layer).
// Everything throws ApiError("INVALID_INPUT", message, hint) so the AI gets an actionable message before ZW3D is touched.
#include <string>
#include <vector>
#include "json.hpp"
#include "../errors.h"

namespace val {
using json = nlohmann::json;

struct V3 {
  double x = 0, y = 0, z = 0;
};

[[noreturn]] void bad(const std::string& message, const std::string& hint = "");

json to_json(const V3& v);
V3 point(const json& v, const std::string& name);     // [x, y, z], finite numbers
V3 vector(const json& v, const std::string& name);    // like point, but not the zero vector
double number(const json& v, const std::string& name);
double positive(const json& v, const std::string& name);
std::vector<int> id_list(const json& ids, const std::string& name, int minimum = 1);
int one_id(const json& id, const std::string& name);

// Closed planar polygon: >= 3 vertices, planar, non-degenerate. Returns the vertices (an explicitly repeated first point is dropped).
std::vector<V3> polygon(const json& points, const std::string& name = "points");
V3 polygon_normal(const std::vector<V3>& pts);

// Profiles made of line / arc / circle segments (3D, or 2D [x, y] for sketches). Return normalised copies.
json segments(const json& segs, const std::string& name = "segments");
json segments2d(const json& segs, const std::string& name = "segments");
json inner_loops2d(const json& loops, const std::string& name = "holes");
json path_segments(const json& segs, const std::string& name = "path");  // connected OPEN chain of lines/arcs

struct Frame {
  V3 origin, x_axis, y_axis;
};
Frame sketch_plane(const json& plane);   // 'XY' | 'YZ' | 'ZX' | {"name",origin?} | {"origin","x_axis","y_axis"}
json frame_json(const Frame& f);
V3 plane_normal(const Frame& f);
json mirror_plane(const json& plane);    // -> {"origin", "normal"}

std::vector<V3> profile_points(const json& segs);   // end/through points of line/arc segments (circles ignored)
std::vector<V3> profile_points2d(const json& segs);
// Rejects a revolve whose profile has points on both sides of the axis.
void check_axis_not_crossed_3d(const std::vector<V3>& pts, const V3& axis_point, const V3& axis_dir);
void check_axis_not_crossed_2d(const std::vector<V3>& pts, double apx, double apy, double adx, double ady);

// Rejects empty expressions and expressions that mention the variable being defined (circular reference).
std::string check_expression(const std::string& name, const json& expression);
}  // namespace val
