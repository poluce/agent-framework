# AgentFramework：最小执行单元内核（模拟独立仓库根）。
# 单独：cmake -S framework -B build-fw
# 本产品：根 CMake add_subdirectory(framework)

cmake_minimum_required(VERSION 3.21)

set(AGENT_FRAMEWORK_VERSION "0.1.0")
set(AGENT_FRAMEWORK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(AGENT_FW_SRC "${AGENT_FRAMEWORK_ROOT}/src")

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(AGENT_FRAMEWORK_STANDALONE ON)
    if(WIN32)
        include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/AgentFrameworkToolchain.cmake")
    endif()
    project(AgentFramework VERSION "${AGENT_FRAMEWORK_VERSION}" LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTORCC ON)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Core Network)
    option(AGENT_FRAMEWORK_BUILD_TESTS "Build framework-only tests" ON)
else()
    set(AGENT_FRAMEWORK_STANDALONE OFF)
    set(AGENT_FRAMEWORK_SOURCE_DIR "${AGENT_FRAMEWORK_ROOT}" PARENT_SCOPE)
    set(AGENT_FRAMEWORK_SOURCE_DIR "${AGENT_FRAMEWORK_ROOT}")
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

function(agent_framework_lib_defaults target lib_subdir)
    target_include_directories(${target}
        PUBLIC "$<BUILD_INTERFACE:${AGENT_FW_SRC}/${lib_subdir}>"
        PRIVATE "${AGENT_FW_SRC}")
    set_target_properties(${target} PROPERTIES AUTOMOC ON)
endfunction()

function(agent_framework_lib_impl_defaults target lib_subdir)
    target_include_directories(${target}
        PRIVATE
            "${AGENT_FW_SRC}/${lib_subdir}"
            "${AGENT_FW_SRC}")
    set_target_properties(${target} PROPERTIES AUTOMOC ON)
endfunction()

