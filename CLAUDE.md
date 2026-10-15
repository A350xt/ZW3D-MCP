# ZW3D-MCP 开发文档

> 本文件是给 Claude Code 的项目说明。放在仓库根目录，Claude Code 会自动读取。
> 目标软件：**中望3D 2025（ZW3D 2025）**。注意不是中望CAD，两者的二次开发接口完全不同。

---

## 1. 项目目标

做一个 MCP Server，让 AI Agent（Claude Code、Claude Desktop、Cursor 等）通过自然语言操纵**正在运行的中望3D 2025** 完成零件建模，并且能把结果（截图、质量属性、包围盒等）回传给 AI，形成"建模 → 查看 → 修正"的闭环。

第一阶段只做零件（Part）建模，不做装配、工程图、CAM。

---

## 2. 必须先理解的约束

1. **中望3D 只提供 C/C++ API。** 没有 COM/ActiveX、没有 VBA、没有 Python、没有 .NET。
   网上的"中望 MCP"项目（ZWCAD-Mechanical-MCP、zwcad-standard-mcp 等）都是针对**中望CAD**的，靠 COM 从外部进程调用，**这条路在中望3D上走不通，不要参考它们的实现方式**。
2. 所以必须写一个 **C++ 插件 DLL**，由中望3D加载、运行在中望3D进程内部，再通过本地 socket 与外部的 Python MCP Server 通信。思路与 Blender MCP（插件内起 socket + 外部 MCP 进程）相同。
3. **API 调用一律在中望3D主线程执行。**（这是 CAD 软件的通用规则，官方文档未明确说明，按最保守方式处理。）socket 收发在后台线程，执行必须投递回主线程。
4. 中望3D内部字符串可能是 **GBK/系统 ANSI 编码**，而 JSON 必须是 UTF-8。所有进出 API 的字符串（文件名、变量名、消息）都要在桥接层转换。
5. 中望3D 2025 起官方建议优先使用新版接口（`Zw*` 函数、`szw*` 结构体），旧的 `cvx*` 接口不再维护，但仍可用。**MVP 阶段优先用签名已核实的 `cvx*` 接口跑通链路，之后再逐步替换为新接口。**

---

## 3. 总体架构

```
AI Agent (Claude Code / Claude Desktop / Cursor)
   │  MCP 协议 (stdio)
   ▼
[A] Python MCP Server  (mcp 官方 Python SDK / FastMCP)
   │  - 定义 MCP 工具、参数 schema、文档字符串
   │  - 参数校验、单位说明、把截图 PNG 作为 Image 返回
   │  TCP 127.0.0.1:47823，每行一个 JSON（NDJSON）
   ▼
[B] ZW3D 插件 DLL (C++, x64, MSVC)
   │  - 后台线程：socket 收发、JSON 解析
   │  - 主线程：执行 API 调用（通过隐藏窗口消息投递）
   ▼
中望3D 2025 进程（cvx* / Zw* C API）
```

另外还要有：

- **[C] fake_zw3d.py**：一个用 Python 写的假插件，监听同一端口、实现同一协议、返回假数据。用于在**没有中望3D的环境**（包括 Linux）下开发和测试 [A]。

### 推荐目录结构

```
zw3d-mcp/
├── CLAUDE.md                  ← 本文件
├── plugin/                    ← [B] C++ 插件
│   ├── CMakeLists.txt
│   ├── src/
│   │   ├── plugin_main.cpp    ← <DllName>Init / <DllName>Exit、命令注册
│   │   ├── server.cpp/.h      ← Winsock 服务器（后台线程）
│   │   ├── dispatcher.cpp/.h  ← 主线程调度（消息窗口 + 任务队列）
│   │   ├── handlers/          ← 每类命令一个文件：part.cpp、query.cpp、file.cpp…
│   │   ├── encoding.cpp/.h    ← UTF-8 ↔ ANSI/GBK
│   │   └── errors.cpp/.h      ← evxErrors / ZW_API_* → 字符串
│   └── third_party/json.hpp   ← nlohmann/json（单头文件，MIT）
├── server/                    ← [A] Python MCP Server
│   ├── pyproject.toml
│   ├── zw3d_mcp/
│   │   ├── server.py          ← MCP 工具定义
│   │   └── bridge.py          ← TCP 客户端、超时、重连
│   └── tests/
│       ├── fake_zw3d.py       ← [C]
│       └── test_tools.py
└── docs/
    └── api_notes.md           ← 开发中核实过的 API 用法记录（持续追加）
```

