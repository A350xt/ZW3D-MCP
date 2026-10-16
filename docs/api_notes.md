# ZW3D API notes (2025+)

Everything under "Verified" was read directly from the local SDK headers
(`C:\Program Files\ZWSOFT\ZW3D 2025\api\inc\*.h`, 237 files) or the vendor examples. "Unverified" items can only be
confirmed by driving a live ZW3D through the MCP tools and are the first thing to check on a new machine.

## Environment (M0) — verified

| Item | Value |
|---|---|
| Install dir | `C:\Program Files\ZWSOFT\ZW3D 2025` |
| Main header | `<install>\api\inc\VxApi.h` (includes `zwapi_*.h`) |
| Import library | `<install>\ZW3D.lib` (imports `ZW3D.dll`) |
| SDK docs / examples | `<install>\api\ApiHelpDoc.chm`, `ZW3D API introduction.pdf`, `ApiExample.zip`, `DevelopmentTemplate\ZW3DApiCMakeTemplate.zip` |
| Compiler used | VS 2022 (MSVC 19.44), CMake + Ninja bundled with VS, x64, `/MD` |
| Newer versions | `CMakeLists.txt` auto-picks the newest `C:\Program Files\ZWSOFT\ZW3D <year>` with `api\inc\VxApi.h`; override with `-DZW3D_SDK_DIR=` or env `ZW3D_DIR`. The user plugin folder year is `-DZW3D_MCP_YEAR=`. |

### Plugin contract — verified (vendor example `BaseInquiry`, API introduction p.10–14)

- Exports: `int <dll>Init(int format, void *data)` and `int <dll>Exit(void)`; the name comes from the DLL file name
  (`zw3dmcp.dll` → `zw3dmcpInit` / `zw3dmcpExit`). Confirmed with `dumpbin /exports`.
- Commands: `cvxCmdFunc("Name", (void*)Fn, VX_CODE_GENERAL)` with `int Fn(int idData)`; `cvxCmdFuncUnload("Name")` in Exit
  (mandatory for dynamic unload).
- Loading, any one of:
  1. ZW3D Applications Manager (path is remembered in the registry).
  2. Copy the DLL to `%APPDATA%\ZWSOFT\ZW3D\ZW3D 2025\custom\apilibs` (scanned at start-up). CMake option `-DZW3D_MCP_INSTALL_USER=ON` does this after each build.
  3. Create `apilibs` in the install directory and copy it there.
  4. Registry-registered resource path (recommended by vendor for distributed plugins; also needs `ZwResourcePathAdd` in Init).
- A DLL that is loaded cannot be overwritten: close ZW3D before rebuilding/copying.
- Auto-load registration tool: vendor example `12.DllRegister` (`DllRegister -n <key> -a <dll> -d <desc> -l <dir>`).

## Signatures differing from CLAUDE.md §8 — verified

| Function | Real 2025 signature |
|---|---|
| `cvxDispState` | `void cvxDispState(int Option, vxDispState State)`; `vxDispState` is `int[10]`. Option 0 = disable, 1 = re-enable (use this after errors), 2 = re-enable **and refresh**. Must be paired in the same active file. |
| `cvxCmdSend` | `void cvxCmdSend(char *CommandString)` (non-const) |
| `cvxFileNew` / `cvxFileNewSingle` | `cvxFileNew(const vxLongPath)` makes a multi-root **.Z3** file (needs `cvxRootAdd`; Save As to .Z3PRT failed on it). **`new_part` uses `cvxFileNewSingle(name, VX_FILE_PART, VX_NORMAL_PART, NULL, NULL)` → single-root `.Z3PRT`** (the 2025 default for parts; header comments `VX_FILE_PART = Z3PRT file`). |
| `cvxRootAdd` | `evxErrors cvxRootAdd(evxRootType Type, vxRootName Name, const char *Template)`; `vxRootName` = `char[256]` |
| `cvxPartInqShapes` | `(const vxLongPath File, const vxRootName Part, int *Count, int **Shapes)` — **`NULL`/empty File and Part = active file/part** (header doc). Free with `cvxMemFree((void**)&ptr)`. |
| `cvxFileExport` | `(evxExportType, const vxLongPath Path, void *Data)`; `cvxFileExportInit` zeroes then fills defaults — call it first. |
| `cvxPartInqShapeMass` | `(int idShape, double Density, svxMassProp*)`: "Density = 0 uses the shape default"; the doc says kg/mm³, the struct comment says kg/m³ → treat `mass` as unreliable until checked; volume/area are mm³/mm². |
| `cvxPartInqShapeBox` | `(int idShape, svxMatrix *Mat, svxBndBox *Box)`, `Mat = NULL` OK; `svxBndBox` is `{svxLimit X,Y,Z}` with `.min/.max`. |
| `cvxPartInqFaceBox` | `(int idFace, svxBndBox*)` exists and is used by `list_faces`. |

