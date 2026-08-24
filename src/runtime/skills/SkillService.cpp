#include "SkillService.h"

#include "agent/Agent.h"
#include "skills/FileSkillLoader.h"

namespace SkillService {

bool submitWithSkill(FileSkillLoader *loader,
                     Agent *agent,
                     const QString &slash,
                     const QString &userText,
                     const QStringList &filePaths)
{
    if (!agent || !loader) {
        return false;
    }

    const std::optional<FileSkill> skill = loader->findByDirName(slash);
    if (!skill.has_value()) {
        return false;
    }

    agent->submitUserMessageWithSkill(visibleSkillMessage(*skill, userText),
                                      filePaths, skill->displayName(), skill->body);
    return true;
}

QString visibleSkillMessage(const FileSkill &skill, const QString &userText)
{
    const QString trimmedText = userText.trimmed();
    if (trimmedText.isEmpty())
        return skill.slash();
    return QStringLiteral("%1 %2").arg(skill.slash(), trimmedText);
}

} // namespace SkillService
