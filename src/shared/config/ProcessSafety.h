#pragma once

/// QProcess 启动前的环境硬化（如 Windows NoDefaultCurrentDirectoryInExePath）。
/// 与配置加载无关；勿再经 StartupConfig 间接包含。
void applyProcessSafety();