---

## 4. 开发环境

| 项 | 要求 |
|---|---|
| 操作系统 | Windows 10/11 x64（插件只能在 Windows 上编译和运行） |
| 编译器 | Visual Studio 2022（MSVC，x64，Release 和 Debug 都要能编） |
| 构建 | CMake ≥ 3.20 |
| 中望3D | 2025 版本，已安装且能正常启动 |
| SDK | 中望3D安装目录下的 API 头文件和库：主头文件 **`VxApi.h`**，链接库 **`ZW3D.lib`**。用 CMake 变量 `ZW3D_SDK_DIR` 指向它，不要写死路径 |
| Python | 3.10+，依赖 `mcp`（官方 SDK），测试用 `pytest` |

**第一件事**：让用户在本机找到 `VxApi.h` 和 `ZW3D.lib` 的实际路径，以及 SDK 自带的示例工程（如果有），记录到 `docs/api_notes.md`。

CMake 要点：

- 生成 `SHARED` 库，x64，`/utf-8` 编译选项（源码 UTF-8）。
- 运行库（`/MD` 或 `/MT`）、Qt 版本（如果用到 Qt）必须和中望3D本体一致；默认**不要引入 Qt**，见第 5.2 节。
- DLL 文件名决定入口函数名（见 5.1），CMake 里固定 `OUTPUT_NAME`。

---

## 5. 插件 [B] 设计

### 5.1 生命周期

- 中望3D加载插件 DLL 时调用 **`<DLL名>Init`**，卸载时调用 **`<DLL名>Exit`**。例如 DLL 叫 `zw3dmcp.dll`，就导出 `zw3dmcpInit()` 和 `zw3dmcpExit()`（`extern "C" __declspec(dllexport)`，返回 int）。确切签名以 2025 开发指南和 SDK 示例为准。
- 在 `Init` 里：
  1. 用 `cvxCmdFunc` 注册两个命令：`McpStart`、`McpStop`（让用户能在中望3D里手动开关服务）。
  2. 创建主线程调度器（5.2）。
  3. 默认自动启动 socket 服务（可通过环境变量 `ZW3D_MCP_AUTOSTART=0` 关闭）。
  4. `cvxMsgDisp` 输出一行：`[ZW3D-MCP] listening on 127.0.0.1:47823`。
- 在 `Exit` 里：停止 socket 线程、销毁消息窗口、`cvxCmdFuncUnload` 卸载命令。必须能干净退出，不能让中望3D卡死在关闭阶段。
- 插件如何加载（菜单入口、是否支持开机自动加载）：查 2025 开发指南，查到后写进 `docs/api_notes.md` 和 README。

### 5.2 线程模型（最关键）

不要依赖 Qt。用纯 Win32 实现主线程投递：

1. `Init` 运行在主线程。在这里 `CreateWindowEx(... HWND_MESSAGE ...)` 创建一个**仅消息窗口**，记录其 `HWND`。
2. 后台线程收到请求后：把 `{request, std::promise<json>}` 放入线程安全队列 → `PostMessage(hwnd, WM_APP_MCP_JOB, 0, 0)` → 在 `future` 上等待，超时（默认 60 秒，可由请求参数覆盖）返回 `TIMEOUT` 错误。
3. 窗口过程（主线程）收到 `WM_APP_MCP_JOB` 后取空队列，逐个执行 handler，`set_value` 回结果。
4. handler 内部必须 `try/catch` 全部异常，任何异常都转成错误响应，**绝不能让异常穿出窗口过程**。
5. 同一时刻只处理一个请求（串行），不做并发。
6. 需要验证的风险：中望3D处于模态对话框、或用户正在执行某个命令时，消息是否仍会被派发；API 在"命令进行中"调用是否安全。如有问题，handler 执行前检查状态并返回 `BUSY` 错误。

