# ZW3D-MCP

**English** | [简体中文](README.zh-CN.md)

Let an AI agent (Claude Code, Cursor, …) model parts in a **running ZW3D 2025+** and check the result (screenshot, volume, bounding box).
The target is ZW3D, **not** ZWCAD.

The whole MCP server is **one DLL** that runs inside ZW3D and speaks MCP over HTTP. No Python, no Node, no extra process.

```
AI agent ── MCP over HTTP: POST http://127.0.0.1:47823/mcp ──▶ zw3dmcp.dll (inside ZW3D)
```

## Quick start

1. Download `zw3d-mcp-vX.Y.Z-win64.zip` from [Releases](../../releases) and unzip it.
2. **Close ZW3D**, then copy `zw3dmcp.dll` to `%APPDATA%\ZWSOFT\ZW3D\ZW3D 2025\custom\apilibs` (create the folder if missing; change the year for newer versions).
3. Start ZW3D. The output window shows `[ZW3D-MCP] MCP endpoint: http://127.0.0.1:47823/mcp`. If not, run the command `McpStart`.
4. Register the endpoint in your client:

   ```bash
   claude mcp add --transport http zw3d http://127.0.0.1:47823/mcp
   ```

   Cursor: `{"mcpServers": {"zw3d": {"url": "http://127.0.0.1:47823/mcp"}}}`.
   Stdio-only clients need a bridge such as `npx -y mcp-remote http://127.0.0.1:47823/mcp` (not verified).
5. Ask the AI: "ping ZW3D".

More detail and troubleshooting: [docs/INSTALL.md](docs/INSTALL.md). Commands `McpStart` / `McpStop` toggle the service inside ZW3D.

### Configuration (environment variables, set before starting ZW3D)

| Variable | Meaning |
|---|---|
| `ZW3D_MCP_PORT` | Listening port (default 47823) |
| `ZW3D_MCP_AUTOSTART=0` | Do not listen automatically; use `McpStart` |
| `ZW3D_MCP_TOKEN` | If set, every request must carry `Authorization: Bearer <token>` |
| `ZW3D_MCP_ENABLE_RUN_COMMAND=1` | Enable the raw `run_command` tool (off by default) |

### Security

The server binds to `127.0.0.1` only and rejects requests whose `Host` or `Origin` is not loopback, so a web page cannot drive ZW3D through your browser.
Without `ZW3D_MCP_TOKEN`, any local process can still call it (including `run_command` if enabled). Never expose the port to a network.

## Tools

Units are **millimetres and degrees**; points are `[x, y, z]`. Modeling tools return the affected shapes with bounding box and volume.
Errors come back as `[CODE] message  Hint: …` so the agent can correct itself. Each modeling call is one undo step and is rolled back completely on failure.

Status: ✅ exercised on a real ZW3D 2025 SP2 · ⚠️ implemented from the SDK headers, not fully verified live (see [docs/api_notes.md](docs/api_notes.md)).

**Session and files**

| Tool | What it does | Main parameters | Status |
|---|---|---|---|
| `ping` | Connection check; plugin version, active file and part | – | ✅ |
| `new_part` | New single-root part (`.Z3PRT`), made active | `name` | ✅ |
| `save_part` | Save, or Save As when a path is given | `path?` | ✅ |
| `export_step` | Export the active file as STEP | `path` | ✅ |
| `screenshot` | PNG of the current view returned as an image (temp file deleted) | `width, height, zoom_all` | ✅ |
| `run_command` | Raw ZW3D command string (needs `ZW3D_MCP_ENABLE_RUN_COMMAND=1`) | `command` | ✅ |
| `undo` / `redo` | Undo/redo whole tool calls; reports removed/added shapes | `steps` | ⚠️ |

**Primitives**

| Tool | What it does | Main parameters | Status |
|---|---|---|---|
| `create_box` | Axis-aligned box | `center+size` or `corner1+corner2`, `combine` | ✅ |
| `create_cylinder` | Cylinder along +Z from the bottom-face centre | `base_center, radius, height, combine` | ✅ |
| `create_sphere` | Sphere | `center, radius, combine` | ✅ |

**Profile features** (`combine`: `none` new body, `add`, `remove`, `intersect`)

| Tool | What it does | Main parameters | Status |
|---|---|---|---|
| `sketch_extrude` | Real sketch on a plane (auto dimensions), then extrude; supports inner-loop holes | `plane, segments, distance, start, symmetric, reverse, draft_angle, dimensions, holes, combine` | ⚠️ |
| `sketch_revolve` | Real sketch revolved around a 2D axis | `plane, segments, axis_point, axis_direction, angle, combine` | ⚠️ |
| `extrude_polygon` | Extrude a closed planar polygon (wireframe, no sketch) | `points, distance, start, direction, draft_angle, combine` | ✅ |
| `revolve_polygon` | Revolve a closed planar polygon | `points, axis_point, axis_direction, angle, combine` | ✅ |
| `extrude_profile` | Extrude lines / 3-point arcs / circle | `segments, distance, …` | ✅ |
| `revolve_profile` | Revolve lines / arcs | `segments, axis_point, axis_direction, angle, …` | ✅ |
| `sweep` | Sweep a closed profile along a line/arc path | `profile, path, combine` | ⚠️ |
| `loft` | Loft through ≥ 2 closed sections | `sections, combine` | ⚠️ |

