import json
import math
import os
import pathlib

import pytest

from zw3d_mcp import bridge, server
from zw3d_mcp.bridge import Bridge, ZW3DError


def approx(a, b, rel=1e-6):
    return a == pytest.approx(b, rel=rel)


# ------------------------------------------------------------------ connectivity
def test_ping(fake):
    r = server.ping()
    assert r["plugin_version"] and r["server_version"]


def test_not_connected_gives_hint():
    os.environ["ZW3D_MCP_PORT"] = "1"  # nothing listens here
    bridge.reset_bridge()
    try:
        with pytest.raises(ZW3DError) as e:
            server.ping()
    finally:
        os.environ.pop("ZW3D_MCP_PORT")
        bridge.reset_bridge()
    assert e.value.code == "NOT_CONNECTED" and "McpStart" in e.value.hint


def test_reconnect_after_plugin_restart(fake):
    server.ping()
    fake.stop()
    from fake_zw3d import FakeServer
    fake2 = FakeServer(port=fake.port).start()
    try:
        assert server.ping()["plugin_version"]
    finally:
        fake2.stop()


def test_timeout(fake):
    b = Bridge("127.0.0.1", fake.port)
    b.slack_s = 0.3
    with pytest.raises(ZW3DError) as e:
        b.call("_sleep", {"ms": 5000}, timeout_ms=200)
    assert e.value.code == "TIMEOUT"


# ------------------------------------------------------------------ error mapping
def test_modeling_before_new_part_is_wrong_root_env(fake):
    with pytest.raises(ZW3DError) as e:
        server.create_box(center=[0, 0, 0], size=[1, 1, 1])
    assert e.value.code == "WRONG_ROOT_ENV" and "new_part" in e.value.hint


def test_unknown_shape_id(part):
    with pytest.raises(ZW3DError) as e:
        server.get_bounding_box(999)
    assert e.value.code == "INVALID_INPUT" and "list_shapes" in e.value.hint


# ------------------------------------------------------------------ client-side validation
@pytest.mark.parametrize("kwargs", [
    dict(center=[0, 0, 0], size=[10, -1, 5]),
    dict(center=[0, 0, 0], size=[10, 0, 5]),
    dict(center=[0, 0], size=[1, 1, 1]),
    dict(center=[0, 0, float("nan")], size=[1, 1, 1]),
    dict(corner1=[0, 0, 0], corner2=[1, 1, 1], center=[0, 0, 0]),
    dict(),
])
def test_box_validation(fake, kwargs):
    with pytest.raises(ZW3DError) as e:
        server.create_box(**kwargs)
    assert e.value.code == "INVALID_INPUT"


def test_validation_never_reaches_plugin(fake):
    # No part exists: a plugin round trip would give WRONG_ROOT_ENV, validation must win first.
    with pytest.raises(ZW3DError) as e:
        server.create_cylinder([0, 0, 0], radius=-5, height=10)
    assert e.value.code == "INVALID_INPUT"


def test_polygon_validation(part):
    for pts in ([[0, 0, 0], [1, 0, 0]],                                   # too few
                [[0, 0, 0], [1, 0, 0], [2, 0, 0]],                         # collinear
                [[0, 0, 0], [1, 0, 0], [1, 1, 0], [0, 1, 1]],              # non planar
                [[0, 0, 0], [0, 0, 0], [1, 1, 0]]):                        # duplicate
        with pytest.raises(ZW3DError) as e:
            server.extrude_polygon(pts, 5)
        assert e.value.code == "INVALID_INPUT"


# ------------------------------------------------------------------ modeling happy paths
def test_box_by_corners_and_center_agree(part):
    a = server.create_box(corner1=[0, 0, 0], corner2=[100, 60, 20])
    b = server.create_box(center=[50, 30, 10], size=[100, 60, 20])
    assert approx(a["volume"], 120000) and approx(b["volume"], 120000)
    assert a["bounding_box"]["min"] == [0, 0, 0] and a["bounding_box"]["max"] == [100, 60, 20]


def test_cylinder_sphere_volumes(part):
    c = server.create_cylinder([0, 0, 0], 15, 40)
    s = server.create_sphere([200, 0, 0], 10)
    assert approx(c["volume"], math.pi * 15 ** 2 * 40)
    assert approx(s["volume"], 4 / 3 * math.pi * 1000)


