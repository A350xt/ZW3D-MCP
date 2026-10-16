"""Fake ZW3D plugin: same NDJSON protocol as plugin/ but with an in-memory, approximate model.

Run standalone:  python fake_zw3d.py [--port 47823]
Geometry is analytic and approximate (volumes of primitives/prisms/revolves are exact; booleans use bounding-box
overlap, exact only for axis-aligned boxes). It exists to develop and test the Python side without ZW3D.
"""

from __future__ import annotations

import argparse
import copy
import json
import math
import os
import socket
import tempfile
import threading
import time
import zlib

DENSITY = 7850.0  # kg/m^3 (steel)


class ApiError(Exception):
    def __init__(self, code, message, hint="", api_code=-1):
        self.code, self.message, self.hint, self.api_code = code, message, hint, api_code


def _sub(a, b): return [a[i] - b[i] for i in range(3)]
def _add(a, b): return [a[i] + b[i] for i in range(3)]
def _mul(a, k): return [x * k for x in a]
def _dot(a, b): return sum(a[i] * b[i] for i in range(3))
def _cross(a, b): return [a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]]
def _len(a): return math.sqrt(_dot(a, a))
def _unit(a):
    n = _len(a)
    return [x / n for x in a]


def _poly_area_normal(pts):
    n = [0.0, 0.0, 0.0]
    for i in range(len(pts)):
        n = _add(n, _cross(pts[i], pts[(i + 1) % len(pts)]))
    return _len(n) / 2, _unit(n)


def _poly_centroid(pts):
    tot, c = 0.0, [0.0, 0.0, 0.0]
    for i in range(1, len(pts) - 1):
        a = _len(_cross(_sub(pts[i], pts[0]), _sub(pts[i + 1], pts[0]))) / 2
        c = _add(c, _mul(_add(_add(pts[0], pts[i]), pts[i + 1]), a / 3))
        tot += a
    return _mul(c, 1 / tot)


def _circumcenter(a, b, c):
    ab, ac = _sub(b, a), _sub(c, a)
    n = _cross(ab, ac)
    d = 2 * _dot(n, n)
    t1 = _mul(_cross(n, ab), _dot(ac, ac))   # (|ac|^2 (n x ab) + |ab|^2 (ac x n)) / (2|n|^2)
    t2 = _mul(_cross(ac, n), _dot(ab, ab))
    return _add(a, _mul(_add(t1, t2), 1 / d))


def _overlap(a, b):
    lo = [max(a["min"][i], b["min"][i]) for i in range(3)]
    hi = [min(a["max"][i], b["max"][i]) for i in range(3)]
    if any(hi[i] <= lo[i] for i in range(3)):
        return None
    return {"min": lo, "max": hi}


def _vol(bb): return math.prod(bb["max"][i] - bb["min"][i] for i in range(3))


class State:
    def __init__(self):
        self.part = None            # active part name, None = no part
        self.file = ""
        self.saved_path = None
        self.shapes: dict[int, dict] = {}
        self.next_id = 1
        self.faces: dict[int, tuple[int, dict]] = {}
        self.edges: dict[int, tuple[int, float]] = {}

    def nid(self):
        i = self.next_id
        self.next_id += 1
        return i