**Editing features**

| Tool | What it does | Main parameters | Status |
|---|---|---|---|
| `boolean` | Add / subtract / intersect solids (refuses non-overlapping tools) | `operation, base_id, tool_ids, keep_tools` | ✅ |
| `fillet_edges` | Constant-radius fillet (all edges if none given) | `shape_id, radius, edge_ids?` | ✅ |
| `chamfer_edges` | Symmetric or asymmetric chamfer | `shape_id, distance, distance2?, edge_ids?` | ✅ |
| `shell_shape` | Hollow a solid, optional open faces; inward by default | `shape_id, thickness, open_face_ids?, inward` | ⚠️ |
| `create_hole` | Simple / counterbore / countersink holes on a planar face | `face_id, points, diameter, depth` or `through, hole_type` | ⚠️ |
| `draft_faces` | Draft faces about neutral faces | `face_ids, neutral_face_ids, angle, pull_direction` | ⚠️ |
| `pattern_linear` | Linear / grid pattern of a solid or features | `count, spacing, direction, shape_id` or `feature_ids, count2…` | ⚠️ |
| `pattern_circular` | Circular pattern of a solid or features | `count, axis_point, axis_direction, angle?` | ⚠️ |
| `mirror` | Mirror a solid or features across a plane | `plane, shape_id` or `feature_ids, combine` | ⚠️ |

**Queries**

| Tool | What it does | Main parameters | Status |
|---|---|---|---|
| `list_shapes` | All solids with bbox, volume, area | – | ✅ |
| `list_faces` | Faces with bbox, surface type, plane normal / cylinder radius+axis | `shape_id` | ✅ |
| `list_edges` | Edges with exact bbox and curve (line ends / arc centre+radius) | `shape_id` | ✅ |
| `list_features` | History features with id, name, template | – | ⚠️ |
| `get_bounding_box` | Bounding box of a solid | `shape_id` | ✅ |
| `get_mass_props` | Volume, area, `mass_kg`, centre of gravity | `shape_id, density?` | ✅ |

**Parametric edits**

| Tool | What it does | Main parameters | Status |
|---|---|---|---|
| `list_variables` / `add_variable` / `set_variable` | Part variables (distance, angle, number or expression); setting regenerates the part | `name, value` or `expression, kind` | ⚠️ |
| `list_sketch_variables` / `set_sketch_variable` | Sketch variables with diagnostics; edit one and regenerate | `sketch_id, name, value` | ⚠️ |
| `get_feature_data` | Fields of one feature: id, label, type, value, `variable_name` | `feature_id` | ⚠️ |
| `set_feature_value` | Change a numeric parameter of an existing feature and regenerate | `feature_id, field_id, value` | ⚠️ |

Note: regeneration renumbers shape/face/edge ids, so use only the ids returned by the last call.

**Assemblies and drawings** (basic)

| Tool | What it does | Main parameters | Status |
|---|---|---|---|
| `new_assembly` | New assembly file (`.Z3ASM`) | `name` | ⚠️ |
| `insert_component` | Insert a saved part into the active assembly | `file, position, anchor` | ⚠️ |
| `list_components` | Component ids of the active assembly | – | ⚠️ |
| `new_drawing` | New drawing with the standard views of a saved part | `name, part_file` | ⚠️ |

Not covered yet: assembly mates, drawing dimensions/annotations/BOM, sheet metal, weldments, CAM, simulation.

## Build from source (Windows, VS 2022)

```bat
cd plugin
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The SDK is found automatically (`C:\Program Files\ZWSOFT\ZW3D <year>`, newest ≥ 2025); otherwise pass `-DZW3D_SDK_DIR="D:\ZW3D 2026"`.
`-DZW3D_MCP_INSTALL_USER=ON -DZW3D_MCP_YEAR=2025` copies the DLL into the apilibs folder after the build (**close ZW3D first**, a loaded DLL cannot be overwritten).
`scripts/package.ps1` builds the release zip.

## Testing

The plugin only runs inside ZW3D, so the repository has no offline test suite. Live testing is done by hand or by an agent through the MCP tools;
open items and live findings are recorded in [docs/api_notes.md](docs/api_notes.md).

## Repository layout

| Path | Content |
|---|---|
| `plugin/src/server.cpp` | HTTP server (Winsock) |
| `plugin/src/mcp/` | MCP protocol, tool table with JSON Schemas, validation |
| `plugin/src/dispatcher.cpp` | Main-thread dispatcher (all ZW3D calls run on ZW3D's main thread) |
| `plugin/src/handlers/` | ZW3D API handlers |
| `docs/` | `INSTALL.md`, `api_notes.md` |
| `CLAUDE.md` | Design document |
