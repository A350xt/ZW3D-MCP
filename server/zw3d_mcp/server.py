"""MCP tool definitions. Every tool is a thin, validated wrapper over one plugin method.

Units: millimetres and degrees. Coordinates are [x, y, z] in the part's global frame.
"""

from __future__ import annotations

import os
import tempfile
from typing import Literal

from . import __version__, validation as v
from .bridge import ZW3DError, get_bridge

try:  # mcp >= 2.0
    from mcp.server.mcpserver import Image, MCPServer as _Server
except ImportError:  # mcp 1.x
    from mcp.server.fastmcp import FastMCP as _Server, Image  # type: ignore

INSTRUCTIONS = (
    "Controls a running ZW3D 2025+ (not ZWCAD). Units: millimetres and degrees. "
    "Typical loop: new_part -> create_*/extrude_polygon/revolve_polygon/boolean -> "
    "get_mass_props / get_bounding_box / screenshot -> fix. Every creation tool returns the new "
    "shape_id plus its bounding box and volume so you can verify without extra calls. "
    "Solids are referenced by integer shape_id."
)

mcp = _Server("zw3d-mcp", instructions=INSTRUCTIONS)

Combine = Literal["none", "add", "remove", "intersect"]
_COMBINE_HELP = (
    "combine: 'none' (default) creates a separate new solid and returns its shape_id; 'add'/'remove'/'intersect' "
    "merge into / cut from / intersect with existing solids (shape_id is then null, see 'shapes')."
)


def _with_combine_help(fn):
    fn.__doc__ = (fn.__doc__ or "") + _COMBINE_HELP
    return fn


def _call(method: str, params: dict | None = None, timeout_ms: int = 60_000) -> dict:
    return get_bridge().call(method, params, timeout_ms)


# ---------------------------------------------------------------- session / files
@mcp.tool()
def ping() -> dict:
    """Check the plugin connection. Returns plugin version, ZW3D active file and active root (part) name."""
    r = _call("ping", timeout_ms=10_000)
    r["server_version"] = __version__
    return r


@mcp.tool()
def new_part(name: str, part_name: str | None = None) -> dict:
    """Create a new ZW3D file containing an empty part and make it active.

    name: file name (without folder, e.g. "bracket") or a full path. part_name: accepted for compatibility; single-root part files
    (.Z3PRT) have a fixed root name, so it is not applied. Call this first when ping shows no active part."""
    if not name or not name.strip():
        raise v.bad("name must not be empty")
    return _call("new_part", {"name": name.strip(), "part_name": part_name})


@mcp.tool()
def save_part(path: str | None = None) -> dict:
    """Save the active file. Give `path` (full path, e.g. C:\\work\\a.Z3PRT) to Save As, required for a file
    that was never saved."""
    return _call("save_part", {"path": path} if path else {})


@mcp.tool()
def export_step(path: str) -> dict:
    """Export the active file to a STEP file at `path` (full path ending in .step or .stp)."""
    if not path.lower().endswith((".step", ".stp")):
        raise v.bad("path must end with .step or .stp")
    return _call("export_step", {"path": path})


# ---------------------------------------------------------------- primitives
@mcp.tool()
@_with_combine_help
def create_box(
    center: list[float] | None = None,
    size: list[float] | None = None,
    corner1: list[float] | None = None,
    corner2: list[float] | None = None,
    combine: Combine = "none",
) -> dict:
    """Create an axis-aligned box. Give EITHER center + size [dx, dy, dz] OR two opposite corners (corner1, corner2).
    Units mm. """
    if corner1 is not None or corner2 is not None:
        if corner1 is None or corner2 is None or center is not None or size is not None:
            raise v.bad("Use either (corner1, corner2) or (center, size), not a mix")
        a, b = v.point(corner1, "corner1"), v.point(corner2, "corner2")
        size_ = [abs(a[i] - b[i]) for i in range(3)]
        center_ = [(a[i] + b[i]) / 2 for i in range(3)]
    else:
        if center is None or size is None:
            raise v.bad("Provide center and size, or corner1 and corner2")
        center_, size_ = v.point(center, "center"), v.point(size, "size")
    for i, n in enumerate("xyz"):
        v.positive(size_[i], f"size.{n}")
    return _call("create_box", {"center": center_, "size": size_, "combine": combine})


