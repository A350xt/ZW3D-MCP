# ZW3D API notes (2025+)

Everything under "Verified" was read directly from the local SDK headers
(`C:\Program Files\ZWSOFT\ZW3D 2025\api\inc\*.h`, 237 files) or the vendor examples. "Unverified" items can only be
confirmed by running `tools/smoke_test.py` inside ZW3D and are the first thing to check on a new machine.

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

## Unverified on a live ZW3D — check with `tools/smoke_test.py`

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

2025-10-15, ZW3D 2025 SP2 x64: `tools/smoke_test.py` -> ALL STEPS PASSED.
Confirmed: items 1 (handlers + Init on the same UI thread), 3 (`cvxFileNewSingle` -> `.Z3PRT` part, Chinese names), 5 (STEP export of a part file),
6 (PNG export works, ~130 ms), 7 (fillet, boolean keeps the base id), 8 (Chinese Save As path), 9 (mass units, see above).
Still open: 2 (modal dialog / command in progress), 4 (default extrusion direction when `direction` is omitted),
and whether PNG honours Width/Height.

## Batch 2 (read from headers; NOT yet run on a live ZW3D — run `tools/smoke_test.py`)

- Arc/circle profiles: `cvxPartArcInit/cvxPartArc` with `arcType = VX_ARC_THROUGH` (start, end, through as `svxPntOnEntPath` with
  `pntType = VX_FREE_POINT` and `pnt`), and `cvxPartCircleInit/cvxPartCircle` with `type = VX_CIRCLE_THROUGH` and 3 `bndPnts`.
  Three free points make both plane-independent. `svxPntOnEntPath` is zeroed with memset before use.
- Chamfer: `cvxPartChamferAllInit(svxChamFlltData*)`, `type = VX_CHAMFLLT_SYMMETRIC | VX_CHAMFLLT_ASYMMETRIC`, `nEdges`, `idEdge`, `setback1/2`; `cvxPartChamferAll(&data, &idFeature)`.
- Shell: `cvxPartShellInit(svxShellData*)`, `idShape`, `dThick`, `cntOpenFace`, `pIdOpenFace`; `cvxPartShell(&data, &idFeature)`.
- Undo/redo: `cvxUndoRedo(int Undo)` (1 = undo, 0 = redo), one operation per call. Open question: whether one MCP tool call
  (e.g. a polygon extrude = N lines + curve list + extrude) is one undo step or several.
- Edge bbox: `cvxPartInqEntBox(idEdge, NULL, &box)`; face surface: `cvxPartInqFaceSrfPrim` (`srfType`: `VX_SF_PRIM_PLN/CYL/...`,
  plane normal = z column `zx,zy,zz` of `pln.form`, sign not guaranteed outward; free NURBS with `cvxSurfaceFree`).

## Batch 3 (from user feedback; headers-based, verify with `tools/smoke_test.py`)

- **Sketches**: `cvxPartSkNew(svxMatrix *Plane, int *idSketch)` creates AND activates a sketch; geometry via `cvxSkLine2pt`,
  `cvxSkArc3pt(Start, End, Mid)` (note the order), `cvxSkCirRad(Center, R)` with `svxPoint2` in sketch coordinates;
  `cvxSkAutoAddCoinCons(-1)` joins end points; `cvxSkAddConsDim(type, svxConsDim*)` adds driving dimensions (linear aligned = sub
  type 3, radial diameter = sub type 2; best effort, failures are counted not fatal); leave with `cvxRootExit()` (RAII guard).
  The sketch id is then passed as `idProfile` to `cvxPartExtrude` / `cvxPartRevolve`. `svxMatrix` columns: x = (xx,xy,xz),
  y = (yx,yy,yz), z = (zx,zy,zz), origin = (xt,yt,zt) — field names are `<axis><component>`.
- **Undo granularity / failure cleanup**: every modeling handler runs inside `invoke_handler`: `base = cvxXnUndoCount()`;
  on success `cvxXnBundle(base, method)` merges all steps of the call into one; on failure `cvxUndoRedo(1)` is repeated until the
  count is back at `base`, so no loose lines/sketches remain. Open: whether `cvxXnUndoCount` counts exactly the steps `cvxUndoRedo`
  undoes (the smoke test checks "one undo removes exactly one tool call" and "failed calls leave no step").
- **Round edge boxes**: `cvxEntBndBox` returns a loose box for circles/arcs (R6 circle gave 18x20.8); `list_edges` now rebuilds the
  box from the curve (frame origin/axes, `R`, `A1..A2` degrees; lines from `P1`/`P2`).
- `api_code` is `null` for errors that do not come from a ZW3D API call (0 reads as success).
- Revolve crossing the axis is rejected client-side (profile on both sides of the axis); sketch revolve axis is 2D in sketch frame.
- STEP files written with a Chinese file name show garbled bytes in the header comment (ZW3D's translator writes GBK); geometry is unaffected.

## Batch 4 (read from headers; NOT yet run on a live ZW3D — `tools/smoke_test.py` has a check for each)

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
| sketch variables | `cvxSkActivate(id, 0)` ... `cvxSkInqVars`, `cvxSkVarGet`, `cvxSkVarSet(1, &v)` ... `cvxRootExit()` | Leaving the sketch should regenerate the model. Unknown: whether the dimensions that `cvxSkAddConsDim` creates appear here under usable names (the smoke test prints them). |
| inner loops | extra `cvxSkLine2pt / cvxSkArc3pt / cvxSkCirRad` calls in the same sketch | ZW3D must treat nested closed loops of one sketch as a hole. |
| assembly | `cvxFileNewSingle(VX_FILE_ASSEMBLY, VX_NORMAL_ASSEMBLY)`, `cvxCompInsInit`, `cvxCompIns` (`File`, `Part` = file stem, `Frame` translation, `Anchor`), `cvxPartInqComps` | `Part` (root name) is assumed to equal the file stem of a single-root `.Z3PRT`. |
| drawing | `cvxFileNewSingle(VX_FILE_SHEET)`, `cvxDwgViewLayoutInit`, `cvxDwgViewLayout` with `isIgnoreLoc/Method/Attr = 1` | `name` is a `vxPath` (256 bytes): long paths are rejected. The part must already be saved. |

Every modeling method listed in `is_modeling_method()` (`plugin/src/handlers/common.cpp`) runs in one undo transaction and is rolled back on failure.