def test_extrude_polygon_volume_and_bbox(part):
    sq = [[0, 0, 0], [10, 0, 0], [10, 10, 0], [0, 10, 0]]
    r = server.extrude_polygon(sq, 5, direction=[0, 0, 1])
    assert approx(r["volume"], 500)
    assert r["bounding_box"]["max"][2] == pytest.approx(5)
    r2 = server.extrude_polygon(sq + [], 3, start=1)  # closed-by-repeat tolerated below
    assert approx(r2["volume"], 100 * 2)


def test_extrude_accepts_explicitly_closed_outline(part):
    sq = [[0, 0, 0], [10, 0, 0], [10, 10, 0], [0, 10, 0], [0, 0, 0]]
    assert approx(server.extrude_polygon(sq, 2)["volume"], 200)


def test_revolve_rectangle_is_hollow_cylinder(part):
    # rectangle x 10..20, y 0..10 revolved about the Y axis (the CLAUDE.md example), full turn
    prof = [[10, 0, 0], [20, 0, 0], [20, 10, 0], [10, 10, 0]]
    r = server.revolve_polygon(prof, [0, 0, 0], [0, 1, 0])
    assert approx(r["volume"], math.pi * (20 ** 2 - 10 ** 2) * 10)
    half = server.revolve_polygon(prof, [0, 0, 0], [0, 1, 0], angle=180)
    assert approx(half["volume"], r["volume"] / 2)


def test_bad_revolve_angle(part):
    prof = [[10, 0, 0], [20, 0, 0], [20, 10, 0], [10, 10, 0]]
    with pytest.raises(ZW3DError):
        server.revolve_polygon(prof, [0, 0, 0], [0, 1, 0], angle=400)


def test_boolean_subtract_and_hole_via_combine(part):
    """M2 acceptance scenario: 100x60x20 plate + d30 h40 cylinder, then report total volume."""
    plate = server.create_box(corner1=[0, 0, 0], corner2=[100, 60, 20])
    cyl = server.create_cylinder([50, 30, 0], 15, 40)
    # union (cylinder is not a box -> fake uses bbox overlap, so only check volume monotonicity & ids)
    r = server.boolean("add", plate["shape_id"], [cyl["shape_id"]])
    assert r["shapes"] == [plate["shape_id"]]
    assert r["volume"] > 120000

    p2 = server.create_box(corner1=[0, 0, 0], corner2=[100, 60, 20])
    cut = server.create_box(corner1=[10, 10, -5], corner2=[30, 30, 25])
    r2 = server.boolean("subtract", p2["shape_id"], [cut["shape_id"]])
    assert approx(r2["volume"], 120000 - 20 * 20 * 20)


def test_combine_remove_returns_no_shape_id(part):
    server.create_box(corner1=[0, 0, 0], corner2=[10, 10, 10])
    r = server.create_box(corner1=[2, 2, -1], corner2=[4, 4, 11], combine="remove")
    assert r["shape_id"] is None
    assert approx(server.list_shapes()["shapes"][0]["volume"], 1000 - 2 * 2 * 10)


def test_fillet_all_and_radius_error(part):
    b = server.create_box(corner1=[0, 0, 0], corner2=[100, 60, 20])
    with pytest.raises(ZW3DError) as e:
        server.fillet_edges(b["shape_id"], radius=50)
    assert e.value.code == "CMMD_EXEC_ERROR" and "smaller" in e.value.hint
    r = server.fillet_edges(b["shape_id"], radius=2)
    assert r["volume"] < 120000
    assert server.list_edges(b["shape_id"])["count"] == 12


# ------------------------------------------------------------------ queries
def test_mass_props_and_bbox(part):
    b = server.create_box(corner1=[0, 0, 0], corner2=[10, 20, 30])
    m = server.get_mass_props(b["shape_id"])
    assert approx(m["volume"], 6000) and approx(m["area"], 2 * (200 + 600 + 300))
    assert m["center_of_gravity"] == [5, 10, 15]
    assert server.get_bounding_box(b["shape_id"])["size"] == [10, 20, 30]
    assert len(server.list_faces(b["shape_id"])["faces"]) == 6


def test_screenshot_returns_png_and_cleans_up(part, monkeypatch):
    seen = []
    real_call = server._call

    def spy(*a, **k):
        r = real_call(*a, **k)
        if "path" in r:
            seen.append(r["path"])
        return r

    monkeypatch.setattr(server, "_call", spy)
    img = server.screenshot(width=64, height=64)
    content = img.to_image_content()
    assert (getattr(content, "mime_type", None) or content.mimeType) == "image/png" and content.data
    assert seen and not os.path.exists(seen[0]), "screenshot temp file was not recycled"


