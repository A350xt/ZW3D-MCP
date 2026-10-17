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

单位为毫米和度；点用 `[x, y, z]`。服务端内置了使用指南（工作流程、如何选工具、常见坑）：
客户端连接时会自动收到（MCP `instructions`），也可以通过提示词 `zw3d_guide` 获取。

**会话与文件**

| 工具 | 作用 |
|---|---|
| `ping` | 检查连接；返回插件版本、当前文件与零件 |
| `new_part` | 新建零件文件（`.Z3PRT`）并激活 |
| `save_part` | 保存；给出路径则另存为 |
| `export_step` | 把当前文件导出为 STEP |
| `screenshot` | 截取当前视图并作为图片返回 |
| `run_command` | 发送原始中望3D 命令（默认关闭，需环境变量开启） |
| `undo` / `redo` | 按整次工具调用撤销 / 重做 |

**基础体**

| 工具 | 作用 |
|---|---|
| `create_box` | 创建长方体 |
| `create_cylinder` | 创建圆柱 |
| `create_sphere` | 创建球体 |

**轮廓特征**

| 工具 | 作用 |
|---|---|
| `sketch_extrude` | 在平面上建真实草图（可带内孔）并拉伸 |
| `sketch_revolve` | 在平面上建真实草图并绕轴旋转 |
| `extrude_polygon` | 拉伸封闭的平面多边形 |
| `revolve_polygon` | 绕轴旋转封闭的平面多边形 |
| `extrude_profile` | 拉伸由直线、圆弧、整圆组成的轮廓 |
| `revolve_profile` | 旋转由直线、圆弧组成的轮廓 |
| `sweep` | 让封闭轮廓沿路径扫掠 |
| `loft` | 通过两个及以上封闭截面放样 |

**编辑特征**

| 工具 | 作用 |
|---|---|
| `boolean` | 实体并、差、交 |
| `fillet_edges` | 等半径圆角 |
| `chamfer_edges` | 倒角 |
| `shell_shape` | 抽壳，可留出开口面 |
| `create_hole` | 在平面上打简单孔、沉头孔、锥形沉头孔 |
| `draft_faces` | 对面做拔模 |
| `pattern_linear` | 实体或特征的线性 / 网格阵列 |
| `pattern_circular` | 实体或特征的圆周阵列 |
| `mirror` | 沿平面镜像实体或特征 |

**查询**

| 工具 | 作用 |
|---|---|
| `list_shapes` | 列出所有实体及包围盒、体积、表面积 |
| `list_faces` | 列出实体的面及其几何信息 |
| `list_edges` | 列出实体的边及其几何信息 |
| `list_features` | 列出零件的历史特征 |
| `get_bounding_box` | 获取实体包围盒 |
| `get_mass_props` | 获取体积、表面积、质量、重心 |

**参数化修改**

| 工具 | 作用 |
|---|---|
| `list_variables` | 列出零件变量 |
| `add_variable` | 添加零件变量（数值或表达式） |
| `set_variable` | 修改零件变量并重新生成 |
| `list_sketch_variables` | 列出草图的变量 |
| `set_sketch_variable` | 修改草图变量并重新生成 |
| `get_feature_data` | 读取特征的可编辑字段 |
| `set_feature_value` | 修改已有特征的数值参数并重新生成 |

**装配与工程图**

| 工具 | 作用 |
|---|---|
| `new_assembly` | 新建装配文件 |
| `insert_component` | 把已保存的零件插入当前装配 |
| `list_components` | 列出当前装配的组件 |
| `new_drawing` | 为已保存的零件新建带标准视图的工程图 |

## 从源码构建（Windows，VS 2022）

```bat
cd plugin
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

会自动查找 SDK（`C:\Program Files\ZWSOFT\ZW3D <年份>`，取 ≥ 2025 的最新版）；否则加 `-DZW3D_SDK_DIR="D:\ZW3D 2026"`。
`-DZW3D_MCP_INSTALL_USER=ON -DZW3D_MCP_YEAR=2025` 会在构建后把 DLL 复制到 apilibs 文件夹（**先关闭中望3D**，已加载的 DLL 无法覆盖）。
`scripts/package.ps1` 生成发布用 zip。
