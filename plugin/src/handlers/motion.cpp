// Kinematics simulation (ZW3D motion module): read the motion setup, edit drivers / solutions, solve, read the results.
// The API cannot create bodies, joints or solutions (only drivers): those are set up in ZW3D's motion environment by the user.
#include <algorithm>
#include <cmath>
#include <cstring>
#include "handlers.h"
#include "zwapi_motion.h"
#include "../encoding.h"
#include "../errors.h"

namespace {

void motion_check(int rc, const char* fn) {
  try {
    check_api(rc, fn);
  } catch (ApiError& e) {
    e.hint = "运动仿真接口需要在中望3D里先进入装配的“运动仿真”环境（并已建立刚体/运动副/驱动/方案）。原提示: " + e.hint;
    throw;
  }
}

std::vector<int> motion_ids(ezwMotType type) {
  int n = 0;
  int* ids = nullptr;
  motion_check(ZwMotEntityListGet(type, &n, &ids), "ZwMotEntityListGet");
  std::vector<int> v;
  if (ids) {
    v.assign(ids, ids + n);
    cvxMemFree(reinterpret_cast<void**>(&ids));
  }
  return v;
}

std::string motion_name(int id) {
  char buf[256] = {0};
  if (ZwMotEntityNameGet(id, sizeof(buf), buf) != 0) return std::string();
  return ansi_to_utf8(buf);
}

bool motion_active(int id) {
  int a = 0;
  return ZwMotEntityActiveGet(id, &a) == 0 && a != 0;
}

const char* joint_name(ezwMotJointSubClass s) {
  static const char* n[] = {"revolute", "slider", "cylindrical", "screw", "spherical", "planar", "hooke", "rigid",
                            "point_curve", "curve_curve", "point_surface", "gear", "rack", "cable", "universal", "general"};
  return (int)s >= 0 && (int)s < 16 ? n[(int)s] : "unknown";
}

const char* driver_type_name(ezwMotDriverType t) {
  static const char* n[] = {"displacement", "velocity", "acceleration", "angle", "angular_velocity", "angular_acceleration"};
  return (int)t >= 0 && (int)t < 6 ? n[(int)t] : "unknown";
}

const char* function_name(ezwMotDriverFunctionType t) {
  static const char* n[] = {"constant", "linear", "cosine", "cycloidal", "parabolic", "polynomial", "user", "fitted"};
  return (int)t >= 0 && (int)t < 8 ? n[(int)t] : "unknown";
}

template <class T>
json vec_json(const T& v) { return {v.x, v.y, v.z}; }

json driver_json(int id) {
  szwMotDriver d{};
  motion_check(ZwMotDriverGet(id, &d), "ZwMotDriverGet");
  return {{"driver_id", id}, {"name", motion_name(id)}, {"active", motion_active(id)}, {"joint_id", d.idJoint}, {"type", driver_type_name(d.eType)},
          {"function", function_name(d.eFunctionType)}, {"expression", ansi_to_utf8(d.sFunctionExpression)},
          {"parameters_ABCDLT", {d.dParameter[0], d.dParameter[1], d.dParameter[2], d.dParameter[3], d.dParameter[4], d.dParameter[5]}},
          {"initial_position", d.dInitPosition}, {"initial_velocity", d.dInitVelocity}};
}

json solution_json(int id) {
  szwMotSolution s{};
  motion_check(ZwMotSolutionGet(id, &s), "ZwMotSolutionGet");
  int dof = -1;
  ZwMotSolutionDegreeOfFreedomGet(id, &dof);
  json times = json::array();
  int tn = 0;
  szwMotDriverTime* tl = nullptr;
  if (ZwMotSolutionDriverTimeListGet(id, &tn, &tl) == 0 && tl) {
    for (int i = 0; i < tn; ++i)
      times.push_back({{"driver_id", tl[i].idDriver}, {"start_time", tl[i].dStartTime}, {"end_time", tl[i].dEndTime},
                       {"start_default", tl[i].iStartMode == ZW_MOT_DRIVER_TIME_DEFAULT}, {"end_default", tl[i].iEndMode == ZW_MOT_DRIVER_TIME_DEFAULT}});
    cvxMemFree(reinterpret_cast<void**>(&tl));
  }
  return {{"solution_id", id}, {"name", motion_name(id)}, {"active", motion_active(id)},
          {"type", s.eType == ZW_MOT_SOLUTION_KINEMATICS ? "kinematics" : "location"},
          {"total_frames", s.iTotalFrames}, {"frame_frequency", s.iFrameFrequency}, {"end_time", s.dEndTime},
          {"degrees_of_freedom", dof}, {"driver_times", times}};
}

json h_motion_info(const json&) {
  json bodies = json::array(), joints = json::array(), drivers = json::array(), solutions = json::array();
  for (int id : motion_ids(ZW_MOT_BODY)) {
    szwMotBody b{};
    motion_check(ZwMotBodyGet(id, &b), "ZwMotBodyGet");
    bodies.push_back({{"body_id", id}, {"name", motion_name(id)}, {"fixed", b.iFix != 0}, {"center", vec_json(b.vecCenter)}});
  }
  for (int id : motion_ids(ZW_MOT_JOINT)) {
    szwMotJoint j{};
    motion_check(ZwMotJointGet(id, &j), "ZwMotJointGet");
    json o = {{"joint_id", id}, {"name", motion_name(id)}, {"active", motion_active(id)}, {"kind", joint_name(j.eSubClass)}};
    if (j.eType == ZW_MOT_JOINT_TYPE_BASIC) {
      o["active_body"] = j.data.zBasic.idActiveBody;
      o["base_body"] = j.data.zBasic.idBasicBody;
      if (j.eSubClass == ZW_MOT_JOINT_REVOLUTE || j.eSubClass == ZW_MOT_JOINT_SLIDER) {
        o["upper_limit"] = j.data.zBasic.dUpperLimit;
        o["lower_limit"] = j.data.zBasic.dLowerLimit;
      }
      if (j.eSubClass == ZW_MOT_JOINT_SCREW) o["pitch"] = j.data.zBasic.dScrewPitch;
    } else if (j.eType == ZW_MOT_JOINT_TYPE_CONSTRAINT) {
      o["body1"] = j.data.zConstraint.idBody1;
      o["body2"] = j.data.zConstraint.idBody2;
    } else {
      o["joints"] = {j.data.zCoupler.idJoint1, j.data.zCoupler.idJoint2, j.data.zCoupler.idJoint3};
    }
    ZwMotJointFree(&j);
    joints.push_back(o);
  }
  for (int id : motion_ids(ZW_MOT_DRIVER)) drivers.push_back(driver_json(id));
  for (int id : motion_ids(ZW_MOT_SOLUTION)) solutions.push_back(solution_json(id));
  int active = -1;
  ZwMotActiveSolutionGet(&active);
  json out = {{"bodies", bodies}, {"joints", joints}, {"drivers", drivers}, {"solutions", solutions}, {"active_solution_id", active}};
  if (bodies.empty() && joints.empty())
    out["note"] = "没有任何刚体/运动副：在中望3D的运动仿真环境里先建立刚体和运动副（API 不能创建它们）。";
  return out;
}

int resolve_solution(const json& p) {
  if (p.contains("solution_id") && !p["solution_id"].is_null()) return p["solution_id"].get<int>();
  int id = -1;
  motion_check(ZwMotActiveSolutionGet(&id), "ZwMotActiveSolutionGet");
  if (id < 0) throw ApiError("NO_SOLUTION", "没有激活的运动方案", "用 motion_info 查看 solutions，传入 solution_id，或在中望3D里先建立方案。");
  return id;
}

ezwMotDriverType parse_driver_type(const std::string& s) {
  static const char* n[] = {"displacement", "velocity", "acceleration", "angle", "angular_velocity", "angular_acceleration"};
  for (int i = 0; i < 6; ++i)
    if (s == n[i]) return (ezwMotDriverType)i;
  invalid_input("type 取值无效: " + s, "可选: displacement, velocity, acceleration, angle, angular_velocity, angular_acceleration");
}

ezwMotDriverFunctionType parse_function(const std::string& s) {
  static const char* n[] = {"constant", "linear", "cosine", "cycloidal", "parabolic", "polynomial", "user"};
  for (int i = 0; i < 7; ++i)
    if (s == n[i]) return (ezwMotDriverFunctionType)i;
  invalid_input("function 取值无效: " + s, "可选: constant, linear, cosine, cycloidal, parabolic, polynomial, user（user 需要 expression）");
}

json h_set_motion_driver(const json& p) {
  bool create = !(p.contains("driver_id") && !p["driver_id"].is_null());
  szwMotDriver d{};
  int id = 0;
  if (create) {
    if (!p.contains("joint_id") || p["joint_id"].is_null()) invalid_input("新建驱动需要 joint_id", "motion_info 列出运动副；不给 driver_id 即新建。");
    d.idJoint = p["joint_id"].get<int>();
    d.eType = parse_driver_type(p.value("type", std::string("angle")));
    d.eFunctionType = parse_function(p.value("function", std::string("linear")));
    ezwMotDriverType t = d.eType;
    int rc = ZwMotDriverValidJointCheck(d.idJoint, &t);
    if (rc != 0) throw ApiError("INVALID_DRIVER", "该运动副不支持这种驱动类型", "转动副用 angle / angular_velocity，滑动副用 displacement / velocity。", rc);
  } else {
    id = p["driver_id"].get<int>();
    motion_check(ZwMotDriverGet(id, &d), "ZwMotDriverGet");
    if (p.contains("type")) d.eType = parse_driver_type(p["type"].get<std::string>());
    if (p.contains("function")) d.eFunctionType = parse_function(p["function"].get<std::string>());
  }
  if (p.contains("parameters") && p["parameters"].is_array()) {
    const json& a = p["parameters"];
    if (a.size() > 6) invalid_input("parameters 最多 6 个 (A,B,C,D,L,T)");
    for (size_t i = 0; i < a.size(); ++i) d.dParameter[i] = a[i].get<double>();
  }
  if (p.contains("expression") && p["expression"].is_string()) {
    std::string e = utf8_to_ansi(p["expression"].get<std::string>());
    if (e.size() >= sizeof(d.sFunctionExpression)) invalid_input("expression 过长");
    std::memcpy(d.sFunctionExpression, e.c_str(), e.size() + 1);
  }
  if (p.contains("initial_position")) d.dInitPosition = p["initial_position"].get<double>();
  if (p.contains("initial_velocity")) d.dInitVelocity = p["initial_velocity"].get<double>();
  if (d.eFunctionType == ZW_MOT_DRIVER_FUNCTION_USER && d.sFunctionExpression[0] == 0) invalid_input("function=user 需要 expression");

  if (create) {
    d.idEntity = 0;
    motion_check(ZwMotDriverCreate(d, &id), "ZwMotDriverCreate");
  } else {
    d.idEntity = id;
    motion_check(ZwMotDriverSet(id, d), "ZwMotDriverSet");
  }
  if (p.contains("active")) motion_check(ZwMotEntityActiveSet(id, p["active"].get<bool>() ? 1 : 0), "ZwMotEntityActiveSet");

  // optional start / end time of this driver inside a solution
  if (p.contains("start_time") || p.contains("end_time")) {
    int sol = resolve_solution(p);
    int n = 0;
    szwMotDriverTime* list = nullptr;
    motion_check(ZwMotSolutionDriverTimeListGet(sol, &n, &list), "ZwMotSolutionDriverTimeListGet");
    std::vector<szwMotDriverTime> v;
    if (list) {
      v.assign(list, list + n);
      cvxMemFree(reinterpret_cast<void**>(&list));
    }
    auto it = std::find_if(v.begin(), v.end(), [&](const szwMotDriverTime& t) { return t.idDriver == id; });
    if (it == v.end()) {
      szwMotDriverTime t{};
      t.idDriver = id;
      t.iStartMode = ZW_MOT_DRIVER_TIME_DEFAULT;
      t.iEndMode = ZW_MOT_DRIVER_TIME_DEFAULT;
      v.push_back(t);
      it = v.end() - 1;
    }
    if (p.contains("start_time")) { it->iStartMode = ZW_MOT_DRIVER_TIME_USER; it->dStartTime = p["start_time"].get<double>(); }
    if (p.contains("end_time")) { it->iEndMode = ZW_MOT_DRIVER_TIME_USER; it->dEndTime = p["end_time"].get<double>(); }
    motion_check(ZwMotSolutionDriverTimeListSet(sol, (int)v.size(), v.data()), "ZwMotSolutionDriverTimeListSet");
  }
  json out = driver_json(id);
  out["created"] = create;
  return out;
}

json h_set_motion_solution(const json& p) {
  int id = resolve_solution(p);
  szwMotSolution s{};
  motion_check(ZwMotSolutionGet(id, &s), "ZwMotSolutionGet");
  if (p.contains("type")) s.eType = p["type"].get<std::string>() == "location" ? ZW_MOT_SOLUTION_LOCATION : ZW_MOT_SOLUTION_KINEMATICS;
  bool has_t = p.contains("end_time"), has_n = p.contains("total_frames"), has_f = p.contains("frame_frequency");
  if (has_t) s.dEndTime = p["end_time"].get<double>();
  if (has_n) s.iTotalFrames = p["total_frames"].get<int>();
  if (has_f) s.iFrameFrequency = p["frame_frequency"].get<int>();
  // which two of (time, frequency, frames) the user fixed decides the setting type
  if (has_t && has_f) s.eSettingType = ZW_MOT_SOLUTION_SETTING_TIME_FREQUENCY;
  else if (has_t && has_n) s.eSettingType = ZW_MOT_SOLUTION_SETTING_TIME_FRAMES;
  else if (has_f && has_n) s.eSettingType = ZW_MOT_SOLUTION_SETTING_FREQUENCY_FRAMES;
  motion_check(ZwMotSolutionSet(id, s), "ZwMotSolutionSet");
  if (p.value("activate", false)) motion_check(ZwMotEntityActiveSet(id, 1), "ZwMotEntityActiveSet");
  return solution_json(id);
}

json h_run_motion(const json& p) {
  int sol = resolve_solution(p);
  if (!motion_active(sol)) motion_check(ZwMotEntityActiveSet(sol, 1), "ZwMotEntityActiveSet");  // Calculate solves the active solution
  ezwMotSolutionSolveStatus st{};
  motion_check(ZwMotSolutionCalculate(&st), "ZwMotSolutionCalculate");
  const char* status = st == ZW_MOT_SOLUTION_SOLVED ? "solved" : st == ZW_MOT_SOLUTION_SOLVE_PARTIAL ? "partial"
                     : st == ZW_MOT_SOLUTION_SOLVE_FAILED ? "failed" : "failed_preprocess";
  if (st == ZW_MOT_SOLUTION_SOLVE_FAILED || st == ZW_MOT_SOLUTION_SOLVE_FAILED_PRE_PROCESS)
    throw ApiError("MOTION_SOLVE_FAILED", std::string("运动方案求解失败 (") + status + ")",
                   "检查自由度是否为 0（motion_info 的 degrees_of_freedom；需要足够的驱动）、运动副是否装配到位、驱动类型是否匹配运动副、时间范围是否合理。");

  int n = 0;
  szwMotFrameResult* frames = nullptr;
  motion_check(ZwMotSolutionSolveResultGet(sol, &n, &frames), "ZwMotSolutionSolveResultGet");
  json out = {{"solution_id", sol}, {"status", status}, {"frames", n}};
  if (n > 0 && frames) {
    std::vector<int> filter;
    if (p.contains("body_ids") && p["body_ids"].is_array()) filter = p["body_ids"].get<std::vector<int>>();
    auto wanted = [&](int body) { return filter.empty() || std::find(filter.begin(), filter.end(), body) != filter.end(); };
    int maxf = std::max(2, std::min(p.value("max_frames", 12), 200));
    int step_count = std::min(n, maxf);
    out["start_time"] = frames[0].dTime;
    out["end_time"] = frames[n - 1].dTime;
    json sampled = json::array();
    for (int k = 0; k < step_count; ++k) {
      int idx = step_count == 1 ? 0 : (int)std::lround((double)k * (n - 1) / (step_count - 1));
      const szwMotFrameResult& f = frames[idx];
      json bodies = json::array();
      for (int b = 0; b < f.countBody; ++b) {
        const szwMotEntityResult& r = f.zBodyResult[b];
        if (!wanted(r.idEntity)) continue;
        bodies.push_back({{"body_id", r.idEntity}, {"position", vec_json(r.zPosition)}, {"euler_angle", vec_json(r.zEulerAngle)},
                          {"velocity", vec_json(r.zVelocity)}, {"angle_velocity", vec_json(r.zAngleVelocity)}});
      }
      sampled.push_back({{"frame", idx}, {"time", f.dTime}, {"bodies", bodies}});
    }
    out["sampled_frames"] = sampled;
    // overall travel of every body over all frames: min / max of the position
    json ranges = json::array();
    for (int b = 0; b < frames[0].countBody; ++b) {
      int bid = frames[0].zBodyResult[b].idEntity;
      if (!wanted(bid)) continue;
      double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
      for (int i = 0; i < n; ++i)
        for (int k = 0; k < frames[i].countBody; ++k)
          if (frames[i].zBodyResult[k].idEntity == bid) {
            const szwVector& v = frames[i].zBodyResult[k].zPosition;
            double c[3] = {v.x, v.y, v.z};
            for (int a = 0; a < 3; ++a) { lo[a] = std::min(lo[a], c[a]); hi[a] = std::max(hi[a], c[a]); }
          }
      ranges.push_back({{"body_id", bid}, {"position_min", {lo[0], lo[1], lo[2]}}, {"position_max", {hi[0], hi[1], hi[2]}}});
    }
    out["body_position_range"] = ranges;
    out["note"] = "position 单位 mm；euler_angle 的单位以中望3D返回为准（未在头文件中说明，可能为弧度）。要看某一帧的姿态可用 sampled_frames 的位置/角度。";
  }
  ZwMotSolutionSolveResultFree(n, &frames);
  return out;
}

}  // namespace

void register_motion_handlers(std::map<std::string, Handler>& m) {
  m["motion_info"] = h_motion_info;
  m["set_motion_driver"] = h_set_motion_driver;
  m["set_motion_solution"] = h_set_motion_solution;
  m["run_motion"] = h_run_motion;
}