def test_screenshot_refuses_foreign_paths(part, monkeypatch):
    import tempfile
    victim = pathlib.Path(tempfile.mkdtemp()) / "important.txt"
    victim.write_text("keep me")
    monkeypatch.setattr(server, "_call", lambda *a, **k: {"path": str(victim)})
    with pytest.raises(ZW3DError):
        server.screenshot()
    assert victim.exists()


def test_screenshot_size_validation(part):
    with pytest.raises(ZW3DError):
        server.screenshot(width=10, height=10)


def test_files(part):
    import tempfile
    tmp_path = pathlib.Path(tempfile.mkdtemp())
    with pytest.raises(ZW3DError) as e:
        server.save_part()
    assert e.value.code == "INVALID_PATH"
    assert server.save_part(str(tmp_path / "中文路径.Z3PRT"))["saved"]
    out = tmp_path / "out.step"
    server.export_step(str(out))
    assert out.read_text().startswith("ISO-10303-21")
    with pytest.raises(ZW3DError):
        server.export_step(str(tmp_path / "out.txt"))


def test_run_command_disabled_by_default(part):
    with pytest.raises(ZW3DError) as e:
        server.run_command("anything")
    assert e.value.code == "DISABLED"


def test_chinese_text_roundtrip(fake):
    r = server.new_part("底板_零件")
    assert r["part"] == "底板_零件"
    assert server.ping()["active_root"] == "底板_零件"


# ------------------------------------------------------------------ protocol conformance
CASES = pathlib.Path(__file__).with_name("protocol_cases.json")


@pytest.mark.parametrize("case", json.loads(CASES.read_text(encoding="utf-8")), ids=lambda c: c["name"])
def test_protocol_cases(fake, case):
    """The same file is replayed against the real plugin by tools/smoke_test.py --cases."""
    b = Bridge("127.0.0.1", fake.port)
    last = None
    for step in case["steps"]:
        try:
            last = {"ok": True, "result": b.call(step["method"], step.get("params", {}))}
        except ZW3DError as e:
            last = {"ok": False, "error": {"code": e.code}}
        if "expect_ok" in step:
            assert last["ok"] == step["expect_ok"], (step, last)
        if "expect_error_code" in step:
            assert last["error"]["code"] == step["expect_error_code"]
        for key in step.get("expect_keys", []):
            assert key in last["result"], (key, last)


# ------------------------------------------------------------------ round profiles, chamfer, shell, undo
def test_extrude_circle_profile_matches_cylinder(part):
    r = server.extrude_profile([{"type": "circle", "center": [0, 0, 0], "radius": 15}], 40, direction=[0, 0, 1])
    assert approx(r["volume"], math.pi * 15 ** 2 * 40, rel=1e-3)


def test_extrude_slot_with_arcs(part):
    # 40 x 20 stadium: two lines + two half-circle arcs (r=10)
    segs = [
        {"type": "line", "start": [0, 0, 0], "end": [40, 0, 0]},
        {"type": "arc", "start": [40, 0, 0], "through": [50, 10, 0], "end": [40, 20, 0]},
        {"type": "line", "start": [40, 20, 0], "end": [0, 20, 0]},
        {"type": "arc", "start": [0, 20, 0], "through": [-10, 10, 0], "end": [0, 0, 0]},
    ]
    r = server.extrude_profile(segs, 5, direction=[0, 0, 1])
    assert approx(r["volume"], (40 * 20 + math.pi * 100) * 5, rel=2e-3)


def test_hole_by_circle_remove(part):
    server.create_box(corner1=[0, 0, 0], corner2=[100, 60, 20])
    server.extrude_profile([{"type": "circle", "center": [50, 30, -1], "radius": 15}], 22,
                           direction=[0, 0, 1], combine="remove")
    vol = server.list_shapes()["shapes"][0]["volume"]
    # fake removes by bbox overlap (30x30x20); real ZW3D gives the exact cylinder
    assert vol < 120000


def test_revolve_profile_arc(part):
    segs = [{"type": "line", "start": [10, 0, 0], "end": [20, 0, 0]},
            {"type": "line", "start": [20, 0, 0], "end": [20, 10, 0]},
            {"type": "line", "start": [20, 10, 0], "end": [10, 10, 0]},
            {"type": "line", "start": [10, 10, 0], "end": [10, 0, 0]}]
    r = server.revolve_profile(segs, [0, 0, 0], [0, 1, 0])
    assert approx(r["volume"], math.pi * (400 - 100) * 10)


