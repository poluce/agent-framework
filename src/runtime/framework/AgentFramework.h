#pragma once

/**
 * Agent 框架公开面（配方作者 / 内核嵌入方）。
 *
 * 只 include 本头即可写自定义编排：单元表、编排口、工具源、注册表。
 * 不要从这里去够产品壳：CoreApplicationService、HostBus、HostTypes、
 * LeaderTeam / SingleUnit / PeerPair、MCP、btw。
 *
 * 链接目标：agent_framework（INTERFACE → agent_runtime）。
 * 安装后：find_package(AgentFramework REQUIRED)
 *         target_link_libraries(... AgentFramework::agent_framework)
 */

#include "agent/AbstractOrchestration.h"
#include "agent/AgentModePolicy.h"
#include "agent/Agent.h"
#include "agent/AgentSession.h"
#include "agent/OrchestrationRegistry.h"
#include "tools/AbstractSessionTool.h"
#include "tools/AbstractToolSource.h"
#include "tools/ToolTypes.h"
