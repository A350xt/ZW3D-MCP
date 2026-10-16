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

void add_regen_notes(json& out, const std::vector<int>& ids_before, const std::vector<std::string>& sig_before);

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
  std::vector<int> ids_before = inq_shape_ids();
  std::vector<std::string> sig_before = shapes_signature();
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
  add_regen_notes(out, ids_before, sig_before);
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

bool same_id_set(std::vector<int> a, std::vector<int> b);

// Adds the "ids were renumbered" / "nothing changed" remarks used by every regenerating edit.
void add_regen_notes(json& out, const std::vector<int>& ids_before, const std::vector<std::string>& sig_before) {
  std::vector<int> after = out["shapes"].get<std::vector<int>>();
  std::string note;
  if (!same_id_set(ids_before, after))
    note += "再生成后实体 id 被整体重新编号：之前拿到的 id 全部失效，请只用本结果 shape_details 里的 id（面和边需重新 list_faces / list_edges）。";
  if (sig_before == shapes_signature())
    note += std::string(note.empty() ? "" : " ") + "警告：没有任何实体发生变化（体积、表面积、包围盒都不变），这个变量可能没有驱动任何特征。";
  if (!note.empty()) out["note"] = note;
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
  std::vector<int> ids_before = inq_shape_ids();
  std::vector<std::string> sig_before = shapes_signature();
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
  add_regen_notes(out, ids_before, sig_before);
  return out;
}

// ---- feature parameters ----------------------------------------------------------------------------------------
// Follows the vendor example 15.EditVariable: cvxPartInqFtrData -> cvxDataGetAll lists the fields of a history operation.
// cvxRootVarSet's header documents that a field of an operation's input data is addressed as a variable named
// "<operation_name:field_number>" (brackets included), so editing a literal feature value = setting that variable.
std::string entity_name(int id) {
  char buf[256] = {0};
  cvxEntName(id, buf, sizeof(buf));
  return ansi_to_utf8(buf);
}

std::string field_variable_name(const std::string& op_name, int field_id) {
  return "<" + op_name + ":" + std::to_string(field_id) + ">";
}

json h_list_features(const json&) {
  int n = 0;
  int* ids = nullptr;
  check_api(cvxPartInqFtrList(&n, &ids), "cvxPartInqFtrList");
  std::vector<int> v(ids, ids + n);
  if (ids) cvxMemFree(reinterpret_cast<void**>(&ids));
  json list = json::array();
  for (int id : v) {
    json item = {{"feature_id", id}, {"name", entity_name(id)}};
    vxName tpl;
    std::memset(tpl, 0, sizeof(tpl));
    if (cvxPartInqFtrTemplate(id, tpl) == 0) item["template"] = ansi_to_utf8(tpl);
    list.push_back(item);
  }
  return {{"count", v.size()}, {"feature_ids", v}, {"features", list}};
}

// Reads the field list of a feature. Returns the data container id (caller frees with cvxDataFree) and fills `fields`.
struct FeatureFields {
  int idData = -1;
  int count = 0;
  svxFldData* list = nullptr;
  ~FeatureFields() {
    if (list) cvxFldDataFree(count, &list);
    if (idData > 0) cvxDataFree(idData);
  }
};

void load_feature(int fid, FeatureFields& ff) {
  int r = cvxPartInqFtrData(fid, 0, &ff.idData);  // evaluated data first
  if (r != 0 || ff.idData < 0) {
    ff.idData = -1;
    r = cvxPartInqFtrData(fid, 1, &ff.idData);     // fall back to the last evaluated numbers (entity fields unavailable)
  }
  if (r != 0 || ff.idData < 0) throw ApiError("FEATURE_DATA_UNAVAILABLE", "该特征没有可读取的参数数据", "用 list_features 确认 feature_id；基准面/草图等特征没有参数。", r);
  check_api(cvxDataGetAll(ff.idData, &ff.count, &ff.list), "cvxDataGetAll");
}

json h_get_feature_data(const json& p) {
  int fid = p.at("feature_id").get<int>();
  std::string op = entity_name(fid);
  FeatureFields ff;
  load_feature(fid, ff);
  json fields = json::array();
  for (int i = 0; i < ff.count; ++i) {
    const svxFldData& f = ff.list[i];
    json item = {{"field_id", f.fld_id}, {"label", ansi_to_utf8(f.fld_name)}};
    const svxData* d = f.count > 0 ? f.fld_data : nullptr;
    switch (f.fld_type) {
      case VX_FLD_NUM: case VX_FLD_DST: case VX_FLD_ANG:
        item["type"] = f.fld_type == VX_FLD_NUM ? "number" : f.fld_type == VX_FLD_DST ? "distance_mm" : "angle_deg";
        if (d) {
          item["value"] = f.fld_type == VX_FLD_ANG ? d->Num * 180.0 / 3.14159265358979323846 : d->Num;  // fields hold radians
          if (d->isText && d->Text[0]) item["expression"] = ansi_to_utf8(d->Text);  // a referenced variable / expression
        }
        item["variable_name"] = field_variable_name(op, f.fld_id);  // usable with set_variable, or set_feature_value
        item["editable"] = true;
        break;
      case VX_FLD_PNT:
        item["type"] = "point";
        if (d) item["value"] = from_point(d->Pnt);
        break;
      case VX_FLD_ENT:
        item["type"] = "entity";
        if (d) item["entity_id"] = d->idEntity;
        break;
      case VX_FLD_TXT:
        item["type"] = "text";
        if (d) item["value"] = ansi_to_utf8(d->Text);
        break;
      default:
        continue;  // containers and nulls carry nothing to show
    }
    fields.push_back(item);
  }
  return {{"feature_id", fid}, {"name", op}, {"fields", fields}};
}