@pytest.mark.parametrize("segs", [
    [],
    [{"type": "spline"}],
    [{"type": "line", "start": [0, 0, 0], "end": [1, 0, 0]}, {"type": "line", "start": [5, 5, 0], "end": [0, 0, 0]}],
    [{"type": "arc", "start": [0, 0, 0], "through": [1, 0, 0], "end": [2, 0, 0]}],
    [{"type": "circle", "center": [0, 0, 0], "radius": 5}, {"type": "line", "start": [0, 0, 0], "end": [1, 0, 0]}],
    [{"type": "circle", "center": [0, 0, 0], "radius": -5}],
])
def test_profile_validation(part, segs):
    with pytest.raises(ZW3DError) as e:
        server.extrude_profile(segs, 5)
    assert e.value.code == "INVALID_INPUT"


def test_chamfer(part):
    b = server.create_box(corner1=[0, 0, 0], corner2=[100, 60, 20])
    r = server.chamfer_edges(b["shape_id"], 2)
    assert r["volume"] < 120000 and r["edge_count"] == 12
    with pytest.raises(ZW3DError) as e:
        server.chamfer_edges(b["shape_id"], 15)
    assert "smaller" in e.value.hint


def test_shell_open_box(part):
    b = server.create_box(corner1=[0, 0, 0], corner2=[100, 60, 20])
    faces = server.list_faces(b["shape_id"])["faces"]
    top = [f for f in faces if f.get("plane_normal") == [0.0, 0.0, 1.0] and f["bounding_box"]["min"][2] == 20][0]
    r = server.shell_shape(b["shape_id"], 2, open_face_ids=[top["face_id"]])
    # box 100x60x20, wall 2, top open: outer - inner(96 x 56 x 18)
    assert approx(r["volume"], 120000 - 96 * 56 * 18)
    with pytest.raises(ZW3DError):
        server.shell_shape(b["shape_id"], 0)


def test_undo_redo(part):
    server.create_box(corner1=[0, 0, 0], corner2=[10, 10, 10])
    server.create_sphere([50, 0, 0], 5)
    assert server.list_shapes()["count"] == 2
    assert server.undo()["steps_done"] == 1
    assert server.list_shapes()["count"] == 1
    assert server.redo()["steps_done"] == 1
    assert server.list_shapes()["count"] == 2
    server.undo(2)
    with pytest.raises(ZW3DError) as e:
        server.undo()
    assert e.value.code == "NOTHING_TO_DO"
    with pytest.raises(ZW3DError):
        server.undo(0)


# ------------------------------------------------------------------ errors must survive the real MCP transport
def _mcp_call(port, calls):
    """Run `python -m zw3d_mcp` over stdio (the way a client does) and return the tools/call results."""
    import subprocess
    import sys
    env = dict(os.environ, ZW3D_MCP_PORT=str(port), PYTHONIOENCODING="utf-8")
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
        out = []
        for i, (name, args) in enumerate(calls, start=2):
            send({"jsonrpc": "2.0", "id": i, "method": "tools/call", "params": {"name": name, "arguments": args}})
            out.append(json.loads(p.stdout.readline().decode("utf-8")))
        return out
    finally:
        p.kill()


def test_errors_reach_the_ai_with_code_and_hint(fake):
    """Regression: with mcp 2.x a non-ToolError exception arrives as just 'Error executing tool X'."""
    calls = [
        ("create_box", {"center": [0, 0, 0], "size": [10, -1, 5]}),                       # client-side validation
        ("create_box", {"center": [0, 0, 0], "size": [1, 1, 1]}),                         # plugin error: no part yet
        ("run_command", {"command": "x"}),                                               # DISABLED
    ]
    replies = _mcp_call(fake.port, calls)
    texts = []
    for r in replies:
        res = r["result"]
        assert res["isError"] is True
        texts.append(res["content"][0]["text"])
    assert "INVALID_INPUT" in texts[0] and "positive" in texts[0]
    assert "WRONG_ROOT_ENV" in texts[1] and "new_part" in texts[1]
    assert "DISABLED" in texts[2] and "ZW3D_MCP_ENABLE_RUN_COMMAND" in texts[2]
