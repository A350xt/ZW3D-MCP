// Read-only handlers (no model changes, so no DispGuard) and the method registry.
#include <algorithm>
#include <cstring>
#include "handlers.h"
#include "../dispatcher.h"
#include "../errors.h"
#include "../plugin_info.h"

namespace {

json h_ping(const json&) {
  return {{"plugin_version", ZW3DMCP_VERSION},
          {"active_file", current_file_utf8()},
          {"active_root", current_root_utf8()},
          // Verification aid for the "handlers run on the main thread" assumption (docs/api_notes.md).
          {"handler_thread_id", (unsigned long)GetCurrentThreadId()},
          {"init_thread_id", (unsigned long)Dispatcher::current_main_thread_id()},
          {"main_thread", GetCurrentThreadId() == Dispatcher::current_main_thread_id()}};
}

json h_list_shapes(const json&) {
  json shapes = json::array();
  for (int id : inq_shape_ids()) shapes.push_back(describe_shape(id));
  return {{"count", shapes.size()}, {"shapes", shapes}};
}

json h_list_faces(const json& p) {
  int shape = p.at("shape_id").get<int>();
  require_shape(shape);
  int n = 0;
  int* ids = nullptr;
  check_api(cvxPartInqShapeFaces(shape, &n, &ids), "cvxPartInqShapeFaces");
  std::vector<int> faces(ids, ids + n);
  if (ids) cvxMemFree(reinterpret_cast<void**>(&ids));
  json list = json::array();
  for (int f : faces) {
    svxBndBox box{};
    json item = {{"face_id", f}};
    if (cvxPartInqFaceBox(f, &box) == 0) item["bounding_box"] = bbox_json(box);
    svxSrfPrim sp;
    std::memset(&sp, 0, sizeof(sp));
    if (cvxPartInqFaceSrfPrim(f, &sp) == 0) {
      switch (sp.srfType) {
        case VX_SF_PRIM_PLN: {
          const svxMatrix& m = sp.srfData.pln.form;
          item["surface"] = "plane";
          item["plane_normal"] = {m.zx, m.zy, m.zz};  // z axis of the plane frame; sign may not be outward
          break;
        }
        case VX_SF_PRIM_CYL: {
          const svxMatrix& m = sp.srfData.cyl.form;
          item["surface"] = "cylinder";
          item["radius"] = sp.srfData.cyl.radius;
          item["axis"] = {m.zx, m.zy, m.zz};
          break;
        }
        case VX_SF_PRIM_SPH: item["surface"] = "sphere"; break;
        case VX_SF_PRIM_CON: item["surface"] = "cone"; break;
        case VX_SF_PRIM_TORUS: item["surface"] = "torus"; break;
        case VX_SF_PRIM_NURB: item["surface"] = "nurbs"; cvxSurfaceFree(&sp.srfData.srf); break;
        default: item["surface"] = "other"; break;
      }
    }
    list.push_back(item);
  }
  return {{"shape_id", shape}, {"count", faces.size()}, {"faces", list}};
}

json h_list_edges(const json& p) {
  int shape = p.at("shape_id").get<int>();
  require_shape(shape);
  int n = 0;
  int* ids = nullptr;
  check_api(cvxPartInqShapeEdges(shape, &n, &ids), "cvxPartInqShapeEdges");
  std::vector<int> edges(ids, ids + n);
  if (ids) cvxMemFree(reinterpret_cast<void**>(&ids));
  json detail = json::array();
  for (int e : edges) {
    json item = {{"edge_id", e}};
    svxBndBox box{};
    // cvxPartInqEntBox only supports shape/face/component; edges go through the general entity box.
    if (cvxEntBndBox(e, &box) == 0) item["bounding_box"] = bbox_json(box);
    svxCurve crv;
    std::memset(&crv, 0, sizeof(crv));
    if (cvxPartInqEdgeCrv(e, 0, &crv) == 0) {
      switch (crv.Type) {
        case VX_CRV_LINE: item["curve"] = {{"type", "line"}, {"start", from_point(crv.P1)}, {"end", from_point(crv.P2)}}; break;
        case VX_CRV_ARC:
        case VX_CRV_CIRCLE:
          item["curve"] = {{"type", crv.Type == VX_CRV_ARC ? "arc" : "circle"},
                           {"center", {crv.Frame.xt, crv.Frame.yt, crv.Frame.zt}},
                           {"radius", crv.R}};
          break;
        default: item["curve"] = {{"type", "nurbs"}}; break;
      }
      cvxCurveFree(&crv);
    }
    detail.push_back(item);
  }
  return {{"shape_id", shape}, {"count", edges.size()}, {"edge_ids", edges}, {"edges", detail}};
}

json h_get_mass_props(const json& p) {
  int shape = p.at("shape_id").get<int>();
  require_shape(shape);
  svxMassProp mp{};
  check_api(cvxPartInqShapeMass(shape, 0.0, &mp), "cvxPartInqShapeMass");  // 0 = shape's own density
  // Measured on ZW3D 2025: mp.Density is kg/m^3 and mp.Mass is Volume(mm^3) * Density, i.e. NOT kg. Compute kg here.
  double density = p.value("density", mp.Density);  // kg/m^3
  double mass_kg = mp.Volume * 1e-9 * density;
  return {{"shape_id", shape},
          {"volume", mp.Volume},   // mm^3
          {"area", mp.Area},       // mm^2
          {"mass_kg", mass_kg},
          {"density_kg_m3", density},
          {"center_of_gravity", from_point(mp.Center)}};
}

json h_get_bounding_box(const json& p) {
  int shape = p.at("shape_id").get<int>();
  require_shape(shape);
  svxBndBox box{};
  check_api(cvxPartInqShapeBox(shape, nullptr, &box), "cvxPartInqShapeBox");
  json out = bbox_json(box);
  out["shape_id"] = shape;
  return out;
}

}  // namespace

const std::map<std::string, Handler>& handler_registry() {
  static const std::map<std::string, Handler> reg = [] {
    std::map<std::string, Handler> m;
    register_query_handlers(m);
    register_part_handlers(m);
    register_file_handlers(m);
    return m;
  }();
  return reg;
}

void register_query_handlers(std::map<std::string, Handler>& m) {
  m["ping"] = h_ping;
  m["list_shapes"] = h_list_shapes;
  m["list_faces"] = h_list_faces;
  m["list_edges"] = h_list_edges;
  m["get_mass_props"] = h_get_mass_props;
  m["get_bounding_box"] = h_get_bounding_box;
}