@mcp.tool()
@_with_combine_help
def create_cylinder(base_center: list[float], radius: float, height: float, combine: Combine = "none") -> dict:
    """Create a cylinder whose axis is parallel to +Z. `base_center` is the centre of the bottom face; the cylinder
    extends `height` along +Z. Units mm. Use combine='remove' to drill a hole. """
    return _call("create_cylinder", {
        "base_center": v.point(base_center, "base_center"),
        "radius": v.positive(radius, "radius"),
        "height": v.positive(height, "height"),
        "combine": combine,
    })


@mcp.tool()
@_with_combine_help
def create_sphere(center: list[float], radius: float, combine: Combine = "none") -> dict:
    """Create a sphere. Units mm. """
    return _call("create_sphere", {
        "center": v.point(center, "center"),
        "radius": v.positive(radius, "radius"),
        "combine": combine,
    })


# ---------------------------------------------------------------- profile features
@mcp.tool()
@_with_combine_help
def extrude_polygon(
    points: list[list[float]],
    distance: float,
    start: float = 0.0,
    direction: list[float] | None = None,
    draft_angle: float = 0.0,
    combine: Combine = "none",
) -> dict:
    """Extrude a closed planar polygon into a solid.

    points: vertices [[x,y,z],...] in order around the outline (>=3, planar, do not repeat the first point).
    distance: end offset in mm from the profile plane along the extrusion direction (> start).
    start: start offset in mm (default 0). direction: optional extrusion direction vector; by default the
    profile normal is used. Pass it explicitly (e.g. [0,0,1]) when the sign of the normal matters.
    draft_angle in degrees. Check the returned bounding box to confirm the extrusion went the intended way. """
    pts = v.polygon(points)
    if distance <= start:
        raise v.bad("distance must be greater than start", "distance is the END offset; use start for an offset start.")
    params = {"points": pts, "distance": float(distance), "start": float(start),
              "draft_angle": float(draft_angle), "combine": combine}
    params["direction"] = v.vector(direction, "direction") if direction is not None else None
    return _call("extrude_polygon", params)


@mcp.tool()
@_with_combine_help
def revolve_polygon(
    points: list[list[float]],
    axis_point: list[float],
    axis_direction: list[float],
    angle: float = 360.0,
    start_angle: float = 0.0,
    combine: Combine = "none",
) -> dict:
    """Revolve a closed planar polygon around an axis.

    points: profile vertices [[x,y,z],...] (>=3, planar, same plane as the axis). axis_point/axis_direction define
    the rotation axis; the profile must not cross the axis. angle: end angle in degrees (default 360, full turn),
    start_angle in degrees (default 0). """
    pts = v.polygon(points)
    if not (0 < angle - start_angle <= 360):
        raise v.bad("angle - start_angle must be in (0, 360]")
    ap, ad = v.point(axis_point, "axis_point"), v.vector(axis_direction, "axis_direction")
    v.check_axis_not_crossed(pts, ap, ad)
    return _call("revolve_polygon", {
        "points": pts,
        "axis_point": ap,
        "axis_direction": ad,
        "angle": float(angle), "start_angle": float(start_angle), "combine": combine,
    })


