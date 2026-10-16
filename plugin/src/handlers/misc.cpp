// Variables (part and sketch), assemblies and drawings.
#include <algorithm>
#include <cstring>
#include "handlers.h"
#include "../encoding.h"
#include "../errors.h"

namespace {

template <size_t N>
void set_name(char (&dst)[N], const std::string& utf8, const char* what) {
  std::string a = utf8_to_ansi(utf8);
  if (a.empty() || a.size() >= N) invalid_input(std::string(what) + " 为空或过长（上限 " + std::to_string(N - 1) + " 字节）");
  std::memcpy(dst, a.c_str(), a.size() + 1);
}

void to_ansi_path(const std::string& utf8, char (&out)[512], const char* what) {
  std::string a = utf8_to_ansi(utf8);
  if (a.empty()) invalid_input(std::string(what) + " 不能为空");
  if (a.size() >= sizeof(out)) throw ApiError("PATH_TOO_LONG", std::string(what) + " 过长（上限 511 字节）", "缩短路径。", ZW_API_PATH_LEN_OVER_RANGE);
  std::memcpy(out, a.c_str(), a.size() + 1);
}

json variable_json(const svxVariable& v) {
  const char* kind = v.type == VX_VAR_NUM ? (v.sub_type == VX_VAR_DST ? "distance" : v.sub_type == VX_VAR_ANG ? "angle" : "number")
                   : v.type == VX_VAR_STR ? "string" : v.type == VX_VAR_PNT ? "point" : "vector";
  json j = {{"name", ansi_to_utf8(v.Name)}, {"kind", kind}};
  if (v.type == VX_VAR_NUM) j["value"] = v.Value;  // mm / degrees (database units)
  if (v.type == VX_VAR_PNT || v.type == VX_VAR_VEC) j["value"] = {v.pnt_val.x, v.pnt_val.y, v.pnt_val.z};
  if (v.Expression[0]) j["expression"] = ansi_to_utf8(v.Expression);
  if (v.description[0]) j["description"] = ansi_to_utf8(v.description);
  return j;
}

json variables_json(int n, svxVariable* vars) {
  json list = json::array();
  for (int i = 0; i < n; ++i) list.push_back(variable_json(vars[i]));
  return {{"count", n}, {"variables", list}};
}

json h_list_variables(const json&) {
  int n = 0;
  svxVariable* vars = nullptr;
  check_api(cvxPartInqVars(nullptr, nullptr, &n, &vars), "cvxPartInqVars");
  json out = variables_json(n, vars);
  if (vars) cvxMemFree(reinterpret_cast<void**>(&vars));
  return out;
}

// Load the existing variable (keeps its type/unit), then overwrite value or expression.
void apply_value(svxVariable& v, const json& p) {
  if (p.contains("expression") && p["expression"].is_string()) {
    std::string e = utf8_to_ansi(p["expression"].get<std::string>());
    if (e.size() >= sizeof(v.Expression)) invalid_input("expression 过长");
    std::memcpy(v.Expression, e.c_str(), e.size() + 1);
  } else if (p.contains("value") && p["value"].is_number()) {
    v.Value = p["value"].get<double>();
    v.Expression[0] = 0;  // a plain value replaces any previous expression
  } else {
    invalid_input("需要 value（数字）或 expression（字符串）");
  }
}

json h_set_variable(const json& p) {
  svxVariable v;
  std::memset(&v, 0, sizeof(v));
  set_name(v.Name, p.at("name").get<std::string>(), "name");
  if (cvxPartVarGet(&v) != 0)
    throw ApiError("VARIABLE_NOT_FOUND", "找不到变量 " + p["name"].get<std::string>(), "先调用 list_variables 查看可用变量名。");
  apply_value(v, p);
  DispGuard guard;
  check_api(cvxPartVarSet(1, &v, 0), "cvxPartVarSet");  // regenerates the part
  json out = {{"variable", variable_json(v)}, {"shapes", inq_shape_ids()}};
  json details = json::array();
  for (int s : out["shapes"].get<std::vector<int>>()) details.push_back(describe_shape(s));
  out["shape_details"] = details;
  return out;
}

json h_add_variable(const json& p) {
  svxVariable v;
  std::memset(&v, 0, sizeof(v));
  set_name(v.Name, p.at("name").get<std::string>(), "name");
  std::string kind = p.value("kind", std::string("number"));
  v.type = VX_VAR_NUM;
  if (kind == "distance") { v.sub_type = VX_VAR_DST; v.unit_type = VX_UNIT_MILLIMETER; }
  else if (kind == "angle") { v.sub_type = VX_VAR_ANG; v.unit_type = VX_UNIT_DEG; }
  else if (kind == "number") { v.sub_type = VX_VAR_CONS; }
  else invalid_input("kind 取值无效: " + kind, "可选: number, distance, angle");
  apply_value(v, p);
  check_api(cvxPartVarAdd(&v), "cvxPartVarAdd");
  return {{"variable", variable_json(v)}};
}

// Sketch variables (the driving dimensions of a sketch) live inside the sketch, so enter it, work, and always leave.
struct SketchSession {
  bool active = false;
  explicit SketchSession(int id) {
    check_api(cvxSkActivate(id, 0), "cvxSkActivate");
    active = true;
  }
  void leave() { if (active) { active = false; cvxRootExit(); } }
  ~SketchSession() { leave(); }
};

json h_list_sketch_variables(const json& p) {
  SketchSession s(p.at("sketch_id").get<int>());
  int n = 0;
  svxVariable* vars = nullptr;
  check_api(cvxSkInqVars(&n, &vars), "cvxSkInqVars");
  json out = variables_json(n, vars);
  if (vars) cvxMemFree(reinterpret_cast<void**>(&vars));
  out["sketch_id"] = p["sketch_id"];
  return out;
}

json h_set_sketch_variable(const json& p) {
  int sk = p.at("sketch_id").get<int>();
  json var;
  {
    SketchSession s(sk);
    svxVariable v;
    std::memset(&v, 0, sizeof(v));
    set_name(v.Name, p.at("name").get<std::string>(), "name");
    if (cvxSkVarGet(&v) != 0)
      throw ApiError("VARIABLE_NOT_FOUND", "草图里找不到变量 " + p["name"].get<std::string>(), "先调用 list_sketch_variables 查看变量名。");
    apply_value(v, p);
    check_api(cvxSkVarSet(1, &v), "cvxSkVarSet");
    var = variable_json(v);
    s.leave();  // leaving the sketch triggers the part regeneration
  }
  json out = {{"variable", var}, {"shapes", inq_shape_ids()}};
  json details = json::array();
  for (int s2 : out["shapes"].get<std::vector<int>>()) details.push_back(describe_shape(s2));
  out["shape_details"] = details;
  return out;
}

// ---- assemblies -------------------------------------------------------------------------------------------------
json h_new_assembly(const json& p) {
  char file[512];
  to_ansi_path(p.at("name").get<std::string>(), file, "name");
  check_api(cvxFileNewSingle(file, VX_FILE_ASSEMBLY, VX_NORMAL_ASSEMBLY, nullptr, nullptr), "cvxFileNewSingle");
  return {{"file", current_file_utf8()}, {"root", current_root_utf8()}};
}

std::string stem_of(const std::string& path) {
  size_t s = path.find_last_of("\\/");
  std::string n = s == std::string::npos ? path : path.substr(s + 1);
  size_t d = n.find_last_of('.');
  return d == std::string::npos || d == 0 ? n : n.substr(0, d);
}

json h_insert_component(const json& p) {
  std::string file = p.at("file").get<std::string>();
  svxPoint pos = p.contains("position") ? to_point(p["position"], "position") : svxPoint{0, 0, 0};
  svxCompData c;
  check_api(cvxCompInsInit(&c), "cvxCompInsInit");
  to_ansi_path(file, c.File, "file");
  set_name(c.Part, p.contains("part_name") && p["part_name"].is_string() ? p["part_name"].get<std::string>() : stem_of(file), "part_name");
  c.Anchor = p.value("anchor", false) ? 1 : 0;
  c.Frame.xt = pos.x;
  c.Frame.yt = pos.y;
  c.Frame.zt = pos.z;
  int id = 0;
  check_api(cvxCompIns(&c, &id), "cvxCompIns");
  return {{"component_id", id}};
}

json h_list_components(const json&) {
  int n = 0;
  int* ids = nullptr;
  check_api(cvxPartInqComps(nullptr, nullptr, &n, &ids), "cvxPartInqComps");
  std::vector<int> v(ids, ids + n);
  if (ids) cvxMemFree(reinterpret_cast<void**>(&ids));
  return {{"count", v.size()}, {"component_ids", v}};
}

// ---- drawings ---------------------------------------------------------------------------------------------------
json h_new_drawing(const json& p) {
  char file[512];
  to_ansi_path(p.at("name").get<std::string>(), file, "name");
  std::string part = p.at("part_file").get<std::string>();  // must be a saved part/assembly: use its full path
  check_api(cvxFileNewSingle(file, VX_FILE_SHEET, VX_SUBTYPE_NONE, nullptr, nullptr), "cvxFileNewSingle");
  svxViewLayoutData d;
  check_api(cvxDwgViewLayoutInit(&d), "cvxDwgViewLayoutInit");
  set_name(d.name, part, "part_file");
  d.isIgnoreLoc = 1;     // default location, method and attributes: the standard multi-view layout
  d.isIgnoreMethod = 1;
  d.isIgnoreAttr = 1;
  check_api(cvxDwgViewLayout(&d), "cvxDwgViewLayout");
  return {{"file", current_file_utf8()}, {"root", current_root_utf8()}};
}

}  // namespace

void register_misc_handlers(std::map<std::string, Handler>& m) {
  m["list_variables"] = h_list_variables;
  m["set_variable"] = h_set_variable;
  m["add_variable"] = h_add_variable;
  m["list_sketch_variables"] = h_list_sketch_variables;
  m["set_sketch_variable"] = h_set_sketch_variable;
  m["new_assembly"] = h_new_assembly;
  m["insert_component"] = h_insert_component;
  m["list_components"] = h_list_components;
  m["new_drawing"] = h_new_drawing;
}
