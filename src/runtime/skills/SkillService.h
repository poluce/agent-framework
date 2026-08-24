#pragma once

#include "skills/FileSkill.h"

class Agent;
class FileSkillLoader;

// 技能调用编排（目录查询走注入的 FileSkillLoader，不经过进程单例）。
namespace SkillService {

// 组装对话中可见的用户消息：/skill + 参数；无参数时仅 /skill。
// （曾只回显参数，导致账本里斜杠命令丢失）
QString visibleSkillMessage(const FileSkill &skill, const QString &userText);

// 解析斜杠命令，向 agent 注入技能消息并提交用户输入；找不到技能返回 false
bool submitWithSkill(FileSkillLoader *loader,
                     Agent *agent,
                     const QString &slash,
                     const QString &userText,
                     const QStringList &filePaths);

} // namespace SkillService