All of the `cvxPart*` modeling calls used here are marked `@deprecated` in 2025 headers (replacement names are in each
doc comment, e.g. `ZwFeatureBoxCreateByCenter`, `ZwFeatureExtrudeCreate`, `ZwFeatureLineCreateBy2Point`,
`ZwFeatureCurvelistCreate`, `ZwShapeListGet`, `ZwShapeEdgeListGet`, `ZwFeatureAddShapeCreate`…). They still ship; M4 replaces them.

## Structures — verified (zwapi_cmd_shape_data.h)

- `svxBoxData`: `Combine, idPlane, useAxis, axis, Center` (**centre of the box volume**), `X, Y, Z` (lengths), `ftrName`. `cvxPartBoxInit` first.
- `svxCylData`: `Center` = **centre of the bottom face**, `Radius`, `Length` (along Z of the XY plane when `idPlane=0`).
- `svxSphereData`: `Combine, Center, Radius`.
- `svxExtrudeData`: `idProfile` (curve list or sketch), `Start`/`End` (distance, mm), `Draft` (deg), `UseDirection`+`Direction`,
  `StartType/EndType = VX_INPU_VALUE` for plain distances, `OffsetTyp = VX_OFFSET_NONE`.
- `svxRevolveData`: `idProfile`, `Start`/`End` (deg), `Axis{Pnt,Dir}`, `StartType/EndType`, `OffsetTyp`.
- `evxBoolType`: `VX_BOOL_NONE=0 (new base), ADD=1, REMOVE=2, INTERSECT=3`.
- `idShape` is only output when `Combine == VX_BOOL_NONE`. In 2024+ `VX_BOOL_ADD` is implemented as "create base, then add-shape feature".
- Export: `VX_EXPORT_TYPE_IMG=1`, `VX_EXPORT_TYPE_STEP=6` (counted from `IMG=1`; always use the symbol); `VX_EXPORT_IMG_TYPE_PNG=3`;
  `svxSTEPData{AppProtocol(1=AP214), OutPut(0 part/1 assembly, default 1), ExportType(0 all), …}`.
- Errors (`evxErrors`, `zwapi_util.h`): `ZW_API_NO_ERROR=0`, `ZW_API_WRONG_ROOT_ENV=-6`, `ZW_API_INVALID_INPUT=-31`,
  `ZW_API_CMMD_EXEC_ERROR=-152`, … mapped with hints in `plugin/src/errors.cpp`.

## Unverified on a live ZW3D

1. `Init`/handlers really run on the UI thread and the message-only window receives messages while ZW3D is idle.
   `ping` returns `main_thread` / `handler_thread_id` / `init_thread_id` for this.
2. Behaviour while ZW3D is in a command or modal dialog (we return `TIMEOUT` with a hint; no `BUSY` detection yet).
3. `cvxFileNew` + `cvxRootAdd` leaves a part active and the file name format (extension, folder) it expects; if it misbehaves
   switch `h_new_part` to `cvxFileNewSingle`.
