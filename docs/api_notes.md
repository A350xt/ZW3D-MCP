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
  2. Copy the DLL to `%APPDATA%\ZWSOFT\ZW3D\ZW 3D 2025\custom\apilibs` (scanned at start-up). CMake option `-DZW3D_MCP_INSTALL_USER=ON` does this after each build.
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