@mcp.tool()
@_with_combine_help
def extrude_profile(
    segments: list[dict],
    distance: float,
    start: float = 0.0,
    direction: list[float] | None = None,
    draft_angle: float = 0.0,
    combine: Combine = "none",
) -> dict:
    """Extrude a closed profile made of lines, arcs and circles (use this instead of extrude_polygon for round shapes).

    segments (mm), in order around the outline; the last end must equal the first start:
      {"type":"line","start":[x,y,z],"end":[x,y,z]}
      {"type":"arc","start":[..],"through":[..],"end":[..]}   (through = any point on the arc between start and end)
      {"type":"circle","center":[..],"radius":r,"normal":[0,0,1]}   (a circle is the ONLY segment of its profile)
    Other parameters as in extrude_polygon. Hole example: create the solid, then extrude a circle with
    combine='remove' and a distance that goes through it. """
    segs = v.segments(segments)
    if distance <= start:
        raise v.bad("distance must be greater than start")
    params = {"segments": segs, "distance": float(distance), "start": float(start),
              "draft_angle": float(draft_angle), "combine": combine,
              "direction": v.vector(direction, "direction") if direction is not None else None}
    return _call("extrude_profile", params)


@mcp.tool()
@_with_combine_help
def revolve_profile(
    segments: list[dict],
    axis_point: list[float],
    axis_direction: list[float],
    angle: float = 360.0,
    start_angle: float = 0.0,
    combine: Combine = "none",
) -> dict:
    """Revolve a closed profile of lines/arcs (same segment format as extrude_profile) around an axis.
    Typical use: a half cross-section of a shaft, pulley or bowl. Angles in degrees. """
    segs = v.segments(segments)
    if not (0 < angle - start_angle <= 360):
        raise v.bad("angle - start_angle must be in (0, 360]")
    ap, ad = v.point(axis_point, "axis_point"), v.vector(axis_direction, "axis_direction")
    v.check_axis_not_crossed(v._profile_points(segs), ap, ad)
    return _call("revolve_profile", {
        "segments": segs, "axis_point": ap, "axis_direction": ad,
        "angle": float(angle), "start_angle": float(start_angle), "combine": combine})


@mcp.tool()
@_with_combine_help
def sketch_extrude(
    plane: str | dict,
    segments: list[dict],
    distance: float,
    start: float = 0.0,
    symmetric: bool = False,
    reverse: bool = False,
    draft_angle: float = 0.0,
    dimensions: bool = True,
    combine: Combine = "none",
    holes: list[list[dict]] | None = None,
) -> dict:
    """Create a real SKETCH on a plane and extrude it (the sketch stays in the feature tree, so the part can be edited
    from the sketch later; extrude_profile/extrude_polygon only draw loose wireframe instead).

    plane: 'XY' (normal +Z), 'YZ' (normal +X), 'ZX' (normal +Y); or {"name":"XY","origin":[0,0,20]} for an offset
    plane; or a full frame {"origin":[..],"x_axis":[..],"y_axis":[..]} (normal = x_axis x y_axis).
    segments: 2D points [x, y] in the sketch frame (mm), one closed loop of lines/arcs, or a single circle:
      {"type":"line","start":[x,y],"end":[x,y]} | {"type":"arc","start":[..],"through":[..],"end":[..]} |
      {"type":"circle","center":[x,y],"radius":r}
    distance: extrusion depth along the plane normal (mm); start: start offset; symmetric: extrude distance/2 to both
    sides; reverse: go against the normal. dimensions=True adds driving dimensions (line lengths, circle diameters)
    to the sketch (best effort; the result reports dimensions_added / dimensions_failed).
    holes: optional inner loops (each a segment list in the same 2D format, e.g. a circle) cut out of the profile,
    so one call makes a plate with holes or a washer. Inner loops must lie inside the outer loop and not touch it. """
    frame = v.sketch_plane(plane)
    segs = v.segments2d(segments)
    if distance <= 0:
        raise v.bad("distance must be positive")
    if symmetric:
        start, end = -distance / 2.0, distance / 2.0
    else:
        start, end = float(start), float(start) + float(distance)
    n = v.plane_normal(frame)
    params = {"plane": frame, "segments": segs, "distance": end, "start": start, "draft_angle": float(draft_angle),
              "direction": [-c for c in n] if reverse else None, "dimensions": bool(dimensions), "combine": combine}
    inner = v.inner_loops2d(holes)
    if inner:
        params["inner_loops"] = inner
    return _call("sketch_extrude", params)


