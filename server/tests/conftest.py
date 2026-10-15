import os
import sys

import pytest

sys.path.insert(0, os.path.dirname(__file__))
from fake_zw3d import FakeServer  # noqa: E402

from zw3d_mcp import bridge  # noqa: E402


@pytest.fixture
def fake():
    srv = FakeServer(port=0).start()
    os.environ["ZW3D_MCP_PORT"] = str(srv.port)
    bridge.reset_bridge()
    yield srv
    bridge.reset_bridge()
    srv.stop()
    os.environ.pop("ZW3D_MCP_PORT", None)


@pytest.fixture
def part(fake):
    from zw3d_mcp import server
    server.new_part("test")
    return server