### 5.3 Socket 服务

- Winsock，只绑定 `127.0.0.1`，端口默认 `47823`，可用环境变量 `ZW3D_MCP_PORT` 修改。
- 同时只接受一个客户端；新连接到来时断开旧连接。
- 帧格式：**每行一个 UTF-8 JSON**，以 `\n` 结尾。单条消息上限 16 MB。
- 不做鉴权（仅本机回环），但要在 README 里说明风险。

### 5.4 编码

`encoding.cpp` 提供 `std::string utf8_to_ansi(const std::string&)` 和 `ansi_to_utf8(...)`，用 `MultiByteToWideChar`/`WideCharToMultiByte` 经由 UTF-16 转换，代码页用 `CP_ACP`。所有传入 API 的 `char*`（路径、变量名、表达式）先转 ANSI；所有从 API 读出的字符串先转 UTF-8 再放进 JSON。**路径中含中文的情况必须测试。**

### 5.5 错误处理

- API 返回 `evxErrors`（0 为成功）。新接口返回 `ZW_API_*` 系列错误码（如 `ZW_API_INVALID_INPUT`、`ZW_API_WRONG_ROOT_ENV`、`ZW_API_CMMD_EXEC_ERROR`）。
- `errors.cpp` 把错误码映射成**英文枚举名 + 中文说明 + 给 AI 的修复建议**，例如：
  `{"code":"WRONG_ROOT_ENV","message":"当前不在零件建模环境","hint":"先调用 new_part 或打开一个零件"}`。
  这些提示会直接返回给 AI，写清楚能大幅减少 AI 的无效重试。
- 批量建模时用 `cvxDispState` 暂停刷新，结束后恢复并 `cvxDispRedraw`；无论成功失败都要恢复（RAII 守卫）。

---

## 6. 通信协议（[A] ↔ [B]，fake_zw3d.py 也必须遵守）

请求：

```json
{"id": "uuid-或递增整数", "method": "extrude", "params": {...}, "timeout_ms": 60000}
```

成功响应：

```json
{"id": "...", "ok": true, "result": {...}}
```

失败响应：

```json
{"id": "...", "ok": false, "error": {"code": "INVALID_INPUT", "api_code": -31, "message": "...", "hint": "..."}}
```

约定：

- **单位统一为毫米和度**，在每个 MCP 工具的描述里写明。
- 所有实体用中望3D返回的整数 ID 引用（`idShape`、`idEnt` 等），在响应里统一命名为 `shape_id`、`curve_id`、`feature_id`。
- 坐标点统一用 `[x, y, z]` 数组。
- 新增 method 时，**同时**更新：C++ handler、Python 工具、fake_zw3d.py、本文件第 7 节。

---

## 7. MCP 工具清单

### 7.1 MVP（第一里程碑必须完成）

