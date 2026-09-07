#include "SkillService.h"

namespace SkillService {

QString visibleSkillMessage(const FileSkill &skill, const QString &userText)
{
    const QString trimmedText = userText.trimmed();
    if (trimmedText.isEmpty())
        return skill.slash();
    return QStringLiteral("%1 %2").arg(skill.slash(), trimmedText);
}

} // namespace SkillService