4. Extrusion direction when `direction` is omitted (profile normal sign), and the exact meaning of `End` when `Start != 0`.
5. `svxSTEPData.OutPut` default (assembly) when exporting a part file; set to 0 if the STEP comes out empty.
6. Whether PNG export honours `Width/Height` or exports the window size.
7. `cvxPartFillet` on edges of a shape with no prior selection context; `cvxPartBool` keeps the base shape id.
8. Chinese paths: conversion is UTF-8 → `CP_ACP` (GBK). A path containing characters outside the ANSI code page cannot be passed.
9. (resolved) Mass: on ZW3D 2025 SP2 `svxMassProp.Density` is kg/m³ and `.Mass` = Volume(mm³)×Density (not kg); the plugin returns `mass_kg = Volume*1e-9*Density`.

## Live verification log

2025-10-15, ZW3D 2025 SP2 x64: a scripted run of every MVP tool passed.
Confirmed: items 1 (handlers + Init on the same UI thread), 3 (`cvxFileNewSingle` -> `.Z3PRT` part, Chinese names), 5 (STEP export of a part file),
6 (PNG export works, ~130 ms), 7 (fillet, boolean keeps the base id), 8 (Chinese Save As path), 9 (mass units, see above).
Still open: 2 (modal dialog / command in progress), 4 (default extrusion direction when `direction` is omitted),
and whether PNG honours Width/Height.

## Batch 2 (read from headers; NOT yet run on a live ZW3D)

- Arc/circle profiles: `cvxPartArcInit/cvxPartArc` with `arcType = VX_ARC_THROUGH` (start, end, through as `svxPntOnEntPath` with
  `pntType = VX_FREE_POINT` and `pnt`), and `cvxPartCircleInit/cvxPartCircle` with `type = VX_CIRCLE_THROUGH` and 3 `bndPnts`.
  Three free points make both plane-independent. `svxPntOnEntPath` is zeroed with memset before use.
- Chamfer: `cvxPartChamferAllInit(svxChamFlltData*)`, `type = VX_CHAMFLLT_SYMMETRIC | VX_CHAMFLLT_ASYMMETRIC`, `nEdges`, `idEdge`, `setback1/2`; `cvxPartChamferAll(&data, &idFeature)`.
- Shell: `cvxPartShellInit(svxShellData*)`, `idShape`, `dThick`, `cntOpenFace`, `pIdOpenFace`; `cvxPartShell(&data, &idFeature)`.
- Undo/redo: `cvxUndoRedo(int Undo)` (1 = undo, 0 = redo), one operation per call. Open question: whether one MCP tool call
  (e.g. a polygon extrude = N lines + curve list + extrude) is one undo step or several.
- Edge bbox: `cvxPartInqEntBox(idEdge, NULL, &box)`; face surface: `cvxPartInqFaceSrfPrim` (`srfType`: `VX_SF_PRIM_PLN/CYL/...`,
  plane normal = z column `zx,zy,zz` of `pln.form`, sign not guaranteed outward; free NURBS with `cvxSurfaceFree`).

## Batch 3 (from user feedback; headers-based, verify live)

- **Sketches**: `cvxPartSkNew(svxMatrix *Plane, int *idSketch)` creates AND activates a sketch; geometry via `cvxSkLine2pt`,
  `cvxSkArc3pt(Start, End, Mid)` (note the order), `cvxSkCirRad(Center, R)` with `svxPoint2` in sketch coordinates;
  `cvxSkAutoAddCoinCons(-1)` joins end points; `cvxSkAddConsDim(type, svxConsDim*)` adds driving dimensions (linear aligned = sub
  type 3, radial diameter = sub type 2; best effort, failures are counted not fatal); leave with `cvxRootExit()` (RAII guard).
  The sketch id is then passed as `idProfile` to `cvxPartExtrude` / `cvxPartRevolve`. `svxMatrix` columns: x = (xx,xy,xz),
  y = (yx,yy,yz), z = (zx,zy,zz), origin = (xt,yt,zt) — field names are `<axis><component>`.