| MCP 工具 | 作用 | 主要 API（签名见第 8 节） |
|---|---|---|
| `ping` | 检查插件连通、返回插件版本、中望3D当前激活文件名 | `cvxFileInqActive`、`cvxRootInqActive` |
| `new_part` | 新建零件文件并激活 | `cvxFileNew`（零件类型与命名规则待核实） |
| `create_box` | 按两角点或中心+尺寸建长方体 | `cvxPartBox` + `svxBoxData` |
| `create_cylinder` | 圆柱 | `cvxPartCyl` + `svxCylData` |
| `extrude_polygon` | 给定平面多边形顶点，拉伸成实体 | `cvxPartLine2pt` × N → `cvxPartCrvList` → `cvxPartExtrudeInit` / `cvxPartExtrude` |
| `revolve_polygon` | 给定轮廓顶点和旋转轴，旋转成实体 | 同上 + `cvxPartRevolveInit` / `cvxPartRevolve` |
| `list_shapes` | 列出当前零件所有实体 ID | `cvxPartInqShapes` + `cvxMemFree` |
| `get_mass_props` | 体积、表面积、质量、重心 | `cvxPartInqShapeMass` + `svxMassProp` |
| `get_bounding_box` | 包围盒 | `cvxPartInqShapeBox` + `svxBndBox` |
| `screenshot` | 导出当前视图 PNG；Python 端读文件后作为 MCP Image 返回 | `cvxDispZoomAll` → `cvxFileExportInit(VX_EXPORT_TYPE_IMG, VX_EXPORT_IMG_TYPE_PNG, &data)` → `cvxFileExport` |
| `save_part` / `export_step` | 保存 / 另存为 STEP | `cvxFileSave`、`cvxFileSaveAs`、`cvxFileExport(STEP)` |

`screenshot` 的实现要点：插件把 PNG 写到 `%TEMP%\zw3d_mcp\shot_<时间戳>.png`，返回路径；Python 端读取、转 base64、作为 MCP `ImageContent` 返回，并删除临时文件。默认 800×600。

### 7.2 第二阶段

| MCP 工具 | 主要 API |
|---|---|
| `boolean`（并/差/交） | `cvxPartBool(evxBoolType, idBase, count, shapes, keep)` |
| `fillet_edges` | `cvxPartFillet(count, edges, radius)`，需要先有"查边"工具 |
| `list_faces` / `list_edges` | `cvxPartInqShapeFaces` 及对应的边查询接口 |
| `create_sphere` | `cvxPartSphere` + `svxSphereData` |
| `create_circle` / `create_arc`（作为轮廓） | `cvxPartCircle`、`cvxPartArc` |
| `list_features` | `cvxPartInqFtrList` |
| `get_variables` / `set_variable`（参数化改尺寸） | `cvxPartInqVars`、`cvxPartVarSet`、`cvxPartVarAdd` |
| `undo` | 待查：是否有 undo 接口，否则用 `cvxCmdSend` 发送撤销命令 |
| `run_command`（兜底） | `cvxCmdSend(CommandString)`：发送中望3D命令字符串。默认关闭，需环境变量显式开启，因为行为不可控 |

### 7.3 工具设计原则

- 中等粒度：一个工具完成一个"设计意图"（如"拉伸一个多边形"），而不是把每个底层 API 一对一暴露。
- 每个创建类工具都返回新实体 ID，并**附带该实体的包围盒和体积**，让 AI 无需额外调用就能自检。
- 工具描述（docstring）里写清单位、坐标系约定、常见错误和下一步建议。
- 参数用 JSON Schema 严格约束（数值范围、数组长度），在 Python 端先校验，减少无意义的往返。

---

## 8. 已核实的 API 签名

以下签名取自 `rust-zw3d-raw` crate 中由 bindgen 直接从 `VxApi.h` 生成的绑定文件（`src/bindings_default.rs`，共 2856 个函数）。**该绑定对应的中望3D版本未注明，可能不是 2025。实现前必须对照本机 2025 的 `VxApi.h` 再确认一次**，确认后把结论写进 `docs/api_notes.md`。

