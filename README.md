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

Units are millimetres and degrees; points are `[x, y, z]`. A usage guide (workflow, which tool to pick, common pitfalls) is built into the server:
clients receive it automatically as the MCP `instructions`, and it is also available as the prompt `zw3d_guide`.

**Session and files**

| Tool | What it does |
|---|---|
| `ping` | Check the connection; plugin version, active file and part |
| `new_part` | Create a new part file (`.Z3PRT`) and make it active |
| `save_part` | Save, or Save As when a path is given |
| `export_step` | Export the active file as STEP |
| `open_file` | Open an existing ZW3D file |
| `import_file` | Import STEP, Parasolid, SAT, IGES or STL |
| `screenshot` | Capture the current view as an image (optionally from a standard view / display mode) |
| `set_view` | Switch the standard view and display mode |
| `list_dialogs` | Read the pop-up dialogs ZW3D is showing (title, text, buttons) |
| `dismiss_dialog` | Press a button of the topmost pop-up (accept, cancel or by caption) |
| `run_command` | Send a raw ZW3D command (disabled unless enabled by environment variable) |
| `undo` / `redo` | Undo / redo whole tool calls |

**Primitives**

| Tool | What it does |
|---|---|
| `create_box` | Create a box |
| `create_cylinder` | Create a cylinder |
| `create_sphere` | Create a sphere |

**Profile features**

| Tool | What it does |
|---|---|
| `sketch_extrude` | Draw a real sketch on a plane (optionally with inner holes) and extrude it |
| `sketch_revolve` | Draw a real sketch on a plane and revolve it around an axis |
| `extrude_polygon` | Extrude a closed planar polygon |
| `revolve_polygon` | Revolve a closed planar polygon around an axis |
| `extrude_profile` | Extrude a profile made of lines, arcs and circles |
| `revolve_profile` | Revolve a profile made of lines and arcs |
| `sweep` | Sweep a closed profile along a path |
| `loft` | Loft through two or more closed sections |
| `helical_sweep` | Sweep a closed profile along a helix: springs, coils and real modeled threads |
| `add_thread` | Attach a cosmetic thread definition to a cylindrical face |

**Editing features**

| Tool | What it does |
|---|---|
| `boolean` | Add, subtract or intersect solids |
| `fillet_edges` | Round edges with a constant radius |
| `chamfer_edges` | Chamfer edges |
| `shell_shape` | Hollow out a solid, optionally leaving faces open |
| `create_hole` | Cut simple, counterbore, countersink or threaded holes on a planar face |
| `draft_faces` | Apply a draft angle to faces |
| `pattern_linear` | Repeat a solid or features in a line or grid |
| `pattern_circular` | Repeat a solid or features around an axis |
| `mirror` | Mirror a solid or features across a plane |
| `move_shapes` | Move and/or rotate solids |
| `scale_shapes` | Scale solids uniformly or per axis |
| `create_rib` | Add a rib from an open profile |
| `thicken_faces` | Create a solid by thickening faces |
| `trim_shape` | Cut away one side of a solid with a plane or cutter faces |
| `split_shape` | Split a solid into separate solids |
| `extend_face` | Extend a face outward |
| `set_color` | Colour faces or solids |
| `delete_shapes` | Delete solids |
| `delete_features` | Delete history features |
| `suppress_features` | Suppress or unsuppress features |
| `rename_feature` | Rename a history feature |

**Queries**

| Tool | What it does |
|---|---|
| `list_shapes` | List all solids with bounding box, volume and area |
| `list_faces` | List the faces of a solid with their geometry |
| `list_edges` | List the edges of a solid with their geometry |
| `list_features` | List the history features of the part |
| `get_bounding_box` | Get the bounding box of a solid |
| `measure_distance` | Minimum distance between two solids, faces or edges (0 = touching) |
| `get_mass_props` | Get volume, area, mass and centre of gravity |

**Parametric edits**

| Tool | What it does |
|---|---|
| `list_variables` | List the part variables |
| `add_variable` | Add a part variable (value or expression) |
| `set_variable` | Change a part variable and regenerate |
| `list_sketch_variables` | List the variables of a sketch |
| `set_sketch_variable` | Change a sketch variable and regenerate |
| `get_feature_data` | Read the editable fields of a feature |
| `set_feature_value` | Change a numeric parameter of an existing feature and regenerate |

**Assemblies and drawings**

| Tool | What it does |
|---|---|
| `new_assembly` | Create a new assembly file |
| `insert_component` | Insert a saved part into the active assembly |
| `list_components` | List the components of the active assembly |
| `list_component_faces` | List the faces of a component as pick paths for constraints |
| `add_constraint` | Add an assembly constraint (lock, coincident, concentric, distance, parallel, perpendicular, tangent, angular) |
| `new_drawing` | Create a drawing with the standard views of a saved part |
| `list_drawing_views` | List the drawings and views of the active file |
| `auto_dimension` | Add automatic dimensions to a drawing view |
| `add_bom` | Insert a bill-of-materials table |

## Build from source (Windows, VS 2022)

```bat
cd plugin
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The SDK is found automatically (`C:\Program Files\ZWSOFT\ZW3D <year>`, newest ≥ 2025); otherwise pass `-DZW3D_SDK_DIR="D:\ZW3D 2026"`.
`-DZW3D_MCP_INSTALL_USER=ON -DZW3D_MCP_YEAR=2025` copies the DLL into the apilibs folder after the build (**close ZW3D first**, a loaded DLL cannot be overwritten).
`scripts/package.ps1` builds the release zip.
