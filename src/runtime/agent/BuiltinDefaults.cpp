#include "tools/BuiltinToolRegistry.h"

#include "tools/builtin/AskQuestionTool.h"
#include "tools/builtin/EditTool.h"
#include "tools/builtin/GlobTool.h"
#include "tools/builtin/GrepTool.h"
#include "tools/builtin/MultiEditTool.h"
#include "tools/builtin/NotebookEditTool.h"
#include "tools/builtin/ReadFileTool.h"
#include "tools/builtin/RunCommandTool.h"
#include "tools/builtin/SkillListTool.h"
#include "tools/builtin/WriteFileTool.h"

QList<std::shared_ptr<AbstractBuiltinTool>> BuiltinToolRegistry::defaultTools()
{
    QList<std::shared_ptr<AbstractBuiltinTool>> tools;
    tools.append(std::make_shared<GlobTool>());
    tools.append(std::make_shared<ReadFileTool>());
    tools.append(std::make_shared<GrepTool>());
    tools.append(std::make_shared<WriteFileTool>());
    tools.append(std::make_shared<EditTool>());
    tools.append(std::make_shared<NotebookEditTool>());
    tools.append(std::make_shared<RunCommandTool>());
    tools.append(std::make_shared<SkillListTool>());
    tools.append(std::make_shared<AskQuestionTool>());
    tools.append(std::make_shared<MultiEditTool>());
    return tools;
}
