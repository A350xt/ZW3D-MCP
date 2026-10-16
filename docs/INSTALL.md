# 安装 / Install (Windows, ZW3D 2025+)

整个 MCP 服务端就是一个 DLL（`zw3dmcp.dll`），运行在中望3D 进程内，通过 HTTP 提供 MCP。**不需要 Python / Node。**

## 前提
- 中望3D 2025 或更新版本（Windows x64）。已在 **2025 SP2** 上实测；更新版本理论兼容但未测试。
- 支持 MCP HTTP 传输（Streamable HTTP）的客户端：Claude Code、Cursor 等。

## 步骤
1. 从 [Releases](../../releases) 下载 `zw3d-mcp-vX.Y.Z-win64.zip` 并解压，得到 `zw3dmcp.dll`。
2. **关闭中望3D**（已加载的 DLL 无法覆盖）。
3. 把 `zw3dmcp.dll` 复制到：
   ```
   %APPDATA%\ZWSOFT\ZW3D\ZW3D 2025\custom\apilibs
   ```
   （在资源管理器地址栏直接粘贴即可；2026 等新版本把年份换掉；没有 `apilibs` 文件夹就新建。其他加载方式见 `docs/api_notes.md`。）
4. 启动中望3D，输出窗口应显示 `[ZW3D-MCP] MCP endpoint: http://127.0.0.1:47823/mcp`。
   若没有：在命令栏输入 `McpStart`，或用应用程序管理器手动加载该 DLL。
5. 在客户端里注册这个地址：
   - **Claude Code**：`claude mcp add --transport http zw3d http://127.0.0.1:47823/mcp`
   - **Cursor**：`{"mcpServers": {"zw3d": {"url": "http://127.0.0.1:47823/mcp"}}}`
   - 只支持 stdio 的客户端（如 Claude Desktop 的本地配置）：需要一个 stdio 转 HTTP 的桥，例如 `npx -y mcp-remote http://127.0.0.1:47823/mcp`（需要 Node；未验证）。
6. 验证：对 AI 说“ping 一下中望3D”。

## 卸载
关闭中望3D，删除 `apilibs` 里的 `zw3dmcp.dll`，并在客户端里移除这个 MCP 服务端。

## 环境变量（在启动中望3D之前设置）
| 变量 | 作用 |
|---|---|
| `ZW3D_MCP_PORT` | 监听端口，默认 47823 |
| `ZW3D_MCP_AUTOSTART=0` | 不自动监听，需手动 `McpStart` |
| `ZW3D_MCP_TOKEN` | 设置后，所有请求必须带 `Authorization: Bearer <token>`（Claude Code：`--header "Authorization: Bearer <token>"`） |
| `ZW3D_MCP_ENABLE_RUN_COMMAND=1` | 开启高风险的 `run_command` 工具（默认关） |

## 安全
- 只监听 `127.0.0.1`，并拒绝 `Host` / `Origin` 不是本机回环的请求（网页不能借你的浏览器操作中望3D）。
- 不设 `ZW3D_MCP_TOKEN` 时，本机任何进程都能调用；不要把端口转发到网络上。

## 常见问题
- **连不上 / connection refused**：中望3D 没启动、插件没加载，或端口被占用（换 `ZW3D_MCP_PORT`）。
- **403**：请求的 Host/Origin 不是 127.0.0.1 / localhost（例如经反向代理转发）。**401**：设置了 token 但客户端没带或带错。
- **复制 DLL 时提示文件被占用**：中望3D 还开着，先完全退出。
- **插件加载失败**：确认安装了 x64 的 Visual C++ 2015-2022 运行库（中望3D 通常已带）。
- **请求超时**：中望3D 正在执行命令或弹出对话框，按 Esc / 关闭对话框后重试。
- 自行编译：见 `README.md` 第 2 节。