- **Undo granularity / failure cleanup**: every modeling handler runs inside `invoke_handler`: `base = cvxXnUndoCount()`;
  on success `cvxXnBundle(base, method)` merges all steps of the call into one; on failure `cvxUndoRedo(1)` is repeated until the
  count is back at `base`, so no loose lines/sketches remain. Open: whether `cvxXnUndoCount` counts exactly the steps `cvxUndoRedo`
  undoes (to check live: "one undo removes exactly one tool call" and "failed calls leave no step").
- **Round edge boxes**: `cvxEntBndBox` returns a loose box for circles/arcs (R6 circle gave 18x20.8); `list_edges` now rebuilds the
  box from the curve (frame origin/axes, `R`, `A1..A2` degrees; lines from `P1`/`P2`).
- `api_code` is `null` for errors that do not come from a ZW3D API call (0 reads as success).
- Revolve crossing the axis is rejected client-side (profile on both sides of the axis); sketch revolve axis is 2D in sketch frame.
- STEP files written with a Chinese file name show garbled bytes in the header comment (ZW3D's translator writes GBK); geometry is unaffected.

## Batch 4 (read from headers; NOT yet run on a live ZW3D)

| Tool | API used | Notes / open questions |
|---|---|---|
| `create_hole` | `cvxPartHoleInit(VX_GENERAL_HOLE, evxHoleType, &d)`, `cvxPartHole(&d, &idOp)` | `idInsFace` = face, `Points` projected onto it, direction = face normal (`UseDirection = 0`), `End = VX_THROUGH_ALL / VX_BLIND`. counterbore/countersink use `Diameter2/Depth2`. Returns the history op id as `feature_id`. |
| `pattern_linear/circular` (solid) | `cvxPtnDataInit(type, &pd)`, `cvxPartPatternGeom(1, &shape, &svxPtnGeom, &id)` | linear: `dir[0..1], num[0..1], spacing[0..1], useSecondDir`; circular: `axis{Pnt,Dir}, num[0], angle` (degrees between neighbours). `diameter` of the circular pattern is left at its default. New bodies are found by diffing the shape list. |
| `pattern_*` (feature) | `cvxPartPatternFtr(n, ids, &svxPtnFtr{inpData}, &id)` | `inc = 0, cntDim = 0, pTableVal = NULL`. |
| `mirror` | `cvxPartPlaneNew` (method `VX_PLANE_THREE_POINTS`, origin + 2 points 10 mm along u/v) to create a datum plane, then `cvxPartMirrorGeom` (solid) / `cvxPartMirrorFtr` (features) | One datum plane stays in the part per mirror call. `combine = ADD` also passes the original as `idShapes`. |
| `draft_faces` | `cvxPartDraftInit(VX_FACE_DRAFT)`, `Type = VX_STATION_SYM_FACE_DRAFT`, `idEnt`, `AngleA1`, `StationEnt.stationaryFaces`, `DirP` | Neutral faces = stationary faces. |
| `sweep` | `cvxPartSweepInit`, `cvxPartSweep(&d, NULL, &id)` | Profile = curve list of 3D wire; path = curve list with `idPath` = its first curve, `idParent` = the list, `UseParent = 1`. Profile must be perpendicular to the path start. |
| `loft` | `cvxPartLoftInit`, `cvxPartLoft`, `svxLoftProfile{idParent = curve list, idProfile = its first curve}` | `type = VX_LOFT_PROFS`. |
| variables | `cvxPartInqVars`, `cvxPartVarGet`, `cvxPartVarSet(1, &v, 0)`, `cvxPartVarAdd` | `type = VX_VAR_NUM`, `sub_type = VX_VAR_DST (1) / VX_VAR_ANG (2) / VX_VAR_CONS (0)`, `unit_type = VX_UNIT_MILLIMETER (1) / VX_UNIT_DEG (8)`. Values are database units (mm, deg). |
| sketch variables | `cvxSkActivate(id, 0)` ... `cvxSkInqVars`, `cvxSkVarGet`, `cvxSkVarSet(1, &v)` ... `cvxRootExit()` | Leaving the sketch should regenerate the model. Unknown: whether the dimensions that `cvxSkAddConsDim` creates appear here under usable names (read them from `get_feature_data` on a live feature). |
| inner loops | extra `cvxSkLine2pt / cvxSkArc3pt / cvxSkCirRad` calls in the same sketch | ZW3D must treat nested closed loops of one sketch as a hole. |
| assembly | `cvxFileNewSingle(VX_FILE_ASSEMBLY, VX_NORMAL_ASSEMBLY)`, `cvxCompInsInit`, `cvxCompIns` (`File`, `Part` = file stem, `Frame` translation, `Anchor`), `cvxPartInqComps` | `Part` (root name) is assumed to equal the file stem of a single-root `.Z3PRT`. |
| drawing | `cvxFileNewSingle(VX_FILE_SHEET)`, `cvxDwgViewLayoutInit`, `cvxDwgViewLayout` with `isIgnoreLoc/Method/Attr = 1` | `name` is a `vxPath` (256 bytes): long paths are rejected. The part must already be saved. |

Every modeling method listed in `is_modeling_method()` (`plugin/src/handlers/common.cpp`) runs in one undo transaction and is rolled back on failure.

## Live findings after batch 4 (ZW3D 2025 SP2) and the rework

1. **`cvxPartPlaneNew(VX_PLANE_THREE_POINTS)` always failed (-152)** -> `mirror` could not create its datum plane. Rework: try, in order,
   the standard XY/XZ/YZ method with an offset (axis-aligned planes), `cvxPartPlaneAdd` with `VX_NEW_PLANE_DYNAMIC`,
   `cvxPartPlaneAdd` with `VX_NEW_PLANE_THREE_POINTS`, then the original; the error hint lists every attempt and its code.
2. **`cvxCompIns` failed (-152)** with the full path in `svxCompData.File`. Rework: `Dir` and `File` are separate ("Dir contains File");
   variants tried in order: dir + file name, dir + stem, file name only, full path in File. `Frame` is now a complete matrix (identity rotation
   + translation; the old zeroed rotation with `identity` unchanged was an invalid frame). Error hint lists the attempts. `part_name` (root name) defaults to the file stem.
3. **`cvxXnUndoCount()` is unreliable in a never-saved file** (it lags behind the real log, so `cvxXnBundle(base, ...)` merged the whole history:
   one undo emptied the part; after a save counts were right). Rework: `cvxUndoRedoMarker` (documented to flush the log) brackets every modeling
   call with a start and an end marker; `undo` = `cvxUndoRedoTo(1, start)`, `redo` = `cvxUndoRedoTo(0, end)`; failures roll back with
   `cvxUndoRedoTo(1, start)`. The plugin keeps its own stack (cleared when the active file/root changes); with an empty stack it falls back to one raw
   `cvxUndoRedo` step and says so in `note`. Edits made by hand in ZW3D between tool calls are undone together with the next tool call's step.
4. **Failed `set_variable` left ZW3D without an active root** (following calls: ROOT_ACT_FAIL / WRONG_ROOT_ENV) and returned a misleading
   VARIABLE_NOT_FOUND. Rework: existence is checked against `cvxPartInqVars`; the active root is restored (`cvxRootActivate`) after every modeling call,
   also after failures; a self-referencing expression (`W` = `W*2`) is rejected client-side as a circular reference.
5. **`cvxSkInqVars` returns nothing for the dimensions that `cvxSkAddConsDim` creates.** `list_sketch_variables` now also reports the sketch's
   dimension entities (`cvxSkInqDim`, type) and probes `D1..Dn` with `cvxSkVarGet`; if nothing is found it says so. Open: how a sketch dimension value is
   edited through the API. The header `cvxRootVarSet` documents that fields of history operations are addressed as variables named
   `<operation_name:field_number>`; the vendor example `15.EditVariable` uses `cvxPartInqFtrData` / `cvxDataGet` / `cvxDataSet` for this. A tool built on
   that (edit a feature's variables by name) is the likely way to resize features after the fact.

## Feature parameters by name (get_feature_data / set_feature_value) — NOT yet run on a live ZW3D

Follows the vendor example `15.EditVariable`: `cvxPartInqFtrData(idFeature, 0, &idData)` (falls back to `iNoEval = 1`), `cvxDataGetAll(idData, &n, &svxFldData*)`
(`fld_id`, `fld_name`, `fld_type` = NUM / DST (mm) / ANG (**radians**) / PNT / ENT / TXT / DATA, `fld_data[0]` as `svxData`), freed with `cvxFldDataFree` and `cvxDataFree`.
`svxData.Text` holds the referenced variable name/expression of a numeric field (`isText`).
Editing: `cvxRootVarSet`'s header says a history operation's input field is addressed by a variable named `<operation_name:field_number>`
(brackets included). `set_feature_value` builds that name from `cvxEntName(feature)` + `fld_id` and calls `cvxPartVarSet(1, &v, 0)` with
`type = VX_VAR_NUM`, `sub_type` DST / ANG / CONS (mm / degrees). Unverified: that `cvxEntName` returns the same operation name ZW3D expects
in the variable name, that field ids are stable between the read and the set, and which field ids a given feature uses (read them from `get_feature_data` on a live feature).

## Architecture change (0.3.0): the DLL is the MCP server

The Python MCP server, the TCP/NDJSON bridge and `fake_zw3d.py` were removed. The plugin now serves MCP over HTTP itself
(`plugin/src/server.cpp`: POST `/mcp`, one request per connection, loopback only, Host/Origin check, optional bearer token, chunked and
`Expect: 100-continue` bodies supported; GET/DELETE answer 405 because there is no SSE stream and no session). `plugin/src/mcp/mcp.cpp` implements
`initialize`, `ping`, `tools/list`, `tools/call` (a tool failure is a result with `isError: true` and text `[CODE] message (api_code=N) Hint: ...`;
only an unknown tool is a JSON-RPC error). `plugin/src/mcp/tools.cpp` holds the 45 tools (schema, description, and the argument validation /
translation that used to live in Python); `plugin/src/mcp/validate.cpp` the geometry checks. The handlers and the main-thread dispatcher are unchanged;
the plugin-internal request envelope `{"ok":..,"result"|"error"}` is now only an in-process contract between the tool table and the handlers.
Screenshots: the tool layer reads the temp PNG, returns it as an MCP image and always deletes it (only `shot_*.png` in `%TEMP%\zw3d_mcp`).
The MCP/validation/HTTP layers were exercised offline with a throwaway stub host (not kept in the repository); everything that talks to ZW3D is as verified/unverified as listed above.

## Live findings on the HTTP build (0.3.0) and the fixes

1. **`set_feature_value` on a derived field "succeeds" but changes nothing** (the length of a box built from two corner points: reported OK, bbox unchanged).
   `cvxPartVarSet` does not report this. Now: the field is read back after the set (`FIELD_NOT_APPLIED` if it differs) and the geometry fingerprint
   (`shapes_signature()`: volume, area, bbox of every solid, ids ignored) is compared before/after (`NO_EFFECT` if nothing changed although the value did);
   both throw, so the transaction wrapper rolls the attempt back.
2. **Regeneration renumbers every shape id** (a box 137 became 4230 after one `set_feature_value`; face/edge ids change too). Ids cannot be compared across a
   regeneration, hence the fingerprint above. Results of `set_feature_value` / `set_variable` / `set_sketch_variable` now carry a `note` (and
   `ids_renumbered` for features) and the tool descriptions say to use only the ids in the returned `shape_details`.
3. **Mirror with a plane that coincides with the original returns success and produces nothing.** Now `MIRROR_NO_RESULT` (rolled back) when no new solid appears
   (`combine = none`) or the geometry fingerprint is unchanged (`combine = add`, feature mirror).
4. **A negative value in a distance field silently flips the feature** (extrusion reversed, no error). `set_feature_value` refuses negative distances with
   `NEGATIVE_DISTANCE` unless `allow_negative=true`.