```c
/* 返回值类型 evxErrors：0 为成功 */
evxErrors cvxCmdFunc(const char *Name, void *Function, double Code);   /* Code 常用 VX_CODE_GENERAL */
evxErrors cvxCmdFuncUnload(const char *Name);
void      cvxCmdSend(const char *CommandString);
void      cvxMsgDisp(const char *Text);
void      cvxMemFree(void **MemPointer);           /* 释放 API 分配的数组，注意传的是指针的地址 */

void      cvxDispState(int Option, int *State);    /* 暂停/恢复界面刷新 */
void      cvxDispRedraw(void);
void      cvxDispZoomAll(int Redraw);

evxErrors cvxFileNew(const char *Name);
evxErrors cvxFileOpen(const char *Name);
evxErrors cvxFileSave(int Close);
evxErrors cvxFileSaveAs(const char *Path);
void      cvxFileClose(void);
void      cvxFileInqActive(char *Name, int nBytes);
evxErrors cvxRootActivate(char *Name);
void      cvxRootInqActive(char *Name, int nBytes);
evxErrors cvxFileExportInit(evxExportType type, int imgType, void *data);
evxErrors cvxFileExport(evxExportType Type, const char *Path, void *Data);

evxErrors cvxPartBox(svxBoxData *Box, int *idShape);
evxErrors cvxPartCyl(svxCylData *Cyl, int *idShape);
evxErrors cvxPartSphere(svxSphereData *Sphere, int *idShape);
evxErrors cvxPartExtrudeInit(svxExtrudeData *data);
evxErrors cvxPartExtrude(svxExtrudeData *Ext, int *idShape);
evxErrors cvxPartRevolveInit(svxRevolveData *data);
evxErrors cvxPartRevolve(svxRevolveData *Rev, int *idShape);
evxErrors cvxPartFillet(int Count, int *Edges, double Radius);
evxErrors cvxPartBool(evxBoolType Type, int idBase, int Count, int *Shapes, int Keep);

evxErrors cvxPartPnt(...);                          /* 签名见头文件 */
evxErrors cvxPartLine2pt(svxPoint *Start, svxPoint *End, int *idEnt);
evxErrors cvxPartCircle(const svxCircleData *circleData, int *idCircle);
evxErrors cvxPartArc(const svxArcData *arcData, int *idArc);
evxErrors cvxPartCrvList(int Count, int *Curves, int *idEnt);   /* 多条曲线组成轮廓 */

evxErrors cvxPartInqShapes(const char *file, const char *part, int *count, int **idShapes);
evxErrors cvxPartInqShapeFaces(int idShape, int *count, int **idFaces);
evxErrors cvxPartInqShapeBox(int idShape, svxMatrix *mat, svxBndBox *box);
evxErrors cvxPartInqShapeMass(int idShape, double density, svxMassProp *prop);
evxErrors cvxPartInqFtrList(int *Count, int **Features);

evxErrors cvxPartInqVars(const char *File, const char *Part, int *Count, svxVariable **Variables);
evxErrors cvxPartVarAdd(svxVariable *Variable);
evxErrors cvxPartVarGet(svxVariable *Variable);
evxErrors cvxPartVarSet(int Count, svxVariable *Variables, int Working);
evxErrors cvxSkVarAdd(svxVariable *Variable);
```

待核实的问题（实现到对应工具时逐条查清，写进 `docs/api_notes.md`）：

- `cvxPartInqShapes` 的 `file`/`part` 传 `NULL` 是否表示"当前激活零件"。
- `cvxFileNew` 新建零件时文件名/类型的写法。
- `svxBoxData`、`svxCylData`、`svxExtrudeData`、`svxRevolveData`、`svxImgData`、`svxMassProp`、`svxBndBox` 各字段含义和默认值（`*Init` 函数是否已填好合理默认值）。
- `svxExtrudeData` 中轮廓、方向、布尔方式（`Combine`）、起止类型字段的正确填法。
- STEP 导出对应的 `evxExportType` 枚举值和数据结构（绑定中有 `svxSTEPData`）。
- 新接口 `szwCenterBoxData`、`szwExtrudeData` 对应的创建函数名，作为后续替换目标。

### 已知可用的示例代码（来自 CSDN，可作写法参考）

旋转体 + 质量属性（旧接口）：