@mcp.tool()
@_with_combine_help
def sketch_revolve(
    plane: str | dict,
    segments: list[dict],
    axis_point: list[float],
    axis_direction: list[float],
    angle: float = 360.0,
    start_angle: float = 0.0,
    dimensions: bool = True,
    combine: Combine = "none",
) -> dict:
    """Create a real sketch on a plane and revolve it. plane/segments as in sketch_extrude; axis_point and
    axis_direction are 2D ([x, y]) in the sketch frame and the profile must stay on ONE side of the axis.
    Angles in degrees. """
    frame = v.sketch_plane(plane)
    segs = v.segments2d(segments)
    if not (0 < angle - start_angle <= 360):
        raise v.bad("angle - start_angle must be in (0, 360]")
    ap = [float(c) for c in axis_point] if isinstance(axis_point, (list, tuple)) and len(axis_point) == 2 else None
    ad = [float(c) for c in axis_direction] if isinstance(axis_direction, (list, tuple)) and len(axis_direction) == 2 else None
    if ap is None or ad is None or (ad[0] == 0 and ad[1] == 0):
        raise v.bad("axis_point and axis_direction must be 2D [x, y] (direction not zero)")
    v.check_axis_not_crossed(v._profile_points(segs), ap, ad)
    return _call("sketch_revolve", {
        "plane": frame, "segments": segs, "axis_point": ap, "axis_direction": ad,
        "angle": float(angle), "start_angle": float(start_angle), "dimensions": bool(dimensions), "combine": combine})


# ---------------------------------------------------------------- modify
@mcp.tool()
def boolean(operation: Literal["add", "subtract", "intersect"], base_id: int, tool_ids: list[int],
            keep_tools: bool = False) -> dict:
    """Combine solids: base (+|-|intersect) tools. Returns the surviving shape ids and the base solid's bbox/volume.
    keep_tools=True keeps the tool solids instead of consuming them."""
    return _call("boolean", {
        "operation": operation,
        "base_id": v.id_list([base_id], "base_id")[0],
        "tool_ids": v.id_list(tool_ids, "tool_ids"),
        "keep_tools": bool(keep_tools),
    })


@mcp.tool()
def fillet_edges(shape_id: int, radius: float, edge_ids: list[int] | None = None) -> dict:
    """Fillet edges of a solid with a constant radius (mm). Without edge_ids ALL edges of the shape are filleted.
    Get edge ids with list_edges. A radius that is too large fails with a modeling error; try a smaller one."""
    params = {"shape_id": v.id_list([shape_id], "shape_id")[0], "radius": v.positive(radius, "radius")}
    if edge_ids is not None:
        params["edge_ids"] = v.id_list(edge_ids, "edge_ids")
    return _call("fillet_edges", params)


@mcp.tool()
def chamfer_edges(shape_id: int, distance: float, edge_ids: list[int] | None = None,
                  distance2: float | None = None) -> dict:
    """Chamfer edges of a solid. distance (mm) is the symmetric setback; give distance2 for an asymmetric chamfer.
    Without edge_ids ALL edges are chamfered. Use list_edges (it returns each edge's bounding box) to pick edges."""
    params = {"shape_id": v.id_list([shape_id], "shape_id")[0], "distance": v.positive(distance, "distance")}
    if edge_ids is not None:
        params["edge_ids"] = v.id_list(edge_ids, "edge_ids")
    if distance2 is not None:
        params["distance2"] = v.positive(distance2, "distance2")
    return _call("chamfer_edges", params)


