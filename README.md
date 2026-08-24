# AgentFramework

本目录是 **Agent 执行单元内核** 的仓库根模拟：只依赖 Qt 6 Core + Network。

不是 GUI/TUI，也不是 Host 协议。配方、MCP、AppPaths、本产品组合根仍在上层仓库。

```powershell
# 本仓脚本（不碰 ta / QML / agent_app）
pwsh scripts/build.ps1 -Configuration Debug -Target framework
pwsh scripts/test.ps1 -Configuration Debug -Module Framework

# 或在本目录手敲
cmake -S . -B ../build/framework-Debug -G Ninja
cmake --build ../build/framework-Debug
ctest --test-dir ../build/framework-Debug --output-on-failure

# 安装
cmake --install ../build/framework-Debug --prefix <prefix> --component AgentFramework
```

本产品仓库通过 `add_subdirectory(framework)` 消费这份源；也可 `-DAGENT_QT_USE_INSTALLED_FRAMEWORK=ON` 对安装包 `find_package`（开测试时只跑 `tests/`，链导入目标）。

公开头：`src/runtime/framework/AgentFramework.h`。仓外最小宿主：`examples/minimal`。

Provider 协议演进：`.agents/skills/update-provider-protocol/SKILL.md`；正文 `docs/协议/provider-protocol.md`。
