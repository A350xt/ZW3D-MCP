# ZW3D-MCP

Let an AI agent (Claude Code, Claude Desktop, Cursor …) model parts in a **running ZW3D 2025+** and look at the result
(screenshot, volume, bounding box). Target is ZW3D, **not** ZWCAD.

```
AI agent ──MCP/stdio──▶ server/ (Python) ──TCP 127.0.0.1:47823, NDJSON──▶ plugin/ (zw3dmcp.dll inside ZW3D)
```

## 1. Build the plugin (Windows, VS 2022)

```bat
cd plugin
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The SDK is found automatically (`C:\Program Files\ZWSOFT\ZW3D <year>`, newest ≥ 2025). Other location:
`-DZW3D_SDK_DIR="D:\ZW3D 2026"`. Add `-DZW3D_MCP_INSTALL_USER=ON -DZW3D_MCP_YEAR=2025` to copy the DLL into
`%APPDATA%\ZWSOFT\ZW3D\ZW3D 2025\custom\apilibs` after the build (ZW3D loads it at start-up; **close ZW3D before rebuilding**,
a loaded DLL cannot be overwritten). Other ways to load it: see `docs/api_notes.md`.

On start-up the plugin prints `[ZW3D-MCP] listening on 127.0.0.1:47823`. Commands `McpStart` / `McpStop` toggle the service.

| Env var (set before starting ZW3D) | Meaning |
|---|---|
| `ZW3D_MCP_PORT` | port (default 47823) |
| `ZW3D_MCP_AUTOSTART=0` | do not listen automatically |
| `ZW3D_MCP_ENABLE_RUN_COMMAND=1` | enable the raw `run_command` tool (off by default) |

**Security:** the service binds to loopback only and has **no authentication**. Any local process can drive ZW3D
(including `run_command` if enabled). Do not expose the port.

## 2. Install the MCP server

```bash
pip install -e server            # needs Python 3.10+, mcp
claude mcp add zw3d -- python -m zw3d_mcp
```

Claude Desktop / Cursor: command `python`, args `["-m", "zw3d_mcp"]`. Set `ZW3D_MCP_PORT` for a non-default port.

## 3. Tools (units: mm, degrees)

`ping`, `new_part`, `save_part`, `export_step`, `create_box`, `create_cylinder`, `create_sphere`, `extrude_polygon`,
`revolve_polygon`, `extrude_profile` / `revolve_profile` (lines, 3-point arcs, circles), `boolean`, `fillet_edges`,
`chamfer_edges`, `shell_shape`, `undo`, `redo`, `list_shapes`, `list_faces` (surface type + plane normal), `list_edges`
(per-edge bounding box), `get_mass_props`, `get_bounding_box`, `screenshot`, `run_command` (opt-in). Every creation tool returns the new `shape_id`, its bounding box
and volume. Errors come back as `[CODE] message Hint: …` so the agent can self-correct.

## 4. Test

```bash
cd server && python -m pytest          # no ZW3D needed: uses tests/fake_zw3d.py
python server/tests/fake_zw3d.py       # run the fake plugin standalone for manual MCP testing
python tools/smoke_test.py             # on the ZW3D machine, plugin loaded: exercises every method
```

Status: `tools/smoke_test.py` passes end-to-end on ZW3D 2025 SP2 (all MVP tools, Chinese file names/paths, error paths).
Remaining unverified items are listed in `docs/api_notes.md`.

## Layout

`plugin/` C++ plugin · `server/` Python MCP server + tests (incl. `fake_zw3d.py`) · `tools/smoke_test.py` · `docs/api_notes.md` · `CLAUDE.md` design doc.
