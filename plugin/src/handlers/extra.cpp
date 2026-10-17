// Cleanup and measuring tools: delete, suppress, distance.
#include <algorithm>
#include "handlers.h"
#include "../errors.h"

namespace {

std::vector<int> id_array(const json& v, const char* name) {
  if (!v.is_array() || v.empty()) invalid_input(std::string(name) + " 不能为空");
  std::vector<int> out;
  for (const auto& x : v) {
    if (!x.is_number_integer()) invalid_input(std::string(name) + " 必须是整数 id 数组");
    out.push_back(x.get<int>());
  }
  return out;
}

json h_delete_shapes(const json& p) {
  std::vector<int> ids = id_array(p.at("shape_ids"), "shape_ids");
  for (int id : ids) require_shape(id);
  DispGuard guard;
  check_api(cvxPartErase((int)ids.size(), ids.data()), "cvxPartErase");
  return {{"deleted_shape_ids", ids}, {"remaining", inq_shape_ids()},
          {"note", "删除后实体/面/边 id 可能变化，请重新 list_shapes。"}};
}

json h_delete_features(const json& p) {
  std::vector<int> ids = id_array(p.at("feature_ids"), "feature_ids");
  DispGuard guard;
  check_api(cvxPartHistDel((int)ids.size(), ids.data()), "cvxPartHistDel");
  return {{"deleted_feature_ids", ids}, {"shapes", inq_shape_ids()},
          {"note", "删除特征会重新生成零件，实体/面/边 id 已重新编号；请重新 list_shapes / list_features。"}};
}

json h_suppress_features(const json& p) {
  std::vector<int> ids = id_array(p.at("feature_ids"), "feature_ids");
  bool suppress = p.value("suppress", true);
  DispGuard guard;
  check_api(cvxPartFtrSuppress((int)ids.size(), ids.data(), suppress ? 1 : 0), "cvxPartFtrSuppress");
  return {{"feature_ids", ids}, {"suppressed", suppress}, {"shapes", inq_shape_ids()},
          {"note", "压缩/解压会重新生成零件，实体/面/边 id 已重新编号；请重新 list_shapes。"}};
}

json h_measure_distance(const json& p) {
  int a = p.at("id1").get<int>(), b = p.at("id2").get<int>();
  svxPoint p1{}, p2{};
  double d = 0;
  check_api(cvxEntGetDistance(a, b, &p1, &p2, &d), "cvxEntGetDistance");
  json out = {{"id1", a}, {"id2", b}, {"distance", d}, {"point1", from_point(p1)}, {"point2", from_point(p2)}};
  if (d < 1e-9) out["note"] = "距离为 0：两者接触或重叠（实体之间可能干涉，可用 boolean intersect 且 keep_tools=true 再看体积确认）。";
  return out;
}

}  // namespace

void register_extra_handlers(std::map<std::string, Handler>& m) {
  m["delete_shapes"] = h_delete_shapes;
  m["delete_features"] = h_delete_features;
  m["suppress_features"] = h_suppress_features;
  m["measure_distance"] = h_measure_distance;
}
