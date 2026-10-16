# 安装 / Install (Windows, ZW3D 2025+)

## 前提 Requirements
- 中望3D 2025 或更新版本（Windows x64）。已在 **2025 SP2** 上实测；更新版本理论兼容但未测试。
- Python 3.10+（用于 MCP 服务端），能在命令行运行 `python`。
- 支持 MCP 的 AI 客户端：Claude Code / Claude Desktop / Cursor 等。

## 手动安装 Manual install
1. 从 [Releases](../../releases) 下载 `zw3d-mcp-vX.Y.Z-win64.zip` 并解压，得到 `zw3dmcp.dll` 和 `zw3d_mcp-*.whl`。
2. **关闭中望3D**（已加载的 DLL 无法覆盖）。
3. 把 `zw3dmcp.dll` 复制到：
   ```
   %APPDATA%\ZWSOFT\ZW3D\ZW3D 2025\custom\apilibs
   ```
   （在资源管理器地址栏直接粘贴上面的路径即可；2026 等新版本把年份换掉；没有 `apilibs` 文件夹就新建。）
   其他加载方式（应用程序管理器手动加载、装进安装目录的 `apilibs`）见 `docs/api_notes.md`。
4. 安装 Python 服务端：
   ```
   pip install zw3d_mcp-0.1.0-py3-none-any.whl
   ```
5. 启动中望3D，输出窗口应显示 `[ZW3D-MCP] listening on 127.0.0.1:47823`。
   若没有：在命令栏输入 `McpStart`，或用应用程序管理器手动加载该 DLL。
6. 注册 MCP 服务端：
   - Claude Code：`claude mcp add zw3d -- python -m zw3d_mcp`
   - Claude Desktop / Cursor：command=`python`，args=`["-m","zw3d_mcp"]`
7. 验证：对 AI 说“ping 一下中望3D”。

## 卸载 Uninstall
关闭中望3D，删除 `apilibs` 里的 `zw3dmcp.dll`，再运行 `pip uninstall zw3d-mcp`。

## 环境变量（在启动中望3D之前设置）
| 变量 | 作用 |
|---|---|
| `ZW3D_MCP_PORT` | 监听端口，默认 47823（MCP 服务端读取同名变量） |
| `ZW3D_MCP_AUTOSTART=0` | 不自动监听，需手动 `McpStart` |
| `ZW3D_MCP_ENABLE_RUN_COMMAND=1` | 开启高风险的 `run_command` 工具（默认关） |

## 安全 Security
插件只监听 `127.0.0.1`，**没有认证**；本机任何进程都能操作中望3D。不要把端口转发到网络上。

## 常见问题 Troubleshooting
- **ping 报 NOT_CONNECTED**：中望3D 没启动 / 插件没加载 / 端口被占用（换 `ZW3D_MCP_PORT`）。
- **复制 DLL 时提示文件被占用**：中望3D 还开着，先完全退出。
- **插件加载失败**：确认安装了 x64 的 Visual C++ 2015-2022 运行库（中望3D 通常已带）。
- **请求超时**：中望3D 正在执行命令或弹出对话框，按 Esc / 关闭对话框后重试。
- 自行编译：见 `README.md` 第 1 节。
