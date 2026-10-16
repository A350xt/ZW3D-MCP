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

bool part_variable_exists(const std::string& name_utf8) {
  int n = 0;
  svxVariable* vars = nullptr;
  if (cvxPartInqVars(nullptr, nullptr, &n, &vars) != 0) return false;
  std::string a = utf8_to_ansi(name_utf8);
  bool found = false;
  for (int i = 0; i < n && !found; ++i) found = a == vars[i].Name;
  if (vars) cvxMemFree(reinterpret_cast<void**>(&vars));
  return found;
}

json h_set_variable(const json& p) {
  std::string name = p.at("name").get<std::string>();
  if (!part_variable_exists(name))
    throw ApiError("VARIABLE_NOT_FOUND", "找不到变量 " + name, "先调用 list_variables 查看可用变量名（区分大小写）。");
  svxVariable v;
  std::memset(&v, 0, sizeof(v));
  set_name(v.Name, name, "name");
  check_api(cvxPartVarGet(&v), "cvxPartVarGet");  // loads type/unit of the existing variable
  apply_value(v, p);
  DispGuard guard;
  try {
    check_api(cvxPartVarSet(1, &v, 0), "cvxPartVarSet");  // regenerates the part
  } catch (ApiError& e) {
    if (p.contains("expression"))
      e.hint = "表达式无法求值：检查是否引用了变量自身（循环引用）或不存在的变量。已回滚，模型保持原状。";
    throw;
  }
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
  json list = json::array();
  for (int i = 0; i < n; ++i) list.push_back(variable_json(vars[i]));
  if (vars) cvxMemFree(reinterpret_cast<void**>(&vars));

  // Diagnostics: the driving dimensions are entities of the sketch; cvxSkInqVars may not list them. Report them, and probe the
  // conventional names D1..Dn so that a dimension variable, if it exists under such a name, becomes editable.
  json dims = json::array();
  int dn = 0;
  int* dids = nullptr;
  if (cvxSkInqDim(&dn, &dids) == 0) {
    for (int i = 0; i < dn; ++i) {
      evxConsDimType t = VX_SK_CONSDIM_UNKNOW;
      cvxSkInqDimType(dids[i], &t);
      dims.push_back({{"dimension_id", dids[i]}, {"type", (int)t}});
    }
    if (dids) cvxMemFree(reinterpret_cast<void**>(&dids));
  }
  json probed = json::array();
  for (int i = 1; i <= dn + 3; ++i) {
    svxVariable v;
    std::memset(&v, 0, sizeof(v));
    std::string nm = "D" + std::to_string(i);
    std::memcpy(v.Name, nm.c_str(), nm.size() + 1);
    if (cvxSkVarGet(&v) == 0) {
      bool dup = false;
      for (const auto& e : list) dup = dup || e["name"] == nm;
      if (!dup) { list.push_back(variable_json(v)); probed.push_back(nm); }
    }
  }
  json out = {{"sketch_id", p["sketch_id"]}, {"count", list.size()}, {"variables", list},
              {"dimension_count", dn}, {"dimensions", dims}, {"found_by_probing", probed}};
  if (list.empty() && dn > 0)
    out["note"] = "草图有 " + std::to_string(dn) + " 个尺寸，但没有以变量形式暴露（cvxSkInqVars 为空，D1..Dn 也读不到）。"
                  "改尺寸请改用 set_variable 修改特征参数，或在中望3D界面里编辑草图。";
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

std::string dir_of(const std::string& path) {
  size_t s = path.find_last_of("\\/");
  return s == std::string::npos ? std::string() : path.substr(0, s);
}
std::string name_of(const std::string& path) {
  size_t s = path.find_last_of("\\/");
  return s == std::string::npos ? path : path.substr(s + 1);
}

json h_insert_component(const json& p) {
  std::string file = p.at("file").get<std::string>();
  svxPoint pos = p.contains("position") ? to_point(p["position"], "position") : svxPoint{0, 0, 0};
  std::string part = p.contains("part_name") && p["part_name"].is_string() ? p["part_name"].get<std::string>() : stem_of(file);
  bool anchor = p.value("anchor", false);

  // cvxCompIns wants the DIRECTORY and the FILE NAME separately ("Dir" contains "File"). The first attempt failed on a real
  // ZW3D with a full path in File, so try the documented split first, then variants, and report every error code.
  struct Variant { std::string dir, file; const char* label; };
  std::vector<Variant> variants = {
      {dir_of(file), name_of(file), "dir+file"},
      {dir_of(file), stem_of(file), "dir+stem"},
      {"", name_of(file), "name-only (search path)"},
      {"", file, "full-path-in-File"},
  };
  std::string tried;
  int last = 0;
  for (const auto& v : variants) {
    svxCompData c;
    check_api(cvxCompInsInit(&c), "cvxCompInsInit");
    if (!v.dir.empty()) to_ansi_path(v.dir, c.Dir, "directory");
    to_ansi_path(v.file, c.File, "file");
    set_name(c.Part, part, "part_name");
    c.Anchor = anchor ? 1 : 0;
    // A full frame: identity rotation plus translation (a zeroed rotation with identity = 0 is an invalid matrix).
    std::memset(&c.Frame, 0, sizeof(c.Frame));
    c.Frame.identity = (pos.x == 0 && pos.y == 0 && pos.z == 0) ? 1 : 0;
    c.Frame.xx = c.Frame.yy = c.Frame.zz = 1.0;
    c.Frame.xt = pos.x;
    c.Frame.yt = pos.y;
    c.Frame.zt = pos.z;
    int id = 0;
    int err = cvxCompIns(&c, &id);
    if (err == 0) return {{"component_id", id}, {"variant", v.label}};
    last = err;
    tried += std::string(v.label) + "(" + std::to_string(err) + ") ";
  }
  try {
    check_api(last, "cvxCompIns");
  } catch (ApiError& e) {
    e.hint = "已尝试: " + tried + "。确认当前是装配文件、零件已保存、路径存在；part_name（零件根名称）默认取文件名主干，如不同请显式传入。原提示: " + e.hint;
    throw;
  }
  return {};
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
