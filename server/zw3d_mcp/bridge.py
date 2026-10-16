"""TCP client for the zw3dmcp plugin (NDJSON over 127.0.0.1)."""

from __future__ import annotations

import itertools
import json
import os
import select
import socket
import threading
from typing import Any

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 47823
DEFAULT_TIMEOUT_MS = 60_000
MAX_LINE = 16 * 1024 * 1024


try:  # mcp >= 2.0
    from mcp.server.mcpserver.exceptions import ToolError as _ToolError
except ImportError:  # mcp 1.x
    from mcp.server.fastmcp.exceptions import ToolError as _ToolError  # type: ignore


class ZW3DError(_ToolError):
    """Error reported by the plugin (or by the transport).

    Subclasses the SDK's ToolError on purpose: mcp 2.x forwards the text of a ToolError to the client but treats
    every other exception as a crash and withholds its message, which would hide `code` and `hint` from the AI.
    """

    def __init__(self, code: str, message: str, hint: str = "", api_code: int | None = None):
        super().__init__(message)
        self.code = code
        self.message = message
        self.hint = hint
        self.api_code = api_code

    def __str__(self) -> str:
        parts = [f"[{self.code}] {self.message}"]
        if self.api_code is not None:
            parts[0] += f" (api_code={self.api_code})"
        if self.hint:
            parts.append(f"Hint: {self.hint}")
        return " ".join(parts)


class Bridge:
    """Thread-safe, lazily connecting client. One request at a time."""

    def __init__(self, host: str | None = None, port: int | None = None):
        self.host = host or os.environ.get("ZW3D_MCP_HOST", DEFAULT_HOST)
        self.port = port or int(os.environ.get("ZW3D_MCP_PORT", DEFAULT_PORT))
        self._sock: socket.socket | None = None
        self._buf = b""
        self._lock = threading.Lock()
        self._ids = itertools.count(1)
        self.slack_s = 5.0  # transport slack on top of the request timeout

    # -- connection management -------------------------------------------------
    def _connect(self) -> None:
        try:
            s = socket.create_connection((self.host, self.port), timeout=3)
        except OSError as e:
            raise ZW3DError(
                "NOT_CONNECTED",
                f"Cannot connect to the ZW3D plugin at {self.host}:{self.port} ({e})",
                "Start ZW3D 2025+, make sure zw3dmcp.dll is loaded, and run the McpStart command "
                "(the service starts automatically unless ZW3D_MCP_AUTOSTART=0).",
            ) from e
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self._sock = s
        self._buf = b""

    def close(self) -> None:
        with self._lock:
            self._drop()

    def _drop(self) -> None:
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
        self._sock = None
        self._buf = b""

    def _alive(self) -> bool:
        """False if the peer already closed the socket (plugin reloaded / ZW3D restarted)."""
        if self._sock is None:
            return False
        try:
            readable, _, _ = select.select([self._sock], [], [], 0)
            if not readable:
                return True
            return self._sock.recv(1, socket.MSG_PEEK) != b""
        except OSError:
            return False

    # -- protocol --------------------------------------------------------------
    def _send(self, line: bytes) -> None:
        assert self._sock is not None
        self._sock.settimeout(10)
        self._sock.sendall(line)

    def _recv_line(self, timeout_s: float) -> bytes:
        assert self._sock is not None
        self._sock.settimeout(timeout_s)
        while b"\n" not in self._buf:
            chunk = self._sock.recv(65536)
            if not chunk:
                raise ConnectionError("connection closed by plugin")
            self._buf += chunk
            if len(self._buf) > MAX_LINE:
                raise ConnectionError("response too large")
        line, _, self._buf = self._buf.partition(b"\n")
        return line

    def call(self, method: str, params: dict[str, Any] | None = None,
             timeout_ms: int = DEFAULT_TIMEOUT_MS) -> dict[str, Any]:
        """Send one request and return `result`. Raises ZW3DError on failure."""
        req_id = next(self._ids)
        payload = json.dumps(
            {"id": req_id, "method": method, "params": params or {}, "timeout_ms": timeout_ms},
            ensure_ascii=False,
        ).encode("utf-8") + b"\n"

        with self._lock:
            # A stale socket (ZW3D restarted, plugin reloaded) fails at send time:
            # that is safe to retry once because nothing reached the plugin.
            for attempt in (0, 1):
                if not self._alive():
                    self._drop()
                    self._connect()
                try:
                    self._send(payload)
                    break
                except OSError:
                    self._drop()
                    if attempt == 1:
                        raise ZW3DError("NOT_CONNECTED", "Lost connection to the ZW3D plugin",
                                        "Check that ZW3D is still running and run McpStart.")
            try:
                # Plugin answers with TIMEOUT itself at timeout_ms; allow slack for transport.
                while True:
                    line = self._recv_line(timeout_ms / 1000.0 + self.slack_s)
                    resp = json.loads(line.decode("utf-8"))
                    if resp.get("id") == req_id:
                        break  # discard stale responses from earlier timed-out calls
            except socket.timeout:
                self._drop()
                raise ZW3DError("TIMEOUT", f"No response for '{method}' within {timeout_ms} ms",
                                "ZW3D may be busy or showing a modal dialog. Close dialogs and retry.")
            except (ConnectionError, OSError, ValueError) as e:
                self._drop()
                raise ZW3DError("NOT_CONNECTED", f"Connection failed while waiting for '{method}': {e}",
                                "The request may or may not have been executed; call list_shapes to check.")

        if resp.get("ok"):
            return resp.get("result", {})
        err = resp.get("error") or {}
        raise ZW3DError(err.get("code", "UNKNOWN"), err.get("message", "unknown error"),
                        err.get("hint", ""), err.get("api_code"))


_default: Bridge | None = None


def get_bridge() -> Bridge:
    global _default
    if _default is None:
        _default = Bridge()
    return _default


def reset_bridge() -> None:
    """Drop the shared bridge (used by tests when the port changes)."""
    global _default
    if _default is not None:
        _default.close()
    _default = None
