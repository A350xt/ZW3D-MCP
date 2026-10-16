"""Manual smoke test against the REAL plugin. Run on the machine where ZW3D 2025+ is running with zw3dmcp.dll loaded.

    python tools/smoke_test.py            # full modeling scenario (creates a new file "mcp_smoke")
    python tools/smoke_test.py --cases    # also replay server/tests/protocol_cases.json
    python tools/smoke_test.py --no-fillet  # skip risky steps

Paste the complete output back to the developer: it records which API calls worked on your ZW3D build.
"""

from __future__ import annotations

import argparse
import json
import math
import pathlib
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "server"))
from zw3d_mcp.bridge import Bridge, ZW3DError  # noqa: E402

failures = 0


def step(b: Bridge, label: str, method: str, params: dict | None = None, expect_error: str | None = None):
    global failures
    t0 = time.perf_counter()
    try:
        r = b.call(method, params or {}, timeout_ms=120_000)
        dt = (time.perf_counter() - t0) * 1000
        print(f"[ OK ] {label:<34} {dt:7.0f} ms  {json.dumps(r, ensure_ascii=False)[:300]}")
        return r
    except ZW3DError as e:
        good = expect_error is not None and e.code == expect_error
        failures += 0 if good else 1
        print(f"[{' OK ' if good else 'FAIL'}] {label:<34} {e}")
        return None


def approx(label: str, got, want, tol=1e-3):
    global failures
    ok = got is not None and abs(got - want) <= tol * max(abs(want), 1)
    failures += 0 if ok else 1
    print(f"[{' OK ' if ok else 'FAIL'}] check {label}: got {got}, expected {want:.4f}")


