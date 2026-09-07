#include "SkillSubmit.h"

#include "agent/Agent.h"
#include "skills/FileSkillLoader.h"
#include "skills/SkillService.h"

#include <optional>

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

} // namespace SkillService