class Fake:
    def __init__(self, enable_run_command=False):
        self.s = State()
        self.enable_run_command = enable_run_command
        self.methods = {
            "ping": self.ping, "new_part": self.new_part, "save_part": self.save_part,
            "export_step": self.export_step, "create_box": self.create_box,
            "create_cylinder": self.create_cylinder, "create_sphere": self.create_sphere,
            "extrude_polygon": self.extrude_polygon, "revolve_polygon": self.revolve_polygon,
            "boolean": self.boolean, "fillet_edges": self.fillet_edges, "list_shapes": self.list_shapes,
            "list_faces": self.list_faces, "list_edges": self.list_edges, "get_mass_props": self.get_mass_props,
            "get_bounding_box": self.get_bounding_box, "screenshot": self.screenshot,
            "run_command": self.run_command, "_sleep": self._sleep,
            "extrude_profile": self.extrude_profile, "revolve_profile": self.revolve_profile,
            "chamfer_edges": self.chamfer_edges, "shell_shape": self.shell_shape,
            "undo": self.undo, "redo": self.redo,
        }
        self.mutating = {"create_box", "create_cylinder", "create_sphere", "extrude_polygon", "revolve_polygon",
                         "extrude_profile", "revolve_profile", "boolean", "fillet_edges", "chamfer_edges",
                         "shell_shape"}
        self.undo_stack: list = []
        self.redo_stack: list = []

    # -- helpers ---------------------------------------------------------------
    def need_part(self):
        if self.s.part is None:
            raise ApiError("WRONG_ROOT_ENV", "Not in a part modeling environment",
                           "Call new_part first (or open a part in ZW3D).", -6)

    def shape(self, sid):
        self.need_part()
        if not isinstance(sid, int) or sid not in self.s.shapes:
            raise ApiError("INVALID_INPUT", f"shape_id {sid} does not exist", "Call list_shapes to see valid ids.", -31)
        return self.s.shapes[sid]

    def describe(self, sid):
        sh = self.s.shapes[sid]
        return {"shape_id": sid, "bounding_box": self._bbox(sh), "volume": sh["volume"], "area": sh["area"]}

    @staticmethod
    def _bbox(sh):
        mn, mx = sh["bbox"]["min"], sh["bbox"]["max"]
        return {"min": mn, "max": mx, "size": [mx[i] - mn[i] for i in range(3)]}

    def add_shape(self, kind, bbox, volume, area, edge_lens, face_boxes, combine):
        s = self.s
        sid = s.nid()
        s.shapes[sid] = {"kind": kind, "bbox": bbox, "volume": volume, "area": area, "edges": [], "faces": []}
        for ln in edge_lens:
            eid = s.nid()
            s.edges[eid] = (sid, ln)
            s.shapes[sid]["edges"].append(eid)
        for fb in face_boxes:
            fid = s.nid()
            s.faces[fid] = (sid, fb)
            s.shapes[sid]["faces"].append(fid)
        if combine == "none":
            return {**self.describe(sid), "shapes": sorted(s.shapes)}
        # merge into every overlapping existing shape (fake semantics)
        tool = s.shapes[sid]
        for oid in [i for i in s.shapes if i != sid]:
            ov = _overlap(s.shapes[oid]["bbox"], tool["bbox"])
            if ov is None:
                continue
            self._apply_bool(oid, combine, tool)
        del s.shapes[sid]
        return {"shape_id": None, "shapes": sorted(s.shapes)}

    def _apply_bool(self, base_id, op, tool):
        base = self.s.shapes[base_id]
        ov = _overlap(base["bbox"], tool["bbox"])
        ovv = _vol(ov) if ov else 0.0
        if op in ("add", "union"):
            base["volume"] = base["volume"] + tool["volume"] - ovv
            base["area"] += tool["area"]
            base["bbox"] = {"min": [min(base["bbox"]["min"][i], tool["bbox"]["min"][i]) for i in range(3)],
                            "max": [max(base["bbox"]["max"][i], tool["bbox"]["max"][i]) for i in range(3)]}
        elif op in ("remove", "subtract"):
            base["volume"] = max(base["volume"] - ovv, 0.0)
        elif op == "intersect":
            if ov is None:
                raise ApiError("MODELING_DATA_ERROR", "Intersection is empty", "The solids do not overlap.", -211)
            base["volume"] = min(ovv, base["volume"], tool["volume"])
            base["bbox"] = ov

    # -- methods ---------------------------------------------------------------
    def ping(self, p):
        return {"plugin_version": "fake-0.1.0", "zw3d_version": "fake", "active_file": self.s.file,
                "active_root": self.s.part or "", "main_thread": True}

    def new_part(self, p):
        name = p["name"]
        stem = os.path.splitext(os.path.basename(name))[0]
        if any(c in stem for c in '\\/:*?"<>|') or not stem:
            raise ApiError("INVALID_NAME", f"Invalid name {name!r}", "Avoid \\ / : * ? \" < > |", -241)
        self.s = State()
        self.s.file = name
        self.s.part = p.get("part_name") or stem
        return {"file": name, "part": self.s.part}

    def save_part(self, p):
        self.need_part()
        path = p.get("path")
        if not path and not self.s.saved_path:
            raise ApiError("INVALID_PATH", "File has never been saved", "Pass an explicit path.", -242)
        self.s.saved_path = path or self.s.saved_path
        return {"saved": True, "path": self.s.saved_path}

    def export_step(self, p):
        self.need_part()
        with open(p["path"], "w", encoding="utf-8") as f:
            f.write("ISO-10303-21;\nEND-ISO-10303-21;\n")
        return {"path": p["path"]}

    def create_box(self, p):
        self.need_part()
        c, sz = p["center"], p["size"]
        if min(sz) <= 0:
            raise ApiError("INVALID_INPUT", "Box size must be positive", "", -31)
        bb = {"min": [c[i] - sz[i] / 2 for i in range(3)], "max": [c[i] + sz[i] / 2 for i in range(3)]}
        x, y, z = sz
        faces = []
        for ax in range(3):
            for side in ("min", "max"):
                lo, hi = list(bb["min"]), list(bb["max"])
                if side == "min":
                    hi[ax] = lo[ax]
                else:
                    lo[ax] = hi[ax]
                n = [0.0, 0.0, 0.0]
                n[ax] = 1.0
                faces.append({"min": lo, "max": hi, "normal": n})
        return self.add_shape("box", bb, x * y * z, 2 * (x*y + y*z + x*z), [x]*4 + [y]*4 + [z]*4,
                              faces, p.get("combine", "none"))

    def create_cylinder(self, p):
        self.need_part()
        c, r, h = p["base_center"], p["radius"], p["height"]
        if r <= 0 or h <= 0:
            raise ApiError("INVALID_INPUT", "radius and height must be positive", "", -31)
        bb = {"min": [c[0]-r, c[1]-r, c[2]], "max": [c[0]+r, c[1]+r, c[2]+h]}
        return self.add_shape("cylinder", bb, math.pi*r*r*h, 2*math.pi*r*r + 2*math.pi*r*h,
                              [2*math.pi*r, 2*math.pi*r, h], [bb]*3, p.get("combine", "none"))

    def create_sphere(self, p):
        self.need_part()
        c, r = p["center"], p["radius"]
        if r <= 0:
            raise ApiError("INVALID_INPUT", "radius must be positive", "", -31)
        bb = {"min": [x - r for x in c], "max": [x + r for x in c]}
        return self.add_shape("sphere", bb, 4/3*math.pi*r**3, 4*math.pi*r*r, [], [bb], p.get("combine", "none"))

    def extrude_polygon(self, p):
        self.need_part()
        pts = p["points"]
        if len(pts) < 3:
            raise ApiError("INVALID_INPUT", "Need at least 3 points", "", -31)
        area, n = _poly_area_normal(pts)
        if area < 1e-9:
            raise ApiError("INVALID_INPUT", "Profile is degenerate", "", -31)
        d = _unit(p["direction"]) if p.get("direction") else n
        s0, s1 = p.get("start", 0.0), p["distance"]
        if s1 <= s0:
            raise ApiError("INVALID_INPUT", "distance must be greater than start", "", -31)
        vol = area * (s1 - s0) * abs(_dot(n, d))
        if vol < 1e-9:
            raise ApiError("CMMD_EXEC_ERROR", "Extrusion direction is parallel to the profile plane",
                           "Choose a direction that is not in the plane of the profile.", -152)
        allp = [_add(q, _mul(d, t)) for q in pts for t in (s0, s1)]
        bb = {"min": [min(q[i] for q in allp) for i in range(3)], "max": [max(q[i] for q in allp) for i in range(3)]}
        per = sum(_len(_sub(pts[i], pts[(i+1) % len(pts)])) for i in range(len(pts)))
        edges = [_len(_sub(pts[i], pts[(i+1) % len(pts)])) for i in range(len(pts))] * 2 + [s1 - s0] * len(pts)
        return self.add_shape("extrude", bb, vol, 2*area + per*(s1 - s0), edges, [bb]*(len(pts)+2), p.get("combine", "none"))

    def revolve_polygon(self, p):
        self.need_part()
        pts, a0, ax_d = p["points"], p["axis_point"], _unit(p["axis_direction"])
        sweep = p["angle"] - p.get("start_angle", 0.0)
        if not 0 < sweep <= 360:
            raise ApiError("INVALID_INPUT", "angle - start_angle must be in (0, 360]", "", -31)
        area, _ = _poly_area_normal(pts)

        def radial(q):
            r = _sub(q, a0)
            return _len(_sub(r, _mul(ax_d, _dot(r, ax_d))))

        cen = _poly_centroid(pts)
        rc = radial(cen)
        if min(radial(q) for q in pts) < 1e-9 and rc < 1e-9:
            raise ApiError("CMMD_EXEC_ERROR", "Profile lies on the axis", "", -152)
        vol = area * 2 * math.pi * rc * sweep / 360
        lo, hi = [1e300]*3, [-1e300]*3
        for q in pts:
            r = _sub(q, a0)
            axial = _dot(r, ax_d)
            ctr = _add(a0, _mul(ax_d, axial))
            rad = radial(q)
            for i in range(3):
                half = rad * math.sqrt(max(0.0, 1 - ax_d[i] ** 2))
                lo[i], hi[i] = min(lo[i], ctr[i] - half), max(hi[i], ctr[i] + half)
        bb = {"min": lo, "max": hi}
        edges = [_len(_sub(pts[i], pts[(i+1) % len(pts)])) for i in range(len(pts))]
        return self.add_shape("revolve", bb, vol, 2*area, edges, [bb]*len(pts), p.get("combine", "none"))

    @staticmethod
    def _segments_to_polygon(segs):
        """Sample lines/arcs/circles into a polygon (fake only; circles use 2048 vertices)."""
        if segs[0]["type"] == "circle":
            c, r, n = segs[0]["center"], segs[0]["radius"], _unit(segs[0]["normal"])
            t = [0, 0, 1] if abs(n[2]) < 0.9 else [1, 0, 0]
            u = _unit(_cross(n, t))
            w = _cross(n, u)
            return [_add(c, _add(_mul(u, r * math.cos(a)), _mul(w, r * math.sin(a))))
                    for a in (2 * math.pi * k / 2048 for k in range(2048))]
        pts = []
        for sg in segs:
            pts.append(sg["start"])
            if sg["type"] == "arc":
                a, b, c = sg["start"], sg["through"], sg["end"]
                ab, ac = _sub(b, a), _sub(c, a)
                cen = _circumcenter(a, b, c)
                r = _len(_sub(a, cen))
                u = _unit(_sub(a, cen))
                n = _unit(_cross(ab, ac))
                w = _cross(n, u)

                def ang(q):
                    d_ = _sub(q, cen)
                    return math.atan2(_dot(d_, w), _dot(d_, u)) % (2 * math.pi)
                sweep = ang(c)  # a -> b -> c runs counter-clockwise about n = ab x ac
                for k in range(1, 256):
                    th = sweep * k / 256
                    pts.append(_add(cen, _add(_mul(u, r * math.cos(th)), _mul(w, r * math.sin(th)))))
        return pts

    def extrude_profile(self, p):
        q = dict(p)
        q["points"] = self._segments_to_polygon(p["segments"])
        return self.extrude_polygon(q)

    def revolve_profile(self, p):
        q = dict(p)
        q["points"] = self._segments_to_polygon(p["segments"])
        return self.revolve_polygon(q)

    def chamfer_edges(self, p):
        sh = self.shape(p["shape_id"])
        d1, d2 = p["distance"], p.get("distance2") or p["distance"]
        ids = p.get("edge_ids") or sh["edges"]
        for e in ids:
            if e not in self.s.edges or self.s.edges[e][0] != p["shape_id"]:
                raise ApiError("INVALID_INPUT", f"edge {e} does not belong to shape {p['shape_id']}", "Use list_edges.", -31)
        if any(max(d1, d2) * 2 > self.s.edges[e][1] for e in ids):
            raise ApiError("CMMD_EXEC_ERROR", "Chamfer failed: distance too large", "Use a smaller distance.", -152)
        sh["volume"] = max(sh["volume"] - sum(d1 * d2 / 2 * self.s.edges[e][1] for e in ids), 0.0)
        return {**self.describe(p["shape_id"]), "edge_count": len(ids), "shapes": sorted(self.s.shapes)}

    def shell_shape(self, p):
        sh = self.shape(p["shape_id"])
        t = p["thickness"]
        bb = sh["bbox"]
        dims = [bb["max"][i] - bb["min"][i] for i in range(3)]
        if sh["kind"] != "box":
            raise ApiError("CMMD_EXEC_ERROR", "fake shell supports boxes only", "", -152)
        if any(2 * t >= d for d in dims):
            raise ApiError("CMMD_EXEC_ERROR", "Shell failed: thickness too large", "Use a thinner wall.", -152)
        inner = list(dims)
        for i in range(3):
            inner[i] -= 2 * t
        for fid in p.get("open_face_ids") or []:
            if fid not in sh["faces"]:
                raise ApiError("INVALID_INPUT", f"face {fid} does not belong to shape", "Use list_faces.", -31)
            n = self.s.faces[fid][1]["normal"]
            inner[n.index(1.0)] += t
        sh["volume"] = dims[0] * dims[1] * dims[2] - inner[0] * inner[1] * inner[2]
        return {**self.describe(p["shape_id"]), "shapes": sorted(self.s.shapes)}

    def undo(self, p):
        return self._undo_redo(p, True)

    def redo(self, p):
        return self._undo_redo(p, False)

    def _undo_redo(self, p, undo):
        self.need_part()
        src, dst = (self.undo_stack, self.redo_stack) if undo else (self.redo_stack, self.undo_stack)
        done = 0
        for _ in range(p.get("steps", 1)):
            if not src:
                break
            dst.append(copy.deepcopy(self.s))
            self.s = src.pop()
            done += 1
        if not done:
            raise ApiError("NOTHING_TO_DO", "Nothing to undo" if undo else "Nothing to redo", "")
        return {"steps_done": done, "shapes": sorted(self.s.shapes)}

    def boolean(self, p):
        base = self.shape(p["base_id"])
        op = p["operation"]
        for tid in p["tool_ids"]:
            if tid == p["base_id"]:
                raise ApiError("INVALID_INPUT", "base_id cannot also be a tool", "", -31)
            self._apply_bool(p["base_id"], op, self.shape(tid))
        if not p.get("keep_tools"):
            for tid in p["tool_ids"]:
                del self.s.shapes[tid]
        return {**self.describe(p["base_id"]), "shapes": sorted(self.s.shapes)}

    def fillet_edges(self, p):
        sh = self.shape(p["shape_id"])
        r = p["radius"]
        if r <= 0:
            raise ApiError("INVALID_INPUT", "radius must be positive", "", -31)
        ids = p.get("edge_ids") or sh["edges"]
        if not sh["edges"]:
            raise ApiError("INVALID_INPUT", "Shape has no edges", "", -31)
        for e in ids:
            if e not in self.s.edges or self.s.edges[e][0] != p["shape_id"]:
                raise ApiError("INVALID_INPUT", f"edge {e} does not belong to shape {p['shape_id']}", "Use list_edges.", -31)
        if any(r * 2 > self.s.edges[e][1] for e in ids):
            raise ApiError("CMMD_EXEC_ERROR", "Fillet failed: radius too large for the selected edges",
                           "Use a smaller radius.", -152)
        removed = sum((1 - math.pi / 4) * r * r * self.s.edges[e][1] for e in ids)
        sh["volume"] = max(sh["volume"] - removed, 0.0)
        return self.describe(p["shape_id"])

    def list_shapes(self, p):
        self.need_part()
        return {"count": len(self.s.shapes), "shapes": [self.describe(i) for i in sorted(self.s.shapes)]}

    def list_faces(self, p):
        sh = self.shape(p["shape_id"])
        return {"shape_id": p["shape_id"], "count": len(sh["faces"]),
                "faces": [self._face_json(f) for f in sh["faces"]]}

    def _face_json(self, f):
        fb = self.s.faces[f][1]
        out = {"face_id": f, "bounding_box": {k: fb[k] for k in ("min", "max")}}
        out["bounding_box"]["size"] = [fb["max"][i] - fb["min"][i] for i in range(3)]
        if "normal" in fb:
            out["surface"] = "plane"
            out["plane_normal"] = fb["normal"]
        return out

    def list_edges(self, p):
        sh = self.shape(p["shape_id"])
        return {"shape_id": p["shape_id"], "count": len(sh["edges"]), "edge_ids": list(sh["edges"]),
                "edges": [{"edge_id": e, "bounding_box": self._bbox(sh)} for e in sh["edges"]]}

    def get_mass_props(self, p):
        sh = self.shape(p["shape_id"])
        dens = p.get("density") or DENSITY
        mn, mx = sh["bbox"]["min"], sh["bbox"]["max"]
        return {"shape_id": p["shape_id"], "volume": sh["volume"], "area": sh["area"],
                "mass_kg": sh["volume"] * 1e-9 * dens, "density_kg_m3": dens, "center_of_gravity": [(mn[i] + mx[i]) / 2 for i in range(3)]}

    def get_bounding_box(self, p):
        return {"shape_id": p["shape_id"], **self._bbox(self.shape(p["shape_id"]))}

    def screenshot(self, p):
        self.need_part()
        w, h = int(p.get("width", 800)), int(p.get("height", 600))
        d = os.path.join(tempfile.gettempdir(), "zw3d_mcp")
        os.makedirs(d, exist_ok=True)
        path = os.path.join(d, f"shot_{int(time.time() * 1000)}.png")
        with open(path, "wb") as f:
            f.write(_png(w, h))
        return {"path": path, "width": w, "height": h}

    def run_command(self, p):
        if not self.enable_run_command:
            raise ApiError("DISABLED", "run_command is disabled", "Set ZW3D_MCP_ENABLE_RUN_COMMAND=1 before starting ZW3D.")
        return {"sent": p["command"]}

    def _sleep(self, p):
        time.sleep(p["ms"] / 1000)
        return {}


