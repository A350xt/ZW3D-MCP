#pragma once
#include <map>
#include <string>
#include <vector>
#include "json.hpp"
#include "VxApi.h"

using json = nlohmann::json;
using Handler = json (*)(const json& params);

// method name -> handler. Handlers run on ZW3D's main thread and may throw ApiError / json::exception.
const std::map<std::string, Handler>& handler_registry();

void register_part_handlers(std::map<std::string, Handler>& m);
void register_query_handlers(std::map<std::string, Handler>& m);
void register_file_handlers(std::map<std::string, Handler>& m);

// ---- shared helpers (common.cpp) -----------------------------------------------------------------
svxPoint to_point(const json& v, const char* name);
svxVector to_vector(const json& v, const char* name);
json from_point(const svxPoint& p);
evxBoolType parse_combine(const json& params);

// Pauses UI refresh while a handler edits the model; always restores it (even on exceptions).
struct DispGuard {
  vxDispState state{};
  DispGuard();
  ~DispGuard();
  DispGuard(const DispGuard&) = delete;
  DispGuard& operator=(const DispGuard&) = delete;
};

std::vector<int> inq_shape_ids();                // active part's solids
void require_shape(int id);                     // throws INVALID_INPUT if id is not a solid of the active part
json describe_shape(int id);                     // {shape_id, bounding_box{min,max,size}, volume, area}
json describe_shape_or_null(int id);             // same but null if the id no longer exists
json bbox_json(const svxBndBox& b);
// Result of a creation call: describes `id` if one was produced, otherwise lists the surviving shapes.
json creation_result(evxBoolType combine, int id);
std::string current_file_utf8();
std::string current_root_utf8();
