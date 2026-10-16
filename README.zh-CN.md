# ZW3D-MCP

[English](README.md) | **简体中文**

让 AI 智能体（Claude Code、Cursor 等）在**正在运行的中望3D 2025+** 里建模，并查看结果（截图、体积、包围盒）。
目标软件是中望3D，**不是**中望CAD（ZWCAD）。

整个 MCP 服务端就是**一个 DLL**，运行在中望3D 进程内，通过 HTTP 提供 MCP。不需要 Python、Node 或任何额外进程。

```
AI 智能体 ── MCP over HTTP: POST http://127.0.0.1:47823/mcp ──▶ zw3dmcp.dll（在中望3D 内）
```

## 快速开始

1. 从 [Releases](../../releases) 下载 `zw3d-mcp-vX.Y.Z-win64.zip` 并解压。
2. **关闭中望3D**，把 `zw3dmcp.dll` 复制到 `%APPDATA%\ZWSOFT\ZW3D\ZW3D 2025\custom\apilibs`（没有该文件夹就新建；新版本把年份换掉）。
3. 启动中望3D，输出窗口应显示 `[ZW3D-MCP] MCP endpoint: http://127.0.0.1:47823/mcp`。没有的话运行命令 `McpStart`。
4. 在客户端里注册这个地址：

   ```bash
   claude mcp add --transport http zw3d http://127.0.0.1:47823/mcp
   ```

   Cursor：`{"mcpServers": {"zw3d": {"url": "http://127.0.0.1:47823/mcp"}}}`。
   只支持 stdio 的客户端需要桥接，例如 `npx -y mcp-remote http://127.0.0.1:47823/mcp`（未验证）。
5. 对 AI 说：“ping 一下中望3D”。

详细步骤与常见问题见 [docs/INSTALL.md](docs/INSTALL.md)。命令 `McpStart` / `McpStop` 可在中望3D 里开关服务。

### 配置（环境变量，在启动中望3D 之前设置）

| 变量 | 作用 |
|---|---|
| `ZW3D_MCP_PORT` | 监听端口，默认 47823 |
| `ZW3D_MCP_AUTOSTART=0` | 不自动监听，需手动 `McpStart` |
| `ZW3D_MCP_TOKEN` | 设置后，所有请求必须带 `Authorization: Bearer <token>` |
| `ZW3D_MCP_ENABLE_RUN_COMMAND=1` | 开启原始命令工具 `run_command`（默认关） |

### 安全

只监听 `127.0.0.1`，并拒绝 `Host` 或 `Origin` 不是本机回环的请求，网页无法借你的浏览器操作中望3D。
不设 `ZW3D_MCP_TOKEN` 时，本机任何进程都能调用（开启后包括 `run_command`）。不要把端口暴露到网络。

## 工具

单位为**毫米和度**；点用 `[x, y, z]`。建模类工具会返回受影响的实体及其包围盒和体积。
出错时返回 `[CODE] message  Hint: …`，方便智能体自行修正。每次建模调用是一个撤销步骤，失败时完全回滚。

状态：✅ 已在真实的中望3D 2025 SP2 上测试 · ⚠️ 按 SDK 头文件实现，尚未完全实机验证（见 [docs/api_notes.md](docs/api_notes.md)）。

**会话与文件**

| 工具 | 作用 | 主要参数 | 状态 |
|---|---|---|---|
| `ping` | 检查连接；插件版本、当前文件与零件 | – | ✅ |
| `new_part` | 新建单根零件（`.Z3PRT`）并激活 | `name` | ✅ |
| `save_part` | 保存；给出路径则另存为 | `path?` | ✅ |
| `export_step` | 把当前文件导出为 STEP | `path` | ✅ |
| `screenshot` | 当前视图 PNG，作为图片返回（临时文件会删除） | `width, height, zoom_all` | ✅ |
| `run_command` | 原始中望3D 命令字符串（需 `ZW3D_MCP_ENABLE_RUN_COMMAND=1`） | `command` | ✅ |
| `undo` / `redo` | 按整次工具调用撤销/重做；返回减少/新增的实体 | `steps` | ⚠️ |

**基础体**

| 工具 | 作用 | 主要参数 | 状态 |
|---|---|---|---|
| `create_box` | 轴对齐长方体 | `center+size` 或 `corner1+corner2`、`combine` | ✅ |
| `create_cylinder` | 沿 +Z 的圆柱，从底面圆心起 | `base_center, radius, height, combine` | ✅ |
| `create_sphere` | 球体 | `center, radius, combine` | ✅ |

**轮廓特征**（`combine`：`none` 新实体、`add`、`remove`、`intersect`）

| 工具 | 作用 | 主要参数 | 状态 |
|---|---|---|---|
| `sketch_extrude` | 在平面上建真实草图（自动标注尺寸）再拉伸；支持内孔 | `plane, segments, distance, start, symmetric, reverse, draft_angle, dimensions, holes, combine` | ⚠️ |
| `sketch_revolve` | 真实草图绕 2D 轴旋转 | `plane, segments, axis_point, axis_direction, angle, combine` | ⚠️ |
| `extrude_polygon` | 拉伸封闭平面多边形（线框，无草图） | `points, distance, start, direction, draft_angle, combine` | ✅ |
| `revolve_polygon` | 旋转封闭平面多边形 | `points, axis_point, axis_direction, angle, combine` | ✅ |
| `extrude_profile` | 拉伸直线 / 三点圆弧 / 整圆组成的轮廓 | `segments, distance, …` | ✅ |
| `revolve_profile` | 旋转直线 / 圆弧组成的轮廓 | `segments, axis_point, axis_direction, angle, …` | ✅ |
| `sweep` | 封闭轮廓沿直线/圆弧路径扫掠 | `profile, path, combine` | ⚠️ |
| `loft` | 通过 ≥ 2 个封闭截面放样 | `sections, combine` | ⚠️ |