bool same_id_set(std::vector<int> a, std::vector<int> b) {
  std::sort(a.begin(), a.end());
  std::sort(b.begin(), b.end());
  return a == b;
}

// Current value of a numeric field (angles in degrees), or false if the field cannot be read.
bool read_field_value(int fid, int field, evxFldType* type, double* value) {
  try {
    FeatureFields ff;
    load_feature(fid, ff);
    for (int i = 0; i < ff.count; ++i) {
      if (ff.list[i].fld_id != field) continue;
      *type = ff.list[i].fld_type;
      if (ff.list[i].count > 0) {
        double n = ff.list[i].fld_data[0].Num;
        *value = *type == VX_FLD_ANG ? n * 180.0 / 3.14159265358979323846 : n;
        return true;
      }
    }
  } catch (const ApiError&) {
  }
  return false;
}

json h_set_feature_value(const json& p) {
  int fid = p.at("feature_id").get<int>();
  int field = p.at("field_id").get<int>();
  double value = p.at("value").get<double>();
  bool allow_negative = p.value("allow_negative", false);
  std::string op = entity_name(fid);
  // Find the field first, so the error is specific (unknown field / not numeric) instead of a generic API failure.
  evxFldType type = VX_FLD_NULL;
  double old_value = 0;
  bool has_old = read_field_value(fid, field, &type, &old_value);
  if (!has_old) {
    FeatureFields ff;
    load_feature(fid, ff);  // throws FEATURE_DATA_UNAVAILABLE if the feature has no data
    bool found = false;
    for (int i = 0; i < ff.count && !found; ++i)
      if (ff.list[i].fld_id == field) { found = true; type = ff.list[i].fld_type; }
    if (!found)
      throw ApiError("FIELD_NOT_FOUND", "特征 " + op + " 没有字段 " + std::to_string(field), "先调用 get_feature_data 查看 field_id。");
  }
  if (type != VX_FLD_NUM && type != VX_FLD_DST && type != VX_FLD_ANG)
    throw ApiError("FIELD_NOT_NUMERIC", "字段 " + std::to_string(field) + " 不是数值字段", "只能修改数字、距离、角度字段。");
  if (type == VX_FLD_DST && value < 0 && !allow_negative)
    throw ApiError("NEGATIVE_DISTANCE", "距离字段不接受负值 " + std::to_string(value) + "（中望3D不会报错，但会让特征反向/翻转）",
                   "要反向拉伸请用 sketch_extrude 的 reverse，或在 extrude_* 里给 direction。确实需要负距离时传 allow_negative=true。");

  svxVariable v;
  std::memset(&v, 0, sizeof(v));
  set_name(v.Name, field_variable_name(op, field), "variable name");
  v.type = VX_VAR_NUM;
  v.sub_type = type == VX_FLD_DST ? VX_VAR_DST : type == VX_FLD_ANG ? VX_VAR_ANG : VX_VAR_CONS;
  v.unit_type = type == VX_FLD_DST ? VX_UNIT_MILLIMETER : type == VX_FLD_ANG ? VX_UNIT_DEG : 0;
  v.Value = value;  // database units: mm / degrees
  std::vector<int> ids_before = inq_shape_ids();
  std::vector<std::string> sig_before = shapes_signature();
  DispGuard guard;
  try {
    check_api(cvxPartVarSet(1, &v, 0), "cvxPartVarSet");  // regenerates the part
  } catch (ApiError& e) {
    e.hint = "按 " + field_variable_name(op, field) + " 设置特征字段失败：值可能使几何无效（已回滚，模型保持原状），或该字段不能这样修改。原提示: " + e.hint;
    throw;
  }

  // The call "succeeds" even when the field is derived (e.g. a box made from two corner points) and nothing really changes.
  // Verify instead of trusting it; throwing here makes the transaction wrapper roll the attempt back.
  double now = 0;
  evxFldType t2 = type;
  if (read_field_value(fid, field, &t2, &now) && std::fabs(now - value) > 1e-6 * std::max(1.0, std::fabs(value)))
    throw ApiError("FIELD_NOT_APPLIED", "字段 " + std::to_string(field) + " 没有被改成 " + std::to_string(value) + "（读回的值是 " + std::to_string(now) + "）",
                   "这个字段很可能是派生值（例如按两个角点创建的长方体，长度由角点决定）。已回滚。改用该特征的其他字段，或重新创建。");
  if (sig_before == shapes_signature() && std::fabs(old_value - value) > 1e-9)
    throw ApiError("NO_EFFECT", "把字段 " + std::to_string(field) + " 改成 " + std::to_string(value) + " 没有改变任何实体（体积、表面积、包围盒都不变）",
                   "该字段没有驱动几何，可能是派生字段（例如按两个角点建的长方体的“长度”）。已回滚。用 get_feature_data 找真正驱动几何的字段，或删除后用 create_* 重新创建。");

  json out = {{"feature_id", fid}, {"field_id", field}, {"value", value}, {"shapes", inq_shape_ids()}};
  json details = json::array();
  for (int s : out["shapes"].get<std::vector<int>>()) details.push_back(describe_shape(s));
  out["shape_details"] = details;
  if (!same_id_set(ids_before, out["shapes"].get<std::vector<int>>())) {
    out["ids_renumbered"] = true;
    out["note"] = "再生成后实体 id 被整体重新编号：之前拿到的 shape_id / face_id / edge_id 全部失效，请只使用本结果 shape_details 里的 id（面和边需重新 list_faces / list_edges）。";
  }
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
  m["list_features"] = h_list_features;  // replaces the plain id list registered by part.cpp
  m["get_feature_data"] = h_get_feature_data;
  m["set_feature_value"] = h_set_feature_value;
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
