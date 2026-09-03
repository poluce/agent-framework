# 最小框架宿主

只链安装后的 `AgentFramework`，不碰产品仓的 Host / GUI / 内置配方。

样例做两件事：

1. 登记一个仓外配方（单单元，无 spawn）。
2. 注入假 Provider，`start()` + `submitUserDelivery`，等到助手正文出现。

不访问网络，不加载本产品内置配方。

```powershell
cmake --install <build> --prefix <prefix> --component AgentFramework
cmake -S . -B build -DCMAKE_PREFIX_PATH="<prefix>;<qt-prefix>"
cmake --build build
./build/agent_framework_minimal
# find_package(AgentFramework 0.3)
```