@mcp.tool()
def shell_shape(shape_id: int, thickness: float, open_face_ids: list[int] | None = None,
                inward: bool = True) -> dict:
    """Hollow out a solid with a uniform wall `thickness` (mm). open_face_ids are faces that are removed (left open),
    e.g. the top face of a box to make an open container; get them from list_faces (planar faces report plane_normal).
    inward=True (default) keeps the outer size and cuts the cavity inside; inward=False grows the wall outward."""
    params = {"shape_id": v.id_list([shape_id], "shape_id")[0], "thickness": v.positive(thickness, "thickness")}
    params["inward"] = bool(inward)
    if open_face_ids:
        params["open_face_ids"] = v.id_list(open_face_ids, "open_face_ids")
    return _call("shell_shape", params)


@mcp.tool()
def undo(steps: int = 1) -> dict:
    """Undo the last `steps` operations (1..50); one modeling tool call is one step. Returns the remaining shapes plus
    removed_shape_ids / added_shape_ids so you can see what actually changed."""
    if not (1 <= steps <= 50):
        raise v.bad("steps must be between 1 and 50")
    return _call("undo", {"steps": steps})


@mcp.tool()
def redo(steps: int = 1) -> dict:
    """Redo `steps` previously undone operations (1..50)."""
    if not (1 <= steps <= 50):
        raise v.bad("steps must be between 1 and 50")
    return _call("redo", {"steps": steps})


# ---------------------------------------------------------------- features: hole / pattern / mirror / draft / sweep / loft
@mcp.tool()
def create_hole(face_id: int, points: list[list[float]], diameter: float, depth: float | None = None,
                through: bool = False, hole_type: Literal["simple", "counterbore", "countersink"] = "simple",
                diameter2: float | None = None, depth2: float | None = None) -> dict:
    """Drill hole(s) into a planar face, perpendicular to it. face_id from list_faces; points: [[x,y,z],...] locations
    (projected onto the face); diameter in mm; give depth (blind) or through=True. counterbore/countersink use
    diameter2 (default 1.6 x diameter) and depth2 (default 0.6 x diameter). Returns feature_id (usable by
    pattern_linear / pattern_circular / mirror) and the resulting shapes with bbox and volume."""
    if not points:
        raise v.bad("points must list at least one hole location")
    params = {"face_id": v.id_list([face_id], "face_id")[0], "points": [v.point(p, "points[i]") for p in points],
              "diameter": v.positive(diameter, "diameter"), "through": bool(through), "hole_type": hole_type}
    if not through:
        if depth is None:
            raise v.bad("A blind hole needs depth (or set through=True)")
        params["depth"] = v.positive(depth, "depth")
    if diameter2 is not None:
        params["diameter2"] = v.positive(diameter2, "diameter2")
    if depth2 is not None:
        params["depth2"] = v.positive(depth2, "depth2")
    return _call("create_hole", params)


def _target(shape_id, feature_ids):
    if (shape_id is None) == (feature_ids is None):
        raise v.bad("Give exactly one of shape_id (pattern/mirror a solid) or feature_ids (pattern/mirror features, "
                    "e.g. a hole's feature_id)")
    if shape_id is not None:
        return "shape", {"shape_id": v.id_list([shape_id], "shape_id")[0]}
    return "feature", {"feature_ids": v.id_list(feature_ids, "feature_ids")}


@mcp.tool()
@_with_combine_help
def pattern_linear(count: int, spacing: float, direction: list[float], shape_id: int | None = None,
                   feature_ids: list[int] | None = None, count2: int | None = None, spacing2: float | None = None,
                   direction2: list[float] | None = None, combine: Combine = "none") -> dict:
    """Linear pattern of a solid (shape_id) or of features (feature_ids, e.g. a hole). count includes the original.
    spacing in mm along direction; add count2/spacing2/direction2 for a second direction (grid). For solids use
    combine='add' to fuse the copies, 'none' for separate bodies. """
    kind, target = _target(shape_id, feature_ids)
    if not isinstance(count, int) or count < 2:
        raise v.bad("count must be an integer >= 2 (it includes the original)")
    params = {"kind": "linear", "count": count, "spacing": v.positive(spacing, "spacing"),
              "direction": v.vector(direction, "direction"), "combine": combine, **target}
    if count2 is not None or spacing2 is not None or direction2 is not None:
        if count2 is None or spacing2 is None or direction2 is None or count2 < 2:
            raise v.bad("A second direction needs count2 (>=2), spacing2 and direction2 together")
        params.update({"count2": count2, "spacing2": v.positive(spacing2, "spacing2"),
                       "direction2": v.vector(direction2, "direction2")})
    return _call("pattern_shape" if kind == "shape" else "pattern_feature", params)


