#pragma once

#include "skills/FileSkill.h"

#include <QString>

namespace SkillService {

/// 组装对话中可见的用户消息：/skill + 参数；无参数时仅 /skill。
QString visibleSkillMessage(const FileSkill &skill, const QString &userText);

} // namespace SkillService