**编辑特征**

| 工具 | 作用 | 主要参数 | 状态 |
|---|---|---|---|
| `boolean` | 实体并 / 差 / 交（工具体不相交时拒绝执行） | `operation, base_id, tool_ids, keep_tools` | ✅ |
| `fillet_edges` | 等半径圆角（不给边则全部边） | `shape_id, radius, edge_ids?` | ✅ |
| `chamfer_edges` | 对称或不对称倒角 | `shape_id, distance, distance2?, edge_ids?` | ✅ |
| `shell_shape` | 抽壳，可指定开口面；默认向内 | `shape_id, thickness, open_face_ids?, inward` | ⚠️ |
| `create_hole` | 平面上的简单孔 / 沉头孔 / 锥形沉头孔 | `face_id, points, diameter, depth` 或 `through, hole_type` | ⚠️ |
| `draft_faces` | 围绕中性面做拔模 | `face_ids, neutral_face_ids, angle, pull_direction` | ⚠️ |
| `pattern_linear` | 实体或特征的线性 / 网格阵列 | `count, spacing, direction, shape_id` 或 `feature_ids, count2…` | ⚠️ |
| `pattern_circular` | 实体或特征的圆周阵列 | `count, axis_point, axis_direction, angle?` | ⚠️ |
| `mirror` | 沿平面镜像实体或特征 | `plane, shape_id` 或 `feature_ids, combine` | ⚠️ |

**查询**

| 工具 | 作用 | 主要参数 | 状态 |
|---|---|---|---|
| `list_shapes` | 所有实体及包围盒、体积、表面积 | – | ✅ |
| `list_faces` | 面的包围盒、曲面类型、平面法向 / 圆柱半径与轴 | `shape_id` | ✅ |
| `list_edges` | 边的精确包围盒与曲线（直线端点 / 圆弧圆心+半径） | `shape_id` | ✅ |
| `list_features` | 历史特征的 id、名称、模板 | – | ⚠️ |
| `get_bounding_box` | 实体包围盒 | `shape_id` | ✅ |
| `get_mass_props` | 体积、表面积、`mass_kg`、重心 | `shape_id, density?` | ✅ |

**参数化修改**

| 工具 | 作用 | 主要参数 | 状态 |
|---|---|---|---|
| `list_variables` / `add_variable` / `set_variable` | 零件变量（距离、角度、数值或表达式）；设置后重新生成零件 | `name, value` 或 `expression, kind` | ⚠️ |
| `list_sketch_variables` / `set_sketch_variable` | 草图变量及诊断信息；修改后重新生成 | `sketch_id, name, value` | ⚠️ |
| `get_feature_data` | 单个特征的字段：id、标签、类型、值、`variable_name` | `feature_id` | ⚠️ |
| `set_feature_value` | 修改已有特征的数值参数并重新生成 | `feature_id, field_id, value` | ⚠️ |

注意：重新生成会让实体/面/边 id 全部重新编号，请只使用最近一次调用返回的 id。

**装配与工程图**（基础）

| 工具 | 作用 | 主要参数 | 状态 |
|---|---|---|---|
| `new_assembly` | 新建装配文件（`.Z3ASM`） | `name` | ⚠️ |
| `insert_component` | 把已保存的零件插入当前装配 | `file, position, anchor` | ⚠️ |
| `list_components` | 当前装配的组件 id | – | ⚠️ |
| `new_drawing` | 为已保存零件新建带标准视图的工程图 | `name, part_file` | ⚠️ |

暂不支持：装配配合、工程图标注/明细表、钣金、焊接、CAM、仿真。

## 从源码构建（Windows，VS 2022）

```bat
cd plugin
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

会自动查找 SDK（`C:\Program Files\ZWSOFT\ZW3D <年份>`，取 ≥ 2025 的最新版）；否则加 `-DZW3D_SDK_DIR="D:\ZW3D 2026"`。
`-DZW3D_MCP_INSTALL_USER=ON -DZW3D_MCP_YEAR=2025` 会在构建后把 DLL 复制到 apilibs 文件夹（**先关闭中望3D**，已加载的 DLL 无法覆盖）。
`scripts/package.ps1` 生成发布用 zip。

## 测试

插件只能在中望3D 内运行，所以仓库里没有离线测试。实机测试由人工或智能体通过 MCP 工具完成；
未完成项和实测发现记录在 [docs/api_notes.md](docs/api_notes.md)。

## 仓库结构

| 路径 | 内容 |
|---|---|
| `plugin/src/server.cpp` | HTTP 服务（Winsock） |
| `plugin/src/mcp/` | MCP 协议、带 JSON Schema 的工具表、参数校验 |
| `plugin/src/dispatcher.cpp` | 主线程调度器（所有中望3D 调用都在其主线程执行） |
| `plugin/src/handlers/` | 调用中望3D API 的处理函数 |
| `docs/` | `INSTALL.md`、`api_notes.md` |
| `CLAUDE.md` | 设计文档 |
