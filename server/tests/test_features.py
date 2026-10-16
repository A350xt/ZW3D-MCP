"""Hole, pattern, mirror, draft, sweep, loft, variables, inner-loop profiles, assembly and drawing (against the fake)."""

import json
import math
import os
import subprocess
import sys

import pytest

from zw3d_mcp import server
from zw3d_mcp.bridge import ZW3DError


def approx(a, b, rel=1e-6):
    return a == pytest.approx(b, rel=rel)


def _plate(part):
    b = server.create_box(corner1=[0, 0, 0], corner2=[100, 60, 20])
    faces = server.list_faces(b["shape_id"])["faces"]
    top = [f for f in faces if f["plane_normal"] == [0.0, 0.0, 1.0] and f["bounding_box"]["min"][2] == 20][0]
    return b["shape_id"], top["face_id"], faces


# ------------------------------------------------------------------ hole
def test_hole_through_and_blind(part):
    sid, top, _ = _plate(part)
    r = server.create_hole(top, [[50, 30, 20]], 10, through=True)
    assert approx(r["shape_details"][0]["volume"], 120000 - math.pi * 25 * 20) and r["feature_id"]
    r = server.create_hole(top, [[20, 30, 20], [80, 30, 20]], 4, depth=5)
    assert approx(r["shape_details"][0]["volume"], 120000 - math.pi * 25 * 20 - 2 * math.pi * 4 * 5)


def test_hole_validation_and_errors(part):
    sid, top, _ = _plate(part)
    with pytest.raises(ZW3DError):
        server.create_hole(top, [[50, 30, 20]], 10)                       # blind without depth
    with pytest.raises(ZW3DError):
        server.create_hole(top, [], 10, through=True)
    with pytest.raises(ZW3DError) as e:
        server.create_hole(top, [[500, 30, 20]], 10, through=True)        # outside the face
    assert e.value.code == "CMMD_EXEC_ERROR"
    with pytest.raises(ZW3DError) as e:
        server.create_hole(99999, [[0, 0, 0]], 5, through=True)
    assert e.value.code == "INVALID_INPUT"


# ------------------------------------------------------------------ pattern / mirror
def test_pattern_linear_circular_and_targets(part):
    s = server.create_cylinder([0, 0, 0], 5, 10)["shape_id"]
    r = server.pattern_linear(3, 50, [1, 0, 0], shape_id=s)
    assert len(r["added_shape_ids"]) == 2
    r = server.pattern_linear(2, 30, [0, 1, 0], shape_id=s, count2=2, spacing2=30, direction2=[0, 0, 1])
    assert len(r["added_shape_ids"]) == 3                                 # 2x2 grid minus the original
    c = server.create_box(center=[100, 0, 0], size=[10, 10, 10])["shape_id"]
    r = server.pattern_circular(4, [0, 0, 0], [0, 0, 1], shape_id=c)
    assert len(r["added_shape_ids"]) == 3
    for bad in (dict(), dict(shape_id=s, feature_ids=[1])):
        with pytest.raises(ZW3DError):
            server.pattern_linear(3, 50, [1, 0, 0], **bad)
    with pytest.raises(ZW3DError):
        server.pattern_linear(1, 50, [1, 0, 0], shape_id=s)
    with pytest.raises(ZW3DError):
        server.pattern_linear(3, 50, [1, 0, 0], shape_id=s, count2=2)    # second direction incomplete


def test_pattern_a_hole_feature(part):
    sid, top, _ = _plate(part)
    h = server.create_hole(top, [[10, 10, 20]], 6, through=True)
    one = h["shape_details"][0]["volume"]
    r = server.pattern_linear(3, 20, [1, 0, 0], feature_ids=[h["feature_id"]])
    assert approx(r["shape_details"][0]["volume"], one - 2 * math.pi * 9 * 20)
    with pytest.raises(ZW3DError):
        server.pattern_linear(3, 20, [1, 0, 0], feature_ids=[424242])


def test_mirror_shape(part):
    b = server.create_box(corner1=[10, 0, 0], corner2=[30, 10, 10])["shape_id"]
    r = server.mirror("YZ", shape_id=b)
    bb = server.get_bounding_box(r["added_shape_ids"][0])
    assert bb["min"][0] == pytest.approx(-30) and bb["max"][0] == pytest.approx(-10)
    with pytest.raises(ZW3DError):
        server.mirror("QQ", shape_id=b)
    with pytest.raises(ZW3DError):
        server.mirror("YZ")


# ------------------------------------------------------------------ draft / sweep / loft
def test_draft(part):
    sid, top, faces = _plate(part)
    sides = [f["face_id"] for f in faces if f["plane_normal"][2] == 0.0]
    assert server.draft_faces(sides, [top], 3, [0, 0, 1])["feature_id"]
    with pytest.raises(ZW3DError):
        server.draft_faces(sides, [top], 95, [0, 0, 1])
    with pytest.raises(ZW3DError) as e:
        server.draft_faces(sides, sides[:1], 3, [0, 0, 1])               # same face drafted and neutral
    assert e.value.code == "INVALID_INPUT"


def test_sweep_pipe(part):
    circle = [{"type": "circle", "center": [0, 0, 0], "radius": 5, "normal": [0, 0, 1]}]
    r = server.sweep(circle, [{"type": "line", "start": [0, 0, 0], "end": [0, 0, 50]}])
    assert approx(r["volume"], math.pi * 25 * 50, rel=1e-3)
    with pytest.raises(ZW3DError) as e:                                  # path in the profile plane
        server.sweep(circle, [{"type": "line", "start": [0, 0, 0], "end": [50, 0, 0]}])
    assert e.value.code == "CMMD_EXEC_ERROR"
    with pytest.raises(ZW3DError):                                       # disconnected path
        server.sweep(circle, [{"type": "line", "start": [0, 0, 0], "end": [0, 0, 10]},
                              {"type": "line", "start": [5, 5, 5], "end": [0, 0, 50]}])