def _png(w, h):
    def chunk(t, d):
        c = struct_pack(len(d)) + t + d
        return c + struct_pack(zlib.crc32(t + d))
    raw = b"".join(b"\x00" + b"\xd0\xd8\xe8" * w for _ in range(h))
    ihdr = struct_pack(w) + struct_pack(h) + bytes([8, 2, 0, 0, 0])
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")


def struct_pack(n):
    return n.to_bytes(4, "big")


class FakeServer:
    """Single-client NDJSON server, mirrors the plugin's transport rules."""

    def __init__(self, port=0, enable_run_command=False):
        self.fake = Fake(enable_run_command)
        self.lsock = socket.socket()
        self.lsock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.lsock.bind(("127.0.0.1", port))
        self.lsock.listen(4)
        self.port = self.lsock.getsockname()[1]
        self._stop = threading.Event()
        self._conns = []
        self._thread = threading.Thread(target=self._serve, daemon=True)

    def start(self):
        self._thread.start()
        return self

    def stop(self):
        self._stop.set()
        for c in [self.lsock, *self._conns]:
            try:
                c.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                c.close()
            except OSError:
                pass

    def _serve(self):
        while not self._stop.is_set():
            try:
                conn, _ = self.lsock.accept()
            except OSError:
                return
            self._conns.append(conn)
            threading.Thread(target=self._client, args=(conn,), daemon=True).start()

    def _client(self, conn):
        buf = b""
        try:
            while not self._stop.is_set():
                data = conn.recv(65536)
                if not data:
                    return
                buf += data
                while b"\n" in buf:
                    line, _, buf = buf.partition(b"\n")
                    if line.strip():
                        conn.sendall(self.handle(line) + b"\n")
        except OSError:
            pass
        finally:
            conn.close()

    def handle(self, line: bytes) -> bytes:
        rid = None
        try:
            req = json.loads(line.decode("utf-8"))
            rid = req.get("id")
            method = req.get("method")
            fn = self.fake.methods.get(method)
            if fn is None:
                raise ApiError("UNKNOWN_METHOD", f"Unknown method {method!r}", "See the tool list.")
            timeout = req.get("timeout_ms", 60000)
            params = req.get("params") or {}
            if method == "_sleep" and params.get("ms", 0) > timeout:
                time.sleep(timeout / 1000)
                raise ApiError("TIMEOUT", f"Request exceeded {timeout} ms", "ZW3D may be busy.")
            snap = copy.deepcopy(self.fake.s) if method in self.fake.mutating else None
            result = fn(params)
            if snap is not None:
                self.fake.undo_stack.append(snap)
                self.fake.redo_stack.clear()
            return json.dumps({"id": rid, "ok": True, "result": result}).encode()
        except ApiError as e:
            err = {"code": e.code, "api_code": e.api_code, "message": e.message, "hint": e.hint}
        except (KeyError, TypeError, ValueError) as e:
            err = {"code": "INVALID_INPUT", "api_code": -31, "message": f"Bad request: {e!r}", "hint": ""}
        return json.dumps({"id": rid, "ok": False, "error": err}).encode()


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=int(os.environ.get("ZW3D_MCP_PORT", 47823)))
    ap.add_argument("--enable-run-command", action="store_true")
    a = ap.parse_args()
    srv = FakeServer(a.port, a.enable_run_command).start()
    print(f"fake ZW3D listening on 127.0.0.1:{srv.port}", flush=True)
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        srv.stop()
