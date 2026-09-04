# AgentFramework 覆盖率一键流程（Windows 原生 PowerShell）
#
# 用法：
#   pwsh scripts/coverage.ps1 [-BuildDir build-coverage] [-OutDir coverage-report] [-Gcov gcov]
#
# 流程：独立 build 目录 → 配置(AGENT_FRAMEWORK_COVERAGE=ON) → 构建 → 跑测
#       （排除 agent_framework_install_layout，见 docs/测试/coverage.md 坑 3）→ gcovr 报告。
#
# 前置：
#   - Windows Python 装 gcovr：pip install gcovr（坑 1：不要在 WSL 里跑 Windows MinGW 的 gcov）
#   - MinGW 的 gcov 在 PATH 中，或用 -Gcov 指定（如 E:/Qt6/Tools/mingw1310_64/bin/gcov.exe）

param(
    [string]$BuildDir = "build-coverage",
    [string]$OutDir = "coverage-report",
    [string]$Gcov = "gcov"
)
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot

# 1. 配置覆盖率构建（独立目录，不污染普通构建；坑 5）
cmake -S $Root -B $BuildDir -G Ninja -DAGENT_FRAMEWORK_COVERAGE=ON
if ($LASTEXITCODE -ne 0) { throw "覆盖率构建配置失败" }

# 2. 构建
cmake --build $BuildDir
if ($LASTEXITCODE -ne 0) { throw "覆盖率构建失败" }

# 3. 跑测（坑 3：覆盖率构建下安装布局测试链接缺 __gcov_* 符号，排除）
ctest --test-dir $BuildDir -E agent_framework_install_layout --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "测试失败" }

# 4. gcovr 报告（坑 1：Windows 原生 PowerShell + Windows Python 的 gcovr）
if (-not (Get-Command gcovr -ErrorAction SilentlyContinue)) {
    Write-Warning "gcovr 未安装：请用 Windows Python 执行 pip install gcovr 后重跑"
    exit 0
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
# 坑 1：--filter 必须用正斜杠；坑 2：不排除 tests 对象目录，用 filter 只留 src/
$filter = ($Root -replace '\\', '/') + "/src/"
gcovr -r $Root `
    --object-directory="$BuildDir" `
    --gcov-executable="$Gcov" `
    --filter "$filter" `
    --exclude-directories '.*/_deps/.*' `
    --html --html-details "$OutDir/coverage.html" `
    --xml "$OutDir/coverage.xml" `
    --print-summary
if ($LASTEXITCODE -ne 0) { throw "gcovr 报告生成失败" }
Write-Host "报告已生成：$OutDir/coverage.html"
