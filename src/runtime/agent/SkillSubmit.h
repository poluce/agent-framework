#pragma once

#include <QString>
#include <QStringList>

class Agent;
class FileSkillLoader;

namespace SkillService {

/// 解析斜杠命令，向单元注入技能消息并提交用户输入；找不到技能返回 false。
bool submitWithSkill(FileSkillLoader *loader,
                     Agent *agent,
                     const QString &slash,
                     const QString &userText,
                     const QStringList &filePaths);

} // namespace SkillService
