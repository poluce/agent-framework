---
name: release-fix-cycle
description: >
  处理 GitHub issue → 修复 → 测试 → 发版 → 关闭 issue 的完整发布闭环。
  当用户要求「看看 issue / 修复问题 / 跑测试 / 发版 / 打 tag / 写 CHANGELOG /
  创建 GitHub Release / 关闭 issue」时使用。
  强制：先测后发、破坏性变更必须写进 Release Notes、只关闭真正已修复的 issue。
---

# Issue 修复与发布闭环（release-fix-cycle）

## 何时使用

- 用户说「仓库被提了 issue，你看看」
- 要求修复 bug / 实现新功能并发布新版本
- 要求打 tag、写 CHANGELOG、创建 GitHub Release、关闭 issue
- 要求把「检查 issue → 修复 → 测试 → 发布 → 关闭 issue」整套走完

**不要用本 skill：** 只改代码不发布、或只查看 issue 不处理时，可以只执行其中部分步骤，但流程顺序和「先测后发」原则不变。

## 前置检查

1. 确认当前仓库：`git remote -v`、`git branch --show-current`
2. 确认工具可用：
   - `gh`（GitHub CLI）—— 查/关 issue、发 Release
   - `cmake` / `ctest` —— 构建测试
   - Windows 环境可参考 `E:\CodeSoftware\CMake\bin\cmake.exe`、`E:\Qt6\6.11.0\mingw_64`
3. 确认工作区干净或改动可控：`git status --short`

## 标准流程

### 1. 检查 issue

```bash
gh issue list --repo <owner>/<repo> --state open --limit 20
gh issue view <number> --repo <owner>/<repo> --json number,title,body,labels,comments
```

- 逐个判断 issue 属于**内核仓**还是**产品仓**
- 内核仓能修的 → 候选；产品仓才能修的 → 记录为「后续跟进」，不假装修复

### 2. 与用户对齐（必须，不可跳过）

**拿到 issue 后不要直接动手改。** 先向用户汇报并确认：

- 这个 issue 是不是**真问题**（有的 issue 是误解、误报、或已经不存在）
- 这个方向是不是**本项目/内核的方向**（有的需求违背内核哲学，应放到上层配方/产品仓）
- 修复范围：本次修哪些、哪些保留 OPEN
- 方案选择：如果有多种修法，先给选项让用户拍板

输出格式建议：

```text
共 N 个 issue：
- #1 ... → 建议修（理由）
- #2 ... → 建议不修（理由：不是问题 / 方向不符 / 产品仓跟进）
- #3 ... → 需要你确认方案（选项 A / B）
请确认后我再动手。
```

**用户确认前，不进入修复步骤。**

### 3. 修复

- 按 issue 描述定位代码，先读相关头文件/实现，再动手
- 保持内核「最小执行单元、不预置模式」的哲学：能放在配方/上层的不塞进内核
- 涉及公开 API 变更时，同步更新注释和文档
- 涉及 `SessionRuntime` 字段时，必须同步 `src/shared/config/SessionRuntime.fields.h` 和 `tests/SessionRuntimeFieldsTests.cpp`

### 4. 测试

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

- 新增/修改功能必须补测试
- 全部测试通过后才能进入发布步骤
- 如果环境没有工具链，明确告知用户，不能跳过测试直接发版

### 5. 更新版本与 CHANGELOG

- 按 SemVer 决定版本号：
  - 破坏性变更 / 大功能 → minor（0.2.0 → 0.3.0）
  - 纯修复 → patch（0.3.0 → 0.3.1）
- 更新 `CHANGELOG.md`，按使用者视角分类：

```markdown
## [x.y.z] - YYYY-MM-DD

### 🔴 Breaking Changes（升级前必看）
### 🟢 新增功能
### 🟡 功能修改
### 🔵 修复
```

- 同步版本号到 `CMakeLists.txt`、`README.md`、`AGENTS.md`、`examples/*/CMakeLists.txt`、`tests/check_framework_install.cmake` 等硬编码位置
- 用 `grep -rn "旧版本号"` 检查是否漏改

### 6. 提交

```bash
git add -A
git commit -m "feat: 修复 #N ... 并发布 x.y.z"
```

- 提交信息里带上 issue 编号
- 提交前确认没有临时文件（如 `.release-notes-*.md`）

### 7. 打 tag 并推送

```bash
git tag -a v<x.y.z> -m "AgentFramework <x.y.z>"
git push origin main --tags
```

### 8. 创建 GitHub Release

```bash
gh release create v<x.y.z> \
  --repo <owner>/<repo> \
  --title "AgentFramework <x.y.z>" \
  --notes-file <notes-file>
```

- Release Notes 直接使用 CHANGELOG 对应版本的内容
- 必须把 **Breaking Changes 放最前面**
- 关联 issue 编号（`#1` `#8` 等），让使用者能追溯
- 临时 notes 文件用后删除

### 9. 关闭已修复的 issue

```bash
gh issue close <number> --repo <owner>/<repo> \
  --comment "已在 v<x.y.z> 修复，见 <release-url>"
```

- **只关闭真正已修复的 issue**
- 产品仓才能修的 issue 保留 OPEN，并注明「产品仓跟进」
- 关闭后复查：`gh issue list --repo <owner>/<repo> --state all`

## 自检清单

- [ ] 所有 issue 已分类（内核仓 / 产品仓）
- [ ] **已与用户对齐**：确认哪些是真问题、哪些方向符合项目、修哪些、方案已拍板
- [ ] 代码改动有对应测试
- [ ] `ctest` 全部通过
- [ ] 版本号所有硬编码位置已同步
- [ ] CHANGELOG 包含 Breaking Changes / 新增 / 修改 / 修复
- [ ] tag 已推送
- [ ] GitHub Release 已创建，Notes 关联 issue
- [ ] 已修复的 issue 已关闭，未修复的保留 OPEN
- [ ] 工作区干净，无临时文件残留

## 不要做

- **不要拿到 issue 就直接改**——先和用户对齐，有的 issue 不是问题，有的方向不是本项目方向
- 不要没跑测试就发版
- 不要关闭实际上没修复的 issue
- 不要把产品仓的 issue 关在内核仓里（除非内核仓确实修了）
- 不要只打 tag 不写 Release Notes——使用者需要知道改了什么
- 不要把内部实现细节写进 Release Notes（使用者关心的是行为变化）
- 不要忘记 Breaking Changes——这是使用者升级前最需要的信息