@mcp.tool()
@_with_combine_help
def pattern_circular(count: int, axis_point: list[float], axis_direction: list[float], angle: float | None = None,
                     shape_id: int | None = None, feature_ids: list[int] | None = None,
                     combine: Combine = "none") -> dict:
    """Circular pattern around an axis. count includes the original; angle is the step between neighbours in degrees
    (default 360/count = evenly around the full circle). Target is a solid (shape_id) or features (feature_ids). """
    kind, target = _target(shape_id, feature_ids)
    if not isinstance(count, int) or count < 2:
        raise v.bad("count must be an integer >= 2 (it includes the original)")
    step = 360.0 / count if angle is None else v.positive(angle, "angle")
    return _call("pattern_shape" if kind == "shape" else "pattern_feature", {
        "kind": "circular", "count": count, "angle": step, "axis_point": v.point(axis_point, "axis_point"),
        "axis_direction": v.vector(axis_direction, "axis_direction"), "combine": combine, **target})


@mcp.tool()
@_with_combine_help
def mirror(plane: str | dict, shape_id: int | None = None, feature_ids: list[int] | None = None,
           combine: Combine = "none") -> dict:
    """Mirror a solid (shape_id) or features (feature_ids) across a plane: 'XY', 'YZ', 'ZX' (through the origin),
    {"name":"YZ","origin":[x,y,z]} for an offset plane, or {"origin":[..],"normal":[..]}. For a solid, combine='add' fuses the
    mirror image into the original (symmetric part), 'none' keeps it as a separate body. """
    kind, target = _target(shape_id, feature_ids)
    return _call("mirror_shape" if kind == "shape" else "mirror_feature",
                 {"plane": v.mirror_plane(plane), "combine": combine, **target})


@mcp.tool()
def draft_faces(face_ids: list[int], neutral_face_ids: list[int], angle: float, pull_direction: list[float]) -> dict:
    """Add a draft (taper) to faces: they tilt by `angle` degrees (0 < angle < 89) about the neutral faces, which stay
    fixed, relative to the pull_direction vector (the mold opening direction). Use list_faces to pick faces."""
    if not (0 < angle < 89):
        raise v.bad("angle must be between 0 and 89 degrees")
    return _call("draft_faces", {"face_ids": v.id_list(face_ids, "face_ids"),
                                 "neutral_face_ids": v.id_list(neutral_face_ids, "neutral_face_ids"),
                                 "angle": float(angle), "pull_direction": v.vector(pull_direction, "pull_direction")})


@mcp.tool()
@_with_combine_help
def sweep(profile: list[dict], path: list[dict], combine: Combine = "none") -> dict:
    """Sweep a closed profile along a path. profile: closed loop of 3D segments (same line/arc/circle format as
    extrude_profile) lying in a plane PERPENDICULAR to the path at its start. path: connected open chain of 3D
    {"type":"line"|"arc",...} segments, each start equal to the previous end. Example: a pipe = circle profile + straight
    or arced path. Loose wireframe is used for profile and path (no sketch). """
    return _call("sweep", {"profile": v.segments(profile, "profile"), "path": v.path_segments(path), "combine": combine})


@mcp.tool()
@_with_combine_help
def loft(sections: list[list[dict]], combine: Combine = "none") -> dict:
    """Loft a solid through 2 or more closed cross-sections (each a closed loop of 3D segments, e.g. a circle at
    z=0 and a square at z=50). Keep the sections in order and give them the same segment count/orientation for a clean
    result. """
    if not isinstance(sections, (list, tuple)) or len(sections) < 2:
        raise v.bad("loft needs at least 2 sections")
    return _call("loft", {"sections": [v.segments(sec, f"sections[{i}]") for i, sec in enumerate(sections)],
                          "combine": combine})