# ----------------------------------------------------------
# agent_shared
# ----------------------------------------------------------
set(AGENT_SHARED_SOURCES
    src/shared/logging/LogManager.cpp
    src/shared/logging/LogManager.h
    src/shared/config/ProcessSafety.cpp
    src/shared/config/ProcessSafety.h
    src/shared/config/ModelTokenDefaults.h
    src/shared/config/PathGuard.h
    src/shared/config/SessionRuntime.fields.h
)
add_library(agent_shared STATIC ${AGENT_SHARED_SOURCES})
agent_framework_lib_defaults(agent_shared shared)
target_include_directories(agent_shared PUBLIC
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/agent-framework>")
target_link_libraries(agent_shared PUBLIC Qt6::Core)

# ----------------------------------------------------------
# agent_runtime
# ----------------------------------------------------------
file(GLOB_RECURSE AGENT_RUNTIME_AGENT_SOURCES CONFIGURE_DEPENDS
    "${AGENT_FW_SRC}/runtime/agent/*.cpp"
    "${AGENT_FW_SRC}/runtime/agent/*.h")
file(GLOB_RECURSE AGENT_RUNTIME_TOOLS_SOURCES CONFIGURE_DEPENDS
    "${AGENT_FW_SRC}/runtime/tools/*.cpp"
    "${AGENT_FW_SRC}/runtime/tools/*.h")
file(GLOB_RECURSE AGENT_RUNTIME_PROVIDERS_SOURCES CONFIGURE_DEPENDS
    "${AGENT_FW_SRC}/runtime/providers/*.cpp"
    "${AGENT_FW_SRC}/runtime/providers/*.h")
file(GLOB_RECURSE AGENT_RUNTIME_SKILLS_SOURCES CONFIGURE_DEPENDS
    "${AGENT_FW_SRC}/runtime/skills/*.cpp"
    "${AGENT_FW_SRC}/runtime/skills/*.h")
set(AGENT_RUNTIME_SOURCES
    ${AGENT_RUNTIME_AGENT_SOURCES}
    ${AGENT_RUNTIME_TOOLS_SOURCES}
    ${AGENT_RUNTIME_PROVIDERS_SOURCES}
    ${AGENT_RUNTIME_SKILLS_SOURCES}
    src/runtime/ir/CoreEvent.h
    src/runtime/ir/CoreEventChannel.h
    src/runtime/models/ConversationMessage.h
    src/runtime/config/SystemPromptBuilder.cpp
    src/runtime/config/SystemPromptBuilder.h
    src/runtime/framework/AgentFramework.h
)
add_library(agent_runtime STATIC ${AGENT_RUNTIME_SOURCES})
agent_framework_lib_impl_defaults(agent_runtime runtime)
if(EXISTS "${AGENT_FRAMEWORK_ROOT}/pch.h")
    target_precompile_headers(agent_runtime PRIVATE "${AGENT_FRAMEWORK_ROOT}/pch.h")
endif()
target_link_libraries(agent_runtime
    PUBLIC agent_shared Qt6::Core Qt6::Network)
target_sources(agent_runtime PRIVATE "${AGENT_FRAMEWORK_ROOT}/resources/system_prompts.qrc")

# 伞头 quoted include 闭包；增头须同步本表与 tests/check_framework_install.cmake
set(AGENT_FRAMEWORK_PUBLIC_HEADERS
    framework/AgentFramework.h
    agent/AbstractOrchestration.h
    agent/AbstractLoop.h
    agent/Agent.h
    agent/AgentMode.h
    agent/AgentModePolicy.h
    agent/AgentSession.h
    agent/AgentTaskManager.h
    agent/OrchestrationRegistry.h
    agent/ProviderRunLedger.h
    agent/SessionRuntime.h
    agent/compact/CompactConfig.h
    agent/compact/CompactEngine.h
    agent/compact/ModelViewStore.h
    agent/compact/SummaryJobQueue.h
    agent/compact/SummaryStore.h
    config/SystemPromptBuilder.h
    ir/CoreEvent.h
    ir/CoreEventChannel.h
    models/ConversationMessage.h
    providers/ProviderTypes/ProviderAdapterTypes.h
    providers/ProviderTypes/ProviderCommon.h
    providers/ProviderTypes/ProviderEvent.h
    providers/ProviderTypes/ProviderItem.h
    providers/ProviderTypes/ProviderRequest.h
    providers/ProviderTypes/ProviderTypes.h
    providers/core/AbstractProvider.h
    providers/core/ProviderRetryPolicy.h
    providers/service/InstanceModelCatalogStore.h
    providers/service/ModelContextMetaStore.h
    providers/service/ModelsDevClient.h
    providers/service/ProviderCredential.h
    providers/service/ProviderService.h
    skills/FileSkill.h
    skills/FileSkillLoader.h
    skills/SkillService.h
    tools/AbstractSessionTool.h
    tools/AbstractToolSource.h
    tools/BuiltinToolRuntime.h
    tools/ToolTypes.h
)

set(_fw_inc "${CMAKE_CURRENT_BINARY_DIR}/agent-framework-include")
set(_fw_staged "")
foreach(_rel IN LISTS AGENT_FRAMEWORK_PUBLIC_HEADERS)
    set(_src "${AGENT_FW_SRC}/runtime/${_rel}")
    set(_dst "${_fw_inc}/${_rel}")
    get_filename_component(_dst_dir "${_dst}" DIRECTORY)
    add_custom_command(OUTPUT "${_dst}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_dst_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_src}" "${_dst}"
        DEPENDS "${_src}"
        VERBATIM)
    list(APPEND _fw_staged "${_dst}")
endforeach()
foreach(_pair IN ITEMS
        "shared/logging/LogManager.h|logging/LogManager.h"
        "shared/config/ModelTokenDefaults.h|config/ModelTokenDefaults.h"
        "shared/config/SessionRuntime.fields.h|config/SessionRuntime.fields.h"
        "shared/config/ProcessSafety.h|config/ProcessSafety.h"
        "shared/config/PathGuard.h|config/PathGuard.h")
    string(REPLACE "|" ";" _pair_parts "${_pair}")
    list(GET _pair_parts 0 _src_rel)
    list(GET _pair_parts 1 _dst_rel)
    set(_src "${AGENT_FW_SRC}/${_src_rel}")
    set(_dst "${_fw_inc}/${_dst_rel}")
    get_filename_component(_dst_dir "${_dst}" DIRECTORY)
    add_custom_command(OUTPUT "${_dst}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_dst_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_src}" "${_dst}"
        DEPENDS "${_src}"
        VERBATIM)
    list(APPEND _fw_staged "${_dst}")
endforeach()
add_custom_target(agent_framework_stage DEPENDS ${_fw_staged})

add_library(agent_framework INTERFACE)
add_dependencies(agent_framework agent_framework_stage)
target_include_directories(agent_framework INTERFACE
    "$<BUILD_INTERFACE:${_fw_inc}>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/agent-framework>")
target_link_libraries(agent_framework INTERFACE agent_runtime)
add_library(AgentFramework::agent_framework ALIAS agent_framework)

# ----------------------------------------------------------
# 安装
# ----------------------------------------------------------
set(_agent_framework_cmake_dir "${CMAKE_INSTALL_LIBDIR}/cmake/AgentFramework")

install(TARGETS agent_shared agent_runtime agent_framework
    EXPORT AgentFrameworkTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT AgentFramework
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT AgentFramework
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT AgentFramework
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/agent-framework)

foreach(_rel IN LISTS AGENT_FRAMEWORK_PUBLIC_HEADERS)
    get_filename_component(_dir "${_rel}" DIRECTORY)
    install(FILES "${AGENT_FW_SRC}/runtime/${_rel}"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/agent-framework/${_dir}"
        COMPONENT AgentFramework)
endforeach()

install(DIRECTORY "${AGENT_FW_SRC}/shared/logging/"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/agent-framework/logging
    COMPONENT AgentFramework
    FILES_MATCHING PATTERN "*.h")

install(FILES
    "${AGENT_FW_SRC}/shared/config/ModelTokenDefaults.h"
    "${AGENT_FW_SRC}/shared/config/SessionRuntime.fields.h"
    "${AGENT_FW_SRC}/shared/config/ProcessSafety.h"
    "${AGENT_FW_SRC}/shared/config/PathGuard.h"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/agent-framework/config
    COMPONENT AgentFramework)

configure_package_config_file(
    "${AGENT_FRAMEWORK_ROOT}/cmake/AgentFrameworkConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/AgentFrameworkConfig.cmake"
    INSTALL_DESTINATION "${_agent_framework_cmake_dir}")
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/AgentFrameworkConfigVersion.cmake"
    VERSION "${AGENT_FRAMEWORK_VERSION}"
    COMPATIBILITY SameMinorVersion)

install(EXPORT AgentFrameworkTargets
    FILE AgentFrameworkTargets.cmake
    NAMESPACE AgentFramework::
    DESTINATION "${_agent_framework_cmake_dir}"
    COMPONENT AgentFramework)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/AgentFrameworkConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/AgentFrameworkConfigVersion.cmake"
    DESTINATION "${_agent_framework_cmake_dir}"
    COMPONENT AgentFramework)

if(AGENT_FRAMEWORK_STANDALONE AND AGENT_FRAMEWORK_BUILD_TESTS)
    enable_testing()
    find_package(Qt6 6.5 REQUIRED COMPONENTS Test)
    add_subdirectory(tests)
endif()