def main() -> int:
    global failures
    ap = argparse.ArgumentParser()
    ap.add_argument("--cases", action="store_true")
    ap.add_argument("--no-fillet", action="store_true")
    ap.add_argument("--port", type=int, default=None)
    a = ap.parse_args()
    b = Bridge(port=a.port)

    r = step(b, "ping", "ping")
    if r is None:
        return 1
    print(f"       handlers run on ZW3D main thread: {r.get('main_thread')}  "
          f"(handler={r.get('handler_thread_id')} init={r.get('init_thread_id')})")

    step(b, "new_part (Chinese name)", "new_part", {"name": f"mcp_smoke_{int(time.time())}_底板"})
    step(b, "ping after new_part", "ping")

    plate = step(b, "create_box 100x60x20", "create_box", {"center": [50, 30, 10], "size": [100, 60, 20], "combine": "none"})
    if plate:
        approx("plate volume", plate["volume"], 120000)
    cyl = step(b, "create_cylinder d30 h40", "create_cylinder",
               {"base_center": [50, 30, 0], "radius": 15, "height": 40, "combine": "none"})
    if cyl:
        approx("cylinder volume", cyl["volume"], math.pi * 15 ** 2 * 40)
    if plate and cyl:
        u = step(b, "boolean add", "boolean", {"operation": "add", "base_id": plate["shape_id"],
                                                 "tool_ids": [cyl["shape_id"]], "keep_tools": False})
        if u and u.get("volume") is not None:
            approx("union volume", u["volume"], 120000 + math.pi * 15 ** 2 * 20)

    sq = [[200, 0, 0], [210, 0, 0], [210, 10, 0], [200, 10, 0]]
    ex = step(b, "extrude_polygon 10x10x5", "extrude_polygon",
              {"points": sq, "distance": 5, "start": 0, "direction": [0, 0, 1], "draft_angle": 0, "combine": "none"})
    if ex:
        approx("prism volume", ex["volume"], 500)
    rv = step(b, "revolve_polygon (vendor example)", "revolve_polygon", {
        "points": [[10, 0, 0], [20, 0, 0], [20, 10, 0], [10, 10, 0]],
        "axis_point": [0, 0, 0], "axis_direction": [0, 1, 0], "angle": 180, "start_angle": 0, "combine": "none"})
    if rv:
        approx("half hollow cylinder", rv["volume"], math.pi * (400 - 100) * 10 / 2)

    shapes = step(b, "list_shapes", "list_shapes")
    if plate:
        step(b, "get_mass_props", "get_mass_props", {"shape_id": plate["shape_id"]})
        step(b, "get_bounding_box", "get_bounding_box", {"shape_id": plate["shape_id"]})
        faces = step(b, "list_faces", "list_faces", {"shape_id": plate["shape_id"]})
        edges = step(b, "list_edges", "list_edges", {"shape_id": plate["shape_id"]})
        if edges and not a.no_fillet:
            step(b, "fillet_edges r=1 (first edge)", "fillet_edges",
                 {"shape_id": plate["shape_id"], "radius": 1, "edge_ids": edges["edge_ids"][:1]})

    # ---- round profiles, chamfer, shell, undo (batch 2)
    cp = step(b, "extrude_profile circle r10 h30", "extrude_profile", {
        "segments": [{"type": "circle", "center": [400, 0, 0], "radius": 10, "normal": [0, 0, 1]}],
        "distance": 30, "start": 0, "direction": [0, 0, 1], "draft_angle": 0, "combine": "none"})
    if cp:
        approx("circle extrude volume", cp["volume"], math.pi * 100 * 30)
    slot = step(b, "extrude_profile slot (2 lines + 2 arcs)", "extrude_profile", {
        "segments": [
            {"type": "line", "start": [500, 0, 0], "end": [540, 0, 0]},
            {"type": "arc", "start": [540, 0, 0], "through": [550, 10, 0], "end": [540, 20, 0]},
            {"type": "line", "start": [540, 20, 0], "end": [500, 20, 0]},
            {"type": "arc", "start": [500, 20, 0], "through": [490, 10, 0], "end": [500, 0, 0]}],
        "distance": 5, "start": 0, "direction": [0, 0, 1], "draft_angle": 0, "combine": "none"})
    if slot:
        approx("slot volume", slot["volume"], (40 * 20 + math.pi * 100) * 5)
    cb = step(b, "create_box 50x50x20 (for chamfer)", "create_box",
              {"center": [700, 0, 10], "size": [50, 50, 20], "combine": "none"})
    if cb and not a.no_fillet:
        step(b, "chamfer_edges d=2 (all edges)", "chamfer_edges", {"shape_id": cb["shape_id"], "distance": 2})
    sb = step(b, "create_box 60x40x30 (for shell)", "create_box",
              {"center": [900, 0, 15], "size": [60, 40, 30], "combine": "none"})
    if sb:
        fl = step(b, "list_faces (surface info)", "list_faces", {"shape_id": sb["shape_id"]})
        if fl:
            tops = [f for f in fl["faces"] if f.get("surface") == "plane"
                    and abs(abs(f["plane_normal"][2]) - 1) < 1e-6 and abs(f["bounding_box"]["min"][2] - 30) < 1e-6]
            print(f"       top-face candidates: {[t['face_id'] for t in tops]}")
            if tops:
                sh = step(b, "shell_shape t=2 (top open)", "shell_shape",
                          {"shape_id": sb["shape_id"], "thickness": 2, "open_face_ids": [tops[0]["face_id"]]})
                if sh and sh.get("volume") is not None:
                    approx("shelled volume", sh["volume"], 60 * 40 * 30 - 56 * 36 * 28)
        ed = step(b, "list_edges (bbox + curve)", "list_edges", {"shape_id": sb["shape_id"]})
        if ed:
            have = sum(1 for e in ed["edges"] if "bounding_box" in e)
            print(f"       edges with bounding_box: {have}/{ed['count']}; first: {json.dumps(ed['edges'][0], ensure_ascii=False)[:200]}")
            failures += 0 if have == ed["count"] else 1
    # undo granularity probe: one extrude_profile call = N curves + curve list + extrude. Does ONE undo remove the solid?
    pr = step(b, "undo probe: create_box", "create_box", {"center": [1100, 0, 5], "size": [10, 10, 10], "combine": "none"})
    px = step(b, "undo probe: extrude_polygon", "extrude_polygon", {"points": [[1200, 0, 0], [1210, 0, 0], [1210, 10, 0], [1200, 10, 0]],
              "distance": 5, "start": 0, "direction": [0, 0, 1], "draft_angle": 0, "combine": "none"})
    if px:
        u1 = step(b, "undo probe: undo 1", "undo", {"steps": 1})
        if u1:
            gone = px["shape_id"] not in u1["shapes"]
            print(f"       one undo removed the whole extrude solid: {gone}")
            step(b, "undo probe: redo 1", "redo", {"steps": 1})
    before = step(b, "list_shapes before undo", "list_shapes")
    u = step(b, "undo 1", "undo", {"steps": 1})
    r = step(b, "redo 1", "redo", {"steps": 1})
    if before and u and r:
        same = sorted(r["shapes"]) == sorted(s_["shape_id"] for s_ in before["shapes"])
        print(f"       shapes after undo+redo match: {same}")
        failures += 0 if same else 1

    # ---- batch 3: real sketches, atomic undo, failure cleanup, round-edge boxes
    rect = [{"type": "line", "start": [0, 0], "end": [40, 0]}, {"type": "line", "start": [40, 0], "end": [40, 20]},
            {"type": "line", "start": [40, 20], "end": [0, 20]}, {"type": "line", "start": [0, 20], "end": [0, 0]}]
    sk = step(b, "sketch_extrude rect 40x20 d=10 (XY, z=300)", "sketch_extrude", {
        "plane": {"origin": [1300, 0, 300], "x_axis": [1, 0, 0], "y_axis": [0, 1, 0]}, "segments": rect,
        "distance": 10, "start": 0, "direction": None, "draft_angle": 0, "dimensions": True, "combine": "none"})
    if sk:
        approx("sketch rect volume", sk["volume"], 40 * 20 * 10)
        print(f"       sketch_id={sk.get('sketch_id')} dimensions_added={sk.get('dimensions_added')} "
              f"dimensions_failed={sk.get('dimensions_failed')}  bbox={sk['bounding_box']['min']}..{sk['bounding_box']['max']}")
    skc = step(b, "sketch_extrude circle R6 d=8", "sketch_extrude", {
        "plane": {"origin": [1500, 0, 0], "x_axis": [1, 0, 0], "y_axis": [0, 1, 0]},
        "segments": [{"type": "circle", "center": [0, 0], "radius": 6}], "distance": 8, "start": 0,
        "direction": None, "draft_angle": 0, "dimensions": True, "combine": "none"})
    if skc:
        approx("sketch circle volume", skc["volume"], math.pi * 36 * 8)
        el = step(b, "list_edges (circle bbox must be 12x12)", "list_edges", {"shape_id": skc["shape_id"]})
        if el:
            circ = [e for e in el["edges"] if e.get("curve", {}).get("type") == "circle"]
            ok = bool(circ) and all(abs(e["bounding_box"]["size"][0] - 12) < 1e-3 and abs(e["bounding_box"]["size"][1] - 12) < 1e-3 for e in circ)
            print(f"       circle edges: {len(circ)}, boxes exact: {ok}  e.g. {circ[0]['bounding_box']['size'] if circ else None}")
            failures += 0 if ok else 1
    sr = step(b, "sketch_revolve rect around sketch Y", "sketch_revolve", {
        "plane": {"origin": [1700, 0, 0], "x_axis": [1, 0, 0], "y_axis": [0, 1, 0]},
        "segments": [{"type": "line", "start": [10, 0], "end": [20, 0]}, {"type": "line", "start": [20, 0], "end": [20, 10]},
                     {"type": "line", "start": [20, 10], "end": [10, 10]}, {"type": "line", "start": [10, 10], "end": [10, 0]}],
        "axis_point": [0, 0], "axis_direction": [0, 1], "angle": 360, "start_angle": 0, "dimensions": False, "combine": "none"})
    if sr:
        approx("sketch revolve volume", sr["volume"], math.pi * (400 - 100) * 10)

    # one tool call == one undo step
    ua = step(b, "undo test: box A", "create_box", {"center": [2000, 0, 5], "size": [10, 10, 10], "combine": "none"})
    ub = step(b, "undo test: polygon extrude B", "extrude_polygon", {
        "points": [[2100, 0, 0], [2110, 0, 0], [2110, 10, 0], [2100, 10, 0]], "distance": 5, "start": 0,
        "direction": [0, 0, 1], "draft_angle": 0, "combine": "none"})
    if ua and ub:
        r1 = step(b, "undo #1 (must remove exactly B)", "undo", {"steps": 1})
        r2 = step(b, "undo #2 (must remove exactly A)", "undo", {"steps": 1})
        ok = bool(r1 and r2) and r1["removed_shape_ids"] == [ub["shape_id"]] and r2["removed_shape_ids"] == [ua["shape_id"]]
        print(f"       undo is one step per tool call: {ok}")
        failures += 0 if ok else 1
        step(b, "redo x2", "redo", {"steps": 2})

    # failed operations must leave nothing behind
    fa = step(b, "cleanup test: box F", "create_box", {"center": [2300, 0, 5], "size": [10, 10, 10], "combine": "none"})
    if fa:
        step(b, "failing fillet r=500 (expect error)", "fillet_edges", {"shape_id": fa["shape_id"], "radius": 500}, expect_error="CMMD_EXEC_ERROR")
        try:  # bypasses the client-side axis check on purpose
            res = b.call("revolve_polygon", {"points": [[2400, 0, 0], [2410, 0, 0], [2410, 10, 0], [2400, 10, 0]],
                                             "axis_point": [2405, 0, 0], "axis_direction": [0, 1, 0],
                                             "angle": 360, "start_angle": 0, "combine": "none"})
            print("       (axis-crossing revolve was accepted by ZW3D; undoing it)")
            b.call("undo", {"steps": 1})
        except ZW3DError as e:
            print(f"       axis-crossing revolve rejected by ZW3D: [{e.code}] {e.message[:80]}")
        r = step(b, "undo after failures (must remove only box F)", "undo", {"steps": 1})
        ok = bool(r) and r["removed_shape_ids"] == [fa["shape_id"]]
        print(f"       failed operations left no undo steps behind: {ok}")
        failures += 0 if ok else 1
        step(b, "redo", "redo", {"steps": 1})

    # ---- batch 4: hole, pattern, mirror, draft, sweep, loft, variables, sketch dimension edit, inner hole
    pl = step(b, "plate for hole tests", "create_box", {"center": [3000, 0, 10], "size": [100, 60, 20], "combine": "none"})
    if pl:
        fl = step(b, "list_faces (find top face)", "list_faces", {"shape_id": pl["shape_id"]})
        tops = [f for f in (fl or {"faces": []})["faces"] if f.get("surface") == "plane"
                and abs(abs(f["plane_normal"][2]) - 1) < 1e-6 and abs(f["bounding_box"]["min"][2] - 20) < 1e-6]
        if tops:
            h = step(b, "create_hole d10 through", "create_hole", {"face_id": tops[0]["face_id"], "points": [[3000, 0, 20]],
                     "diameter": 10, "through": True, "hole_type": "simple"})
            if h:
                vol = [d for d in h["shape_details"] if d["shape_id"] == pl["shape_id"]]
                if vol:
                    approx("plate with hole volume", vol[0]["volume"], 120000 - math.pi * 25 * 20)
                pf = step(b, "pattern_linear the hole x3", "pattern_feature", {"kind": "linear", "count": 3, "spacing": 20,
                          "direction": [1, 0, 0], "feature_ids": [h["feature_id"]], "combine": "none"})
                if pf:
                    vol = [d for d in pf["shape_details"] if d["shape_id"] == pl["shape_id"]]
                    if vol:
                        approx("plate with 3 holes volume", vol[0]["volume"], 120000 - 3 * math.pi * 25 * 20)
        else:
            print("       (no top face found, hole tests skipped)")
            failures += 1
    cyl4 = step(b, "cylinder for mirror test", "create_cylinder", {"base_center": [3500, 0, 0], "radius": 5, "height": 10, "combine": "none"})
    if cyl4:
        mr = step(b, "mirror across YZ plane at x=3400", "mirror_shape", {"shape_id": cyl4["shape_id"], "combine": "none",
                  "plane": {"origin": [3400, 0, 0], "normal": [1, 0, 0]}})
        if mr and mr.get("added_shape_ids"):
            bb = step(b, "mirrored copy bbox (expect x 3295..3305)", "get_bounding_box", {"shape_id": mr["added_shape_ids"][0]})
            if bb:
                ok = abs(bb["min"][0] - 3295) < 1e-3 and abs(bb["max"][0] - 3305) < 1e-3
                failures += 0 if ok else 1
                print(f"       mirror position correct: {ok}")
        elif mr:
            print("       mirror reported no new shape")
            failures += 1
    dbx = step(b, "box for draft test", "create_box", {"center": [3700, 0, 15], "size": [40, 40, 30], "combine": "none"})
    if dbx:
        fl = step(b, "list_faces (draft box)", "list_faces", {"shape_id": dbx["shape_id"]})
        if fl:
            sides = [f["face_id"] for f in fl["faces"] if f.get("surface") == "plane" and abs(f["plane_normal"][2]) < 1e-6]
            bottom = [f["face_id"] for f in fl["faces"] if f.get("surface") == "plane" and abs(f["plane_normal"][2]) > 0.9
                      and abs(f["bounding_box"]["min"][2]) < 1e-6]
            if sides and bottom:
                dr = step(b, "draft_faces 3 deg (bottom neutral)", "draft_faces", {"face_ids": sides, "neutral_face_ids": bottom,
                          "angle": 3, "pull_direction": [0, 0, 1]})
                if dr:
                    vol = [d for d in dr["shape_details"] if d["shape_id"] == dbx["shape_id"]]
                    a2 = 40 - 2 * 30 * math.tan(math.radians(3))
                    want = 30 / 3 * (1600 + a2 * a2 + 40 * a2)
                    if vol:
                        approx("drafted box volume", vol[0]["volume"], want, tol=5e-3)
    sw = step(b, "sweep circle r5 along a 60 mm line", "sweep", {
        "profile": [{"type": "circle", "center": [4000, 0, 0], "radius": 5, "normal": [0, 0, 1]}],
        "path": [{"type": "line", "start": [4000, 0, 0], "end": [4000, 0, 60]}], "combine": "none"})
    if sw:
        approx("sweep volume", sw["volume"], math.pi * 25 * 60)

    def square(cx, z, half):
        pts = [[cx - half, -half, z], [cx + half, -half, z], [cx + half, half, z], [cx - half, half, z]]
        return [{"type": "line", "start": pts[i], "end": pts[(i + 1) % 4]} for i in range(4)]
    lf = step(b, "loft square 20 -> 10 over 40 mm", "loft", {"sections": [square(4300, 0, 10), square(4300, 40, 5)], "combine": "none"})
    if lf:
        approx("loft volume", lf["volume"], 40 / 3 * (400 + 100 + 200), tol=5e-3)

    step(b, "list_variables", "list_variables")
    step(b, "add_variable mcp_wall = 2.5 mm", "add_variable", {"name": "mcp_wall", "kind": "distance", "value": 2.5})
    sv = step(b, "set_variable mcp_wall = 3", "set_variable", {"name": "mcp_wall", "value": 3})
    if sv:
        print(f"       variable now: {sv['variable']}")
    # regression checks for the problems found in live use: undo after set_variable, state after a failed set_variable
    if sv:
        n_before = len(step(b, "list_shapes (before undo of set_variable)", "list_shapes")["shapes"])
        u = step(b, "undo the set_variable (must not empty the part)", "undo", {"steps": 1})
        if u:
            n_after = len(u["shapes"])
            ok = n_after == n_before
            print(f"       shapes before/after undo: {n_before}/{n_after}  part intact: {ok}")
            failures += 0 if ok else 1
            step(b, "redo", "redo", {"steps": 1})
    try:  # self-referencing expression, sent raw to bypass the client-side check
        b.call("set_variable", {"name": "mcp_wall", "expression": "mcp_wall*2"})
        print("       (ZW3D accepted the circular expression)")
    except ZW3DError as e:
        print(f"       circular expression rejected by ZW3D: [{e.code}] {e.message[:70]}")
    ok1 = step(b, "box right after the failed set_variable", "create_box", {"center": [6000, 0, 5], "size": [10, 10, 10], "combine": "none"})
    if ok1:
        step(b, "boolean right after (state must be healthy)", "create_box", {"center": [6005, 0, 5], "size": [10, 10, 10], "combine": "none"})
    sc = step(b, "sketch circle r10 d=10 (for dimension edit)", "sketch_extrude", {
        "plane": {"origin": [4600, 0, 0], "x_axis": [1, 0, 0], "y_axis": [0, 1, 0]},
        "segments": [{"type": "circle", "center": [0, 0], "radius": 10}], "distance": 10, "start": 0,
        "direction": None, "draft_angle": 0, "dimensions": True, "combine": "none"})
    if sc:
        lv = step(b, "list_sketch_variables", "list_sketch_variables", {"sketch_id": sc["sketch_id"]})
        if lv and lv["variables"]:
            nm = lv["variables"][0]["name"]
            val = lv["variables"][0].get("value")
            print(f"       first sketch variable: {nm} = {val}")
            if val:
                ed = step(b, f"set_sketch_variable {nm} x2 (volume should grow x4 if it is the diameter)", "set_sketch_variable",
                          {"sketch_id": sc["sketch_id"], "name": nm, "value": val * 2})
                if ed:
                    vol = [d for d in ed["shape_details"] if d["shape_id"] == sc["shape_id"]]
                    if vol:
                        print(f"       volume {sc['volume']:.1f} -> {vol[0]['volume']:.1f} (x{vol[0]['volume'] / sc['volume']:.2f})")
        else:
            print("       (sketch has no variables: dimensions were not created)")
            failures += 1
    ih = step(b, "sketch_extrude 100x60 with inner circle r10", "sketch_extrude", {
        "plane": {"origin": [5000, 0, 0], "x_axis": [1, 0, 0], "y_axis": [0, 1, 0]},
        "segments": [{"type": "line", "start": [0, 0], "end": [100, 0]}, {"type": "line", "start": [100, 0], "end": [100, 60]},
                     {"type": "line", "start": [100, 60], "end": [0, 60]}, {"type": "line", "start": [0, 60], "end": [0, 0]}],
        "inner_loops": [[{"type": "circle", "center": [50, 30], "radius": 10}]], "distance": 10, "start": 0,
        "direction": None, "draft_angle": 0, "dimensions": False, "combine": "none"})
    if ih:
        approx("plate with inner hole volume", ih["volume"], (6000 - math.pi * 100) * 10)
    step(b, "list_features", "list_features")

    shot = step(b, "screenshot", "screenshot", {"width": 800, "height": 600, "zoom_all": True})
    if shot:
        p = pathlib.Path(shot["path"])
        print(f"       png exists={p.exists()} size={p.stat().st_size if p.exists() else 0}")
    step(b, "error path: bad shape id", "get_bounding_box", {"shape_id": 987654}, expect_error="INVALID_INPUT")
    step(b, "error path: negative box", "create_box", {"center": [0, 0, 0], "size": [-1, 1, 1], "combine": "none"},
         expect_error="INVALID_INPUT")
    step(b, "run_command (expect DISABLED)", "run_command", {"command": "noop"}, expect_error="DISABLED")

    out = pathlib.Path.home() / "mcp_smoke_output.step"
    step(b, "export_step", "export_step", {"path": str(out)})
    step(b, "save_part (Save As, Chinese path)", "save_part",
         {"path": str(pathlib.Path.home() / "mcp_smoke_测试.Z3PRT")})

    # ---- assembly and drawing (these switch the active file, so they run last)
    saved = str(pathlib.Path.home() / "mcp_smoke_测试.Z3PRT")
    step(b, "new_assembly", "new_assembly", {"name": f"mcp_smoke_asm_{int(time.time())}"})
    ic = step(b, "insert_component (the saved smoke part)", "insert_component", {"file": saved, "position": [0, 0, 0], "anchor": True})
    lc = step(b, "list_components", "list_components")
    if ic and lc:
        ok = ic["component_id"] in lc["component_ids"]
        print(f"       inserted component listed: {ok}")
        failures += 0 if ok else 1
    step(b, "new_drawing (standard views of the saved part)", "new_drawing", {"name": f"mcp_smoke_dwg_{int(time.time())}", "part_file": saved})

    if a.cases:
        cases = json.loads((ROOT / "server/tests/protocol_cases.json").read_text(encoding="utf-8"))
        for c in cases:
            for s in c["steps"]:
                try:
                    res = {"ok": True, "result": b.call(s["method"], s.get("params", {}))}
                except ZW3DError as e:
                    res = {"ok": False, "error": {"code": e.code}}
                ok = ("expect_ok" not in s or res["ok"] == s["expect_ok"]) and \
                     ("expect_error_code" not in s or res.get("error", {}).get("code") == s["expect_error_code"])
                failures += 0 if ok else 1
                print(f"[{' OK ' if ok else 'FAIL'}] case {c['name']}: {s['method']}")

    print(f"\n{'ALL STEPS PASSED' if failures == 0 else str(failures) + ' FAILURE(S)'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
