"""Client-side parameter checks, so obviously bad calls never reach ZW3D."""

from __future__ import annotations

import math
from typing import Sequence

from .bridge import ZW3DError

EPS = 1e-9


def bad(message: str, hint: str = "") -> ZW3DError:
    return ZW3DError("INVALID_INPUT", message, hint)


def point(p: Sequence[float], name: str) -> list[float]:
    if not isinstance(p, (list, tuple)) or len(p) != 3:
        raise bad(f"{name} must be [x, y, z]", "Use three numbers in millimetres.")
    out = []
    for v in p:
        if isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(v):
            raise bad(f"{name} contains a non-finite or non-numeric value: {v!r}")
        out.append(float(v))
    return out


def positive(value: float, name: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value <= 0:
        raise bad(f"{name} must be a positive number, got {value!r}", "Dimensions are in millimetres and must be > 0.")
    return float(value)


def _sub(a, b):
    return [a[0] - b[0], a[1] - b[1], a[2] - b[2]]


def _cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def _dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _norm(a):
    return math.sqrt(_dot(a, a))


def vector(v: Sequence[float], name: str) -> list[float]:
    v = point(v, name)
    if _norm(v) < EPS:
        raise bad(f"{name} must not be the zero vector")
    return v


def polygon(points: Sequence[Sequence[float]], name: str = "points") -> list[list[float]]:
    """>=3 vertices, no consecutive duplicates, planar, non-degenerate."""
    if not isinstance(points, (list, tuple)) or len(points) < 3:
        raise bad(f"{name} needs at least 3 vertices", "Give the closed outline once; do not repeat the first point at the end.")
    pts = [point(p, f"{name}[{i}]") for i, p in enumerate(points)]
    if _norm(_sub(pts[0], pts[-1])) < 1e-6:
        pts.pop()  # tolerate an explicitly closed outline
        if len(pts) < 3:
            raise bad(f"{name} needs at least 3 distinct vertices")
    for i in range(len(pts)):
        if _norm(_sub(pts[i], pts[(i + 1) % len(pts)])) < 1e-6:
            raise bad(f"{name}: vertices {i} and {(i + 1) % len(pts)} coincide", "Remove duplicate points.")
    # Newell normal -> twice the polygon area vector
    n = [0.0, 0.0, 0.0]
    for i in range(len(pts)):
        c = _cross(pts[i], pts[(i + 1) % len(pts)])
        n = [n[0] + c[0], n[1] + c[1], n[2] + c[2]]
    area2 = _norm(n)
    if area2 < 1e-6:
        raise bad(f"{name} is degenerate (zero area, vertices are collinear)")
    unit = [x / area2 for x in n]
    scale = max(_norm(_sub(p, pts[0])) for p in pts)
    for i, p in enumerate(pts):
        if abs(_dot(unit, _sub(p, pts[0]))) > 1e-6 * max(scale, 1.0) + 1e-6:
            raise bad(f"{name} is not planar (vertex {i} is off the plane)", "All vertices must lie in one plane.")
    return pts


def polygon_normal(pts: Sequence[Sequence[float]]) -> list[float]:
    n = [0.0, 0.0, 0.0]
    for i in range(len(pts)):
        c = _cross(pts[i], pts[(i + 1) % len(pts)])
        n = [n[0] + c[0], n[1] + c[1], n[2] + c[2]]
    length = _norm(n)
    return [x / length for x in n]


def id_list(ids: Sequence[int], name: str, minimum: int = 1) -> list[int]:
    if not isinstance(ids, (list, tuple)) or len(ids) < minimum:
        raise bad(f"{name} must be a list of at least {minimum} integer id(s)")
    out = []
    for v in ids:
        if isinstance(v, bool) or not isinstance(v, int) or v < 1:
            raise bad(f"{name} contains an invalid id: {v!r}", "Ids are positive integers returned by earlier calls.")
        out.append(v)
    return out


def segments(segs, name: str = "segments") -> list[dict]:
    """Validate a profile made of line / arc / circle segments (one closed loop, or a single circle)."""
    if not isinstance(segs, (list, tuple)) or not segs:
        raise bad(f"{name} must be a non-empty list of segments",
                  'Each segment is {"type":"line","start":p,"end":p}, {"type":"arc","start":p,"through":p,"end":p} '
                  'or {"type":"circle","center":p,"radius":r,"normal":[0,0,1]}.')
    out = []
    for i, sg in enumerate(segs):
        if not isinstance(sg, dict) or sg.get("type") not in ("line", "arc", "circle"):
            raise bad(f"{name}[{i}].type must be 'line', 'arc' or 'circle'")
        t = sg["type"]
        if t == "circle":
            item = {"type": t, "center": point(sg.get("center"), f"{name}[{i}].center"),
                    "radius": positive(sg.get("radius"), f"{name}[{i}].radius")}
            item["normal"] = vector(sg["normal"], f"{name}[{i}].normal") if sg.get("normal") is not None else [0.0, 0.0, 1.0]
        else:
            item = {"type": t, "start": point(sg.get("start"), f"{name}[{i}].start"),
                    "end": point(sg.get("end"), f"{name}[{i}].end")}
            if t == "arc":
                item["through"] = point(sg.get("through"), f"{name}[{i}].through")
                a, b, c = item["start"], item["through"], item["end"]
                if _norm(_cross(_sub(b, a), _sub(c, a))) < 1e-9:
                    raise bad(f"{name}[{i}]: start, through and end are collinear", "Pick a through point off the chord.")
            elif _norm(_sub(item["start"], item["end"])) < 1e-9:
                raise bad(f"{name}[{i}]: zero-length line")
        out.append(item)
    if any(sg["type"] == "circle" for sg in out):
        if len(out) != 1:
            raise bad("A circle must be the only segment of a profile",
                      "Make one profile call per circle, or combine with combine='remove' for holes.")
        return out
    # closed chain check: every end meets the next start (in any order is not supported)
    for i, sg in enumerate(out):
        nxt = out[(i + 1) % len(out)]
        if _norm(_sub(sg["end"], nxt["start"])) > 1e-6:
            raise bad(f"{name}: segment {i} does not end where segment {(i + 1) % len(out)} starts",
                      "List segments in order around the outline; the last end must equal the first start.")
    return out


PLANES = {  # right-handed frames: normal = x_axis x y_axis
    "XY": ([1.0, 0.0, 0.0], [0.0, 1.0, 0.0]),   # normal +Z
    "YZ": ([0.0, 1.0, 0.0], [0.0, 0.0, 1.0]),   # normal +X
    "ZX": ([0.0, 0.0, 1.0], [1.0, 0.0, 0.0]),   # normal +Y
}


def sketch_plane(plane) -> dict:
    """'XY' | 'YZ' | 'ZX', or {"name":"XY","origin":[0,0,20]}, or {"origin":p,"x_axis":v,"y_axis":v}. Returns the full frame."""
    origin = [0.0, 0.0, 0.0]
    if isinstance(plane, str):
        name = plane
    elif isinstance(plane, dict):
        name = plane.get("name")
        if plane.get("origin") is not None:
            origin = point(plane["origin"], "plane.origin")
        if name is None:
            x = vector(plane.get("x_axis"), "plane.x_axis")
            y = vector(plane.get("y_axis"), "plane.y_axis")
            if _norm(_cross(x, y)) < 1e-9 * _norm(x) * _norm(y):
                raise bad("plane.x_axis and plane.y_axis must not be parallel")
            return {"origin": origin, "x_axis": x, "y_axis": y}
    else:
        raise bad("plane must be 'XY', 'YZ', 'ZX' or a frame dict")
    key = str(name).upper()
    if key not in PLANES:
        raise bad(f"unknown plane {name!r}", "Use 'XY', 'YZ', 'ZX', or give origin + x_axis + y_axis.")
    x, y = PLANES[key]
    return {"origin": origin, "x_axis": list(x), "y_axis": list(y)}


def plane_normal(frame: dict) -> list[float]:
    n = _cross(frame["x_axis"], frame["y_axis"])
    ln = _norm(n)
    return [c / ln for c in n]


def inner_loops2d(loops, name: str = "holes") -> list[list[dict]]:
    if loops is None:
        return []
    if not isinstance(loops, (list, tuple)):
        raise bad(f"{name} must be a list of loops (each loop is a segment list)")
    return [segments2d(loop, f"{name}[{i}]") for i, loop in enumerate(loops)]


def segments2d(segs, name: str = "segments") -> list[dict]:
    """Same segment format as `segments`, but points are [x, y] in the sketch plane."""
    def up(p):
        if not isinstance(p, (list, tuple)) or len(p) != 2:
            raise bad(f"{name}: sketch points must be [x, y]")
        return [p[0], p[1], 0.0]
    conv = []
    for sg in (segs if isinstance(segs, (list, tuple)) else []):
        if not isinstance(sg, dict):
            conv.append(sg)
            continue
        item = dict(sg)
        for key in ("start", "end", "through", "center"):
            if item.get(key) is not None:
                item[key] = up(item[key])
        item.pop("normal", None)
        conv.append(item)
    checked = segments(conv, name)
    out = []
    for sg in checked:
        o = {"type": sg["type"]}
        for key in ("start", "end", "through", "center"):
            if key in sg:
                o[key] = sg[key][:2]
        if "radius" in sg:
            o["radius"] = sg["radius"]
        out.append(o)
    return out


def _profile_points(segs) -> list:
    pts = []
    for sg in segs:
        if sg["type"] == "circle":
            continue
        pts += [sg["start"], sg["end"]] + ([sg["through"]] if sg["type"] == "arc" else [])
    return pts


def check_axis_not_crossed(points, axis_point, axis_dir, plane_normal_hint=None) -> None:
    """Reject a revolve whose profile lies on both sides of the axis (ZW3D would fail with a vague error).
    Works for 3D points (profile must be coplanar with the axis) and for 2D sketch points."""
    tol = 1e-7
    if points and len(points[0]) == 2:
        d = axis_dir
        side = [d[0] * (p[1] - axis_point[1]) - d[1] * (p[0] - axis_point[0]) for p in points]
    else:
        n = plane_normal_hint
        if n is None:
            n = polygon_normal(points) if len(points) >= 3 else None
        if n is None:
            return
        ad = _norm(axis_dir)
        if abs(_dot(axis_dir, n)) > 1e-6 * ad or abs(_dot(_sub(axis_point, points[0]), n)) > 1e-6 * max(1.0, ad):
            return  # axis not in the profile plane: crossing is not meaningful here
        w = _cross(n, axis_dir)
        side = [_dot(_sub(p, axis_point), w) for p in points]
    scale = max(1.0, max(abs(x) for x in side))
    if min(side) < -tol * scale and max(side) > tol * scale:
        raise bad("The profile crosses the rotation axis (it has points on both sides of it)",
                  "Keep the whole profile on one side of the axis, or revolve each half separately.")


def mirror_plane(plane) -> dict:
    """'XY' | 'YZ' | 'ZX', {"name":..,"origin":p}, or {"origin":p,"normal":v}. Returns {"origin","normal"}."""
    if isinstance(plane, dict) and plane.get("normal") is not None:
        return {"origin": point(plane.get("origin", [0, 0, 0]), "plane.origin"), "normal": vector(plane["normal"], "plane.normal")}
    frame = sketch_plane(plane)
    return {"origin": frame["origin"], "normal": plane_normal(frame)}


def path_segments(segs, name: str = "path") -> list[dict]:
    """A connected OPEN chain of 3D lines/arcs (each start equals the previous end). No circles."""
    if not isinstance(segs, (list, tuple)) or not segs:
        raise bad(f"{name} must be a non-empty list of line/arc segments")
    out = []
    for i, sg in enumerate(segs):
        if not isinstance(sg, dict) or sg.get("type") not in ("line", "arc"):
            raise bad(f"{name}[{i}].type must be 'line' or 'arc'")
        out.append(_one_segment(sg, f"{name}[{i}]"))
    for i in range(len(out) - 1):
        if _norm(_sub(out[i]["end"], out[i + 1]["start"])) > 1e-6:
            raise bad(f"{name}: segment {i} does not end where segment {i + 1} starts",
                      "List path segments in order; each start must equal the previous end.")
    return out


def _one_segment(sg, name):
    item = {"type": sg["type"], "start": point(sg.get("start"), f"{name}.start"), "end": point(sg.get("end"), f"{name}.end")}
    if sg["type"] == "arc":
        item["through"] = point(sg.get("through"), f"{name}.through")
        a, b, c = item["start"], item["through"], item["end"]
        if _norm(_cross(_sub(b, a), _sub(c, a))) < 1e-9:
            raise bad(f"{name}: start, through and end are collinear")
    elif _norm(_sub(item["start"], item["end"])) < 1e-9:
        raise bad(f"{name}: zero-length line")
    return item


def number(value, name: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise bad(f"{name} must be a finite number, got {value!r}")
    return float(value)


def check_expression(name: str, expression: str) -> str:
    """Reject empty expressions and expressions that mention the variable being defined (a circular reference)."""
    import re
    if not isinstance(expression, str) or not expression.strip():
        raise bad("expression must be a non-empty string")
    if re.search(r"(?<![A-Za-z0-9_])" + re.escape(name) + r"(?![A-Za-z0-9_])", expression):
        raise bad(f"expression {expression!r} refers to the variable {name!r} itself (circular reference)",
                  "Use a literal value, or reference a different variable.")
    return expression