@mcp.tool()
def list_features() -> dict:
    """List the feature ids of the active part's history (use with pattern_*/mirror feature targets)."""
    return _call("list_features")


# ---------------------------------------------------------------- variables / parametric edits
@mcp.tool()
def list_variables() -> dict:
    """List the active part's variables (name, kind, value in mm/deg, expression)."""
    return _call("list_variables")


@mcp.tool()
def set_variable(name: str, value: float | None = None, expression: str | None = None) -> dict:
    """Change an existing part variable (give a numeric value in mm/deg, or an expression string) and regenerate the part.
    Returns the new shapes with bbox/volume so the effect can be checked."""
    if (value is None) == (expression is None):
        raise v.bad("Give exactly one of value or expression")
    params: dict = {"name": name}
    if value is not None:
        params["value"] = v.number(value, "value")
    else:
        params["expression"] = expression
    return _call("set_variable", params)


@mcp.tool()
def add_variable(name: str, value: float | None = None, expression: str | None = None,
                 kind: Literal["number", "distance", "angle"] = "number") -> dict:
    """Create a new part variable (kind: unitless number, distance in mm, angle in degrees)."""
    if (value is None) == (expression is None):
        raise v.bad("Give exactly one of value or expression")
    params: dict = {"name": name, "kind": kind}
    if value is not None:
        params["value"] = v.number(value, "value")
    else:
        params["expression"] = expression
    return _call("add_variable", params)


@mcp.tool()
def list_sketch_variables(sketch_id: int) -> dict:
    """List the driving dimensions (variables) of a sketch created by sketch_extrude/sketch_revolve (use its sketch_id)."""
    return _call("list_sketch_variables", {"sketch_id": v.id_list([sketch_id], "sketch_id")[0]})


@mcp.tool()
def set_sketch_variable(sketch_id: int, name: str, value: float | None = None, expression: str | None = None) -> dict:
    """Edit one sketch dimension and regenerate the model: this is how you resize a feature after the fact. Get names from
    list_sketch_variables. Returns the updated shapes with bbox/volume."""
    if (value is None) == (expression is None):
        raise v.bad("Give exactly one of value or expression")
    params: dict = {"sketch_id": v.id_list([sketch_id], "sketch_id")[0], "name": name}
    if value is not None:
        params["value"] = v.number(value, "value")
    else:
        params["expression"] = expression
    return _call("set_sketch_variable", params)


# ---------------------------------------------------------------- assemblies and drawings (basic)
@mcp.tool()
def new_assembly(name: str) -> dict:
    """Create a new assembly file (.Z3ASM) and make it active. Then use insert_component. Part modeling tools do not work
    inside an assembly: switch back with new_part or by opening a part."""
    if not name or not name.strip():
        raise v.bad("name must not be empty")
    return _call("new_assembly", {"name": name.strip()})


@mcp.tool()
def insert_component(file: str, position: list[float] | None = None, anchor: bool = False,
                     part_name: str | None = None) -> dict:
    """Insert a saved part file (full path, e.g. C:\\work\\bolt.Z3PRT) into the active assembly at `position` [x,y,z] mm
    (default origin). anchor=True fixes it in place. Returns component_id."""
    if not file or not file.strip():
        raise v.bad("file must be the path of a saved part")
    params: dict = {"file": file.strip(), "anchor": bool(anchor)}
    if position is not None:
        params["position"] = v.point(position, "position")
    if part_name:
        params["part_name"] = part_name
    return _call("insert_component", params)


@mcp.tool()
def list_components() -> dict:
    """List the component ids in the active assembly."""
    return _call("list_components")