```cpp
svxPoint P1 = {10,0,0}, P2 = {20,0,0}, P3 = {20,10,0}, P4 = {10,10,0};
int L[4];
cvxPartLine2pt(&P1, &P2, &L[0]);
cvxPartLine2pt(&P2, &P3, &L[1]);
cvxPartLine2pt(&P3, &P4, &L[2]);
cvxPartLine2pt(&P4, &P1, &L[3]);
int idList = 0;
cvxPartCrvList(4, L, &idList);

svxRevolveData rev;
cvxPartRevolveInit(&rev);
rev.idProfile  = idList;
rev.Axis.Pnt   = {0,0,0};
rev.Axis.Dir   = {0,1,0};
rev.Combine    = VX_BOOL_NONE;
rev.StartType  = VX_INPU_VALUE;  rev.Start = 0.0;
rev.EndType    = VX_INPU_VALUE;  rev.End   = 180;
rev.OffsetTyp  = VX_OFFSET_NONE;
int idShape = 0;
cvxPartRevolve(&rev, &idShape);

svxMassProp prop;
cvxPartInqShapeMass(idShape, 1.0, &prop);   // prop.Volume / prop.Area / prop.Mass / prop.Center
```

导出 PNG 截图：

```cpp
svxImgData data;
cvxFileExportInit(VX_EXPORT_TYPE_IMG, VX_EXPORT_IMG_TYPE_PNG, &data);
data.Type       = VX_EXPORT_IMG_TYPE_PNG;
data.RangeMode  = VX_EXPORT_IMG_RANGE_MODE_NORMAL;
data.ColorMode  = VX_EXPORT_IMG_COLOR_MODE_24BITS;
data.BkgndMode  = VX_EXPORT_IMG_BKGND_MODE_CURRENT;
data.Quality    = 90;
data.Width      = 800;
data.Height     = 600;
cvxFileExport(VX_EXPORT_TYPE_IMG, path_ansi, &data);
```

---

## 9. 参考资料（按优先级）

1. **本机 SDK 头文件**：`VxApi.h` 及其包含的 `zwapi_*.h`（新接口按模块分文件，如 `zwapi_cmd_shape.h`、`zwapi_part_*.h`、`zwapi_file*.h`、`zwapi_brep_*.h`）。**这是唯一权威来源**，任何签名、结构体字段、枚举值以此为准。用 grep 查，不要猜。
2. **中望3D 2025 官方文档**（从中文官网"中望3D开发文档"页下载）：
   - 开发指南（API introduction）：环境配置、插件加载方式、入口函数约定。
   - 参考手册：英文站的 2025 版是 `https://dl.zwsoft.com/support/ZW3D/2025/ApiHelpDoc.chm`。
   - 入口页：https://www.zwsoft.cn/support/zw3d-devdoc 、https://www.zwsoft.com/support/zw3d-devdoc
3. **SDK 自带示例工程**（如有）：看插件工程配置、入口函数写法。
4. **rust-zw3d 项目**：https://gitee.com/rust-cad/rust-zw3d 。其中 `rust-zw3d-raw` 的 `src/bindings_default.rs` 是 bindgen 从 `VxApi.h` 生成的完整绑定，可以在**没有 SDK 的机器上**快速 grep 函数签名和结构体布局（下载：`https://static.crates.io/crates/rust-zw3d-raw/rust-zw3d-raw-0.1.0-alpha.12.crate`，是 tar.gz）。它的 `wrapper.h` 列出了全部 `zwapi_*.h` 头文件名。注意版本可能与 2025 不同，只作参考。
5. **中望3D 2025 自带的 API 助手**（AI 功能）：用自然语言生成代码片段，适合查新接口函数名。让用户在中望3D里问，再把结果贴回来。
6. **CSDN 二次开发文章**（写法参考，质量参差，以头文件为准）：
   - 2025 + VS2022 环境配置、新接口建方块：https://blog.csdn.net/weixin_62524256/article/details/153876261
   - 函数查找技巧（cvx 缩写规律）：https://blog.csdn.net/m0_46569197/article/details/146073943
   - 质量属性 / 旋转：https://blog.csdn.net/qq_32460115/article/details/139547059
   - 导出图片：https://blog.csdn.net/m0_46569197/article/details/139684068
   - 方程式/变量：https://blog.csdn.net/weixin_38877807/article/details/138073184