def test_loft_frustum(part):
    def sq(h, a):
        return [{"type": "line", "start": [-a, -a, h], "end": [a, -a, h]}, {"type": "line", "start": [a, -a, h], "end": [a, a, h]},
                {"type": "line", "start": [a, a, h], "end": [-a, a, h]}, {"type": "line", "start": [-a, a, h], "end": [-a, -a, h]}]
    r = server.loft([sq(0, 5), sq(50, 10)])
    assert approx(r["volume"], 50 / 3 * (100 + 400 + 200), rel=1e-6)
    with pytest.raises(ZW3DError):
        server.loft([sq(0, 5)])


# ------------------------------------------------------------------ variables / parametric edits
def test_variables_roundtrip(part):
    server.add_variable("wall", 2.5, kind="distance")
    assert server.list_variables()["count"] == 1
    assert server.set_variable("wall", 3)["variable"]["value"] == 3
    r = server.set_variable("wall", expression="wall_base*2")
    assert r["variable"]["expression"] == "wall_base*2" and "value" not in r["variable"]
    with pytest.raises(ZW3DError) as e:
        server.set_variable("nope", 1)
    assert e.value.code == "VARIABLE_NOT_FOUND" and e.value.api_code is None
    with pytest.raises(ZW3DError):
        server.set_variable("wall")
    with pytest.raises(ZW3DError):
        server.set_variable("wall", 1, expression="2")
    with pytest.raises(ZW3DError):
        server.add_variable("wall", 1)                                   # duplicate


def test_edit_sketch_dimension_regenerates(part):
    r = server.sketch_extrude("XY", [{"type": "circle", "center": [0, 0], "radius": 10}], 10)
    sk = r["sketch_id"]
    assert [v_["name"] for v_ in server.list_sketch_variables(sk)["variables"]] == ["D1"]
    out = server.set_sketch_variable(sk, "D1", 40)                       # diameter 20 -> 40
    assert approx(out["shape_details"][0]["volume"], r["volume"] * 4)
    with pytest.raises(ZW3DError):
        server.set_sketch_variable(sk, "D9", 1)
    with pytest.raises(ZW3DError):
        server.list_sketch_variables(999999)


def test_profile_with_inner_hole(part):
    outer = [{"type": "line", "start": [0, 0], "end": [100, 0]}, {"type": "line", "start": [100, 0], "end": [100, 60]},
             {"type": "line", "start": [100, 60], "end": [0, 60]}, {"type": "line", "start": [0, 60], "end": [0, 0]}]
    r = server.sketch_extrude("XY", outer, 10, holes=[[{"type": "circle", "center": [50, 30], "radius": 10}]])
    assert approx(r["volume"], (6000 - math.pi * 100) * 10, rel=2e-3)
    with pytest.raises(ZW3DError):
        server.sketch_extrude("XY", outer, 10, holes=[[{"type": "circle", "center": [1, 1, 1], "radius": 2}]])
    with pytest.raises(ZW3DError):
        server.sketch_extrude("XY", outer, 10, holes="circle")


# ------------------------------------------------------------------ assembly / drawing
def test_assembly_and_drawing_flow(fake):
    with pytest.raises(ZW3DError) as e:
        server.insert_component("C:\\work\\bolt.Z3PRT")
    assert e.value.code == "WRONG_ROOT_ENV"
    server.new_assembly("asm1")
    a = server.insert_component("C:\\work\\bolt.Z3PRT", position=[10, 0, 0])
    b = server.insert_component("C:\\work\\nut.Z3PRT", anchor=True)
    assert server.list_components()["component_ids"] == [a["component_id"], b["component_id"]]
    with pytest.raises(ZW3DError) as e:                                  # part tools are not usable in an assembly
        server.create_box(center=[0, 0, 0], size=[1, 1, 1])
    assert e.value.code == "WRONG_ROOT_ENV"
    with pytest.raises(ZW3DError):
        server.new_assembly(" ")
    assert server.new_drawing("dwg1", "C:\\work\\bolt.Z3PRT")["file"] == "dwg1"
    with pytest.raises(ZW3DError):
        server.new_drawing("dwg2", " ")


def test_new_features_are_one_undo_step(part):
    sid, top, _ = _plate(part)
    server.create_hole(top, [[50, 30, 20]], 10, through=True)
    server.undo()
    assert approx(server.list_shapes()["shapes"][0]["volume"], 120000)


# ------------------------------------------------------------------ the MCP surface
def test_all_tools_exposed_with_descriptions(fake):
    env = dict(os.environ, ZW3D_MCP_PORT=str(fake.port), PYTHONIOENCODING="utf-8")
    p = subprocess.Popen([sys.executable, "-m", "zw3d_mcp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL, env=env)

    def send(o):
        p.stdin.write((json.dumps(o) + "\n").encode())
        p.stdin.flush()

    try:
        send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
            "protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "t", "version": "0"}}})
        p.stdout.readline()
        send({"jsonrpc": "2.0", "method": "notifications/initialized"})
        send({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
        tools = json.loads(p.stdout.readline().decode("utf-8"))["result"]["tools"]
    finally:
        p.kill()
    names = {t["name"] for t in tools}
    for want in ("create_hole", "pattern_linear", "pattern_circular", "mirror", "draft_faces", "sweep", "loft",
                 "list_variables", "set_variable", "add_variable", "list_sketch_variables", "set_sketch_variable",
                 "new_assembly", "insert_component", "list_components", "new_drawing", "list_features"):
        assert want in names, want
    assert all((t.get("description") or "").strip() for t in tools), "every tool needs a description"
