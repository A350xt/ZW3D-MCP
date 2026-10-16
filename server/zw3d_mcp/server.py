"""MCP tool definitions. Every tool is a thin, validated wrapper over one plugin method.

Units: millimetres and degrees. Coordinates are [x, y, z] in the part's global frame.
"""

from __future__ import annotations

import os
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
    return _call("revolve_polygon", {
        "points": pts,
        "axis_point": v.point(axis_point, "axis_point"),
        "axis_direction": v.vector(axis_direction, "axis_direction"),
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
    if any(sg["type"] == "circle" for sg in segs):
        pass  # a circle revolved about an outside axis gives a torus; allowed
    if not (0 < angle - start_angle <= 360):
        raise v.bad("angle - start_angle must be in (0, 360]")
    return _call("revolve_profile", {
        "segments": segs, "axis_point": v.point(axis_point, "axis_point"),
        "axis_direction": v.vector(axis_direction, "axis_direction"),
        "angle": float(angle), "start_angle": float(start_angle), "combine": combine})


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
def shell_shape(shape_id: int, thickness: float, open_face_ids: list[int] | None = None) -> dict:
    """Hollow out a solid with a uniform wall `thickness` (mm). open_face_ids are faces that are removed (left open),
    e.g. the top face of a box to make an open container; get them from list_faces (planar faces report plane_normal)."""
    params = {"shape_id": v.id_list([shape_id], "shape_id")[0], "thickness": v.positive(thickness, "thickness")}
    if open_face_ids:
        params["open_face_ids"] = v.id_list(open_face_ids, "open_face_ids")
    return _call("shell_shape", params)


@mcp.tool()
def undo(steps: int = 1) -> dict:
    """Undo the last `steps` operations (1..50). Returns the shapes that remain."""
    if not (1 <= steps <= 50):
        raise v.bad("steps must be between 1 and 50")
    return _call("undo", {"steps": steps})


@mcp.tool()
def redo(steps: int = 1) -> dict:
    """Redo `steps` previously undone operations (1..50)."""
    if not (1 <= steps <= 50):
        raise v.bad("steps must be between 1 and 50")
    return _call("redo", {"steps": steps})


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
    """List the edges of a shape: edge_ids plus `edges` with each edge's bounding box (use with fillet_edges /
    chamfer_edges; a straight edge's box spans its two end points)."""
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
    path = r["path"]
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as e:
        raise ZW3DError("INTERNAL", f"Plugin reported {path} but it cannot be read: {e}",
                        "The MCP server must run on the same machine as ZW3D.")
    finally:
        try:
            os.remove(path)
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
