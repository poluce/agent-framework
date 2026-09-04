# 测试覆盖率方案

给 AgentFramework 内核补覆盖率：`AGENT_FRAMEWORK_COVERAGE=ON` 构建 + gcovr 报告。

## 用法

```powershell
# 一键流程（Windows 原生 PowerShell）
pwsh scripts/coverage.ps1

# 或手动：
cmake -S . -B build-coverage -G Ninja -DAGENT_FRAMEWORK_COVERAGE=ON
cmake --build build-coverage
ctest --test-dir build-coverage -E agent_framework_install_layout --output-on-failure
gcovr -r . --object-directory=build-coverage --gcov-executable=gcov \
    --filter "F:/B_My_Document/GitHub/agent-framework/src/" \
    --exclude-directories '.*/_deps/.*' \
    --html --html-details coverage-report/coverage.html \
    --xml coverage-report/coverage.xml --print-summary
```

- 开关：`AGENT_FRAMEWORK_COVERAGE=ON`（仅 GCC/Clang；其他编译器配置期报错）
- 报告：文本汇总 + `coverage.html`（逐文件）+ `coverage.xml`（CI 上传用）
- 覆盖率构建用**独立 build 目录**（`build-coverage`），不污染普通构建

## 我们踩过的坑

### 坑 1：`--html-details` 在 MinGW + WSL 下路径映射失败

- 现象：文本汇总能出，但 `--html-details` 报大量 `source file(s) not found`（标准库/Qt 头文件路径被拼错）。
- 根因：WSL 里跑 gcovr + Windows MinGW gcov，路径体系不一致（`/mnt/...` vs `F:/...`）。
- 解决：
  - 在 **Windows 原生 PowerShell** 里用 Windows Python 装 gcovr 再跑；
  - gcovr 的 `--filter` 必须用**正斜杠**；
  - 实在不行先用普通 `--html`（单页汇总），`--html-details` 作为增强。

### 坑 2：排除 `tests/` 对象目录会导致产品源码覆盖率虚低

- 现象：产品源码被直接编进测试目标时（如 `tests/CMakeFiles/xxx.dir/__/src/...`），如果 `--exclude-directories '.*/tests/.*'`，这些源码的 `.gcda` 被一起排除，覆盖率显示 0%。
- 解决：**不要排除 tests 对象目录**；用 `--filter` 只保留要统计的源码路径（如 `src/`），测试自身代码自然不会被计入。

### 坑 3：覆盖率构建与「安装布局测试」冲突

- 现象：静态库带 `--coverage` 后，安装布局测试里单独链接示例程序时缺 `__gcov_*` 符号，链接失败。
- 解决：覆盖率跑测时排除安装布局类测试：
  ```bash
  ctest --test-dir build-coverage -E agent_framework_install_layout
  ```

### 坑 4：第三方依赖拖慢扫描

- `_deps`、`third_party` 等大量无关源码会被 gcovr 扫描，耗时很长。
- 解决：用 `--exclude-directories` 排除 `_deps`、`third_party/mcp-qt`、`third_party/qml-inspector`、`third_party/md4c` 等。

### 坑 5：覆盖率构建很慢

- 所有源码加 `--coverage` 后首次全量编译明显变慢。
- 解决：独立 build 目录 + 增量构建，不要污染普通构建。

## 验收

- [x] `AGENT_FRAMEWORK_COVERAGE=ON` 可配置、可构建、可跑测
- [x] 能生成文本汇总 + HTML/XML 报告
- [x] 覆盖率数字不因「测试目标内编译的产品源码」而虚低（`--filter src/`）
- [x] 文档记录上述坑与规避方法