7. **MCP 官方 Python SDK**：https://github.com/modelcontextprotocol/python-sdk （工具定义、ImageContent 返回）。
8. **架构参考**：blender-mcp（https://github.com/ahujasid/blender-mcp）——插件内 socket + 外部 MCP 进程的模式。只参考架构，不参考 API。

---

## 10. 里程碑与验收标准

每个里程碑结束时，在 `docs/api_notes.md` 记录踩过的坑和核实结论。

**M0：环境确认**
- 找到 `VxApi.h`、`ZW3D.lib` 路径；确认 2025 插件加载方式和入口函数约定。
- 验收：一个只调用 `cvxMsgDisp("hello")` 的空插件能被中望3D 2025 加载并显示消息。

**M1：通信链路**
- 插件：消息窗口调度器 + socket 服务 + `ping`。
- Python：bridge.py + `ping` 工具；fake_zw3d.py 实现 `ping`。
- 验收：在 Claude Code 里调用 `ping`，返回中望3D当前激活文件名；中望3D界面不卡顿；关闭中望3D不卡死。

**M2：MVP 建模闭环**
- 实现 7.1 全部工具。
- 验收：对 AI 说"建一个 100×60×20 的底板，中间拉伸一个直径 30 高 40 的圆柱，然后截图给我看并告诉我总体积"，AI 能独立完成，截图正确，体积与手算一致（误差 < 0.1%）。

**M3：第二阶段工具 + 健壮性**
- 实现 7.2；补全错误映射和 hint；中文路径测试；超时与断线重连测试。
- 验收：故意传错误参数（负尺寸、不闭合轮廓），AI 收到可理解的错误并能自行修正。

**M4（可选）：替换为 2025 新接口**
- 用 `Zw*` / `szw*` 接口替换已弃用的 `cvx*` 调用。

---

## 11. 测试

- **Python 层**：`pytest` + fake_zw3d.py，在任意系统上跑；覆盖每个工具的参数校验、正常路径、错误路径、超时。
- **协议一致性**：用一个 JSON 用例文件（请求 + 期望响应结构），同时跑 fake 和真插件。
- **插件层**：无法在 CI 上跑。提供 `tools/smoke_test.py`：连上真实插件，依次调用所有 method，打印结果和耗时；由用户在装有中望3D的机器上手动执行，把输出贴回来。
- 每次修改 C++ 后，提醒用户：关闭中望3D → 重新编译 → 重新加载插件（DLL 被占用时无法覆盖）。

---

## 12. 给 Claude Code 的工作规则

1. **不要猜 API。** 任何 `cvx*` / `Zw*` 函数、结构体字段、枚举值，写代码前必须在头文件（或第 9 节第 4 条的绑定文件）里 grep 到原文。查不到就停下来问用户，或让用户在中望3D的 API 助手里查。
2. 每核实一个 API，把签名、字段含义、注意事项追加到 `docs/api_notes.md`。
3. 插件代码只能在用户的 Windows 机器上编译。需要编译或测试时，给出明确的命令和操作步骤，让用户执行并贴回输出；不要假设编译通过。
4. 新增工具时同步修改四处：C++ handler、Python 工具、fake_zw3d.py、本文件第 7 节。
5. 所有 handler 必须：主线程执行、捕获全部异常、恢复显示状态、释放 API 分配的内存（`cvxMemFree`）。
6. 先跑通最小链路（M0、M1），再扩展工具。不要一次性写完所有工具再调试。
7. 遇到"中望CAD"相关资料（ZWCAD、LISP、ZRX、COM），一律忽略，那是另一个产品。