@mcp.tool()
def new_drawing(name: str, part_file: str) -> dict:
    """Create a new drawing (.Z3DRW) with the standard multi-view layout of a saved part or assembly (give its full path;
    save_part first). No dimensions or annotations are added yet; save_part (with a path) stores the drawing."""
    if not name.strip() or not part_file.strip():
        raise v.bad("name and part_file must not be empty", "part_file must be the full path of a saved part/assembly.")
    return _call("new_drawing", {"name": name.strip(), "part_file": part_file.strip()})


# ---------------------------------------------------------------- queries
@mcp.tool()
def list_shapes() -> dict:
    """List all solid shape ids in the active part, each with bounding box and volume."""
    return _call("list_shapes")


@mcp.tool()
def list_faces(shape_id: int) -> dict:
    """List the face ids of a shape with bounding box and surface type; planar faces include plane_normal
    (axis direction, sign not guaranteed to point outward), cylinders include radius and axis."""
    return _call("list_faces", {"shape_id": v.id_list([shape_id], "shape_id")[0]})


@mcp.tool()
def list_edges(shape_id: int) -> dict:
    """List the edges of a shape: edge_ids plus `edges`, each with bounding_box and `curve` (line: start/end;
    arc/circle: center/radius). Use with fillet_edges / chamfer_edges to pick edges by position."""
    return _call("list_edges", {"shape_id": v.id_list([shape_id], "shape_id")[0]})


@mcp.tool()
def get_mass_props(shape_id: int, density: float | None = None) -> dict:
    """Volume (mm^3), surface area (mm^2), mass_kg and centre of gravity [x,y,z] (mm) of a solid.
    density (kg/m^3) is optional; omit it to use the part's own material density."""
    params: dict = {"shape_id": v.id_list([shape_id], "shape_id")[0]}
    if density is not None:
        params["density"] = v.positive(density, "density")
    return _call("get_mass_props", params)


@mcp.tool()
def get_bounding_box(shape_id: int) -> dict:
    """Axis-aligned bounding box {min:[x,y,z], max:[x,y,z], size:[dx,dy,dz]} in mm."""
    return _call("get_bounding_box", {"shape_id": v.id_list([shape_id], "shape_id")[0]})


@mcp.tool()
def screenshot(width: int = 800, height: int = 600, zoom_all: bool = True) -> Image:
    """Capture the current ZW3D view as a PNG image (zoomed to fit unless zoom_all=False)."""
    if not (64 <= width <= 4096 and 64 <= height <= 4096):
        raise v.bad("width and height must be between 64 and 4096")
    r = _call("screenshot", {"width": width, "height": height, "zoom_all": zoom_all})
    path = os.path.abspath(r["path"])
    # The reply comes from a local socket with no authentication: only ever read/delete our own screenshot files.
    shot_dir = os.path.join(tempfile.gettempdir(), "zw3d_mcp")
    name = os.path.basename(path)
    if os.path.normcase(os.path.dirname(path)) != os.path.normcase(os.path.abspath(shot_dir))             or not (name.startswith("shot_") and name.endswith(".png")):
        raise ZW3DError("INTERNAL", f"Refusing to read unexpected screenshot path {path!r}",
                        "The plugin should write shot_*.png into the zw3d_mcp folder of the system temp directory.")
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as e:
        raise ZW3DError("INTERNAL", f"Plugin reported {path} but it cannot be read: {e}",
                        "The MCP server must run on the same machine as ZW3D.")
    finally:
        try:
            os.remove(path)  # always recycle the temp file, success or not
        except OSError:
            pass
        try:
            os.rmdir(shot_dir)  # only succeeds when empty
        except OSError:
            pass
    return Image(data=data, format="png")


@mcp.tool()
def run_command(command: str) -> dict:
    """Escape hatch: send a raw ZW3D command string. Disabled unless ZW3D_MCP_ENABLE_RUN_COMMAND=1 is set in ZW3D's
    environment; behaviour is not guaranteed."""
    if not command.strip():
        raise v.bad("command must not be empty")
    return _call("run_command", {"command": command})


def main() -> None:
    mcp.run()


if __name__ == "__main__":
    main()
