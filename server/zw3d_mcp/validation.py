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
