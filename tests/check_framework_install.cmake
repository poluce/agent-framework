# Install AgentFramework component into PREFIX, assert public layout,
# then configure+build+run framework/examples/minimal against the prefix.
# 产品二次配置：USE_INSTALLED=ON 且 BUILD_TESTS=ON（只编框架测试）。
# Expects: BUILD_DIR, PREFIX, CMAKE_COMMAND (set by cmake -P).
# Optional for the example: EXAMPLE_SRC, CMAKE_GENERATOR, CMAKE_CXX_COMPILER,
# CMAKE_MAKE_PROGRAM, QT6_DIR, CMAKE_BUILD_TYPE.

if(NOT BUILD_DIR OR NOT PREFIX)
    message(FATAL_ERROR "need BUILD_DIR and PREFIX")
endif()

# cmake --install 不删旧文件；上次装进 prefix 的产品头会误报泄漏
if(EXISTS "${PREFIX}")
    file(REMOVE_RECURSE "${PREFIX}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}"
            --prefix "${PREFIX}"
            --component AgentFramework
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "cmake --install AgentFramework failed (${_rc}):\n${_out}\n${_err}")
endif()

set(_inc "${PREFIX}/include/agent-framework")
set(_cmake "${PREFIX}/lib/cmake/AgentFramework")

foreach(_rel IN ITEMS
    framework/AgentFramework.h
    agent/Agent.h
    agent/AgentSession.h
    agent/AbstractOrchestration.h
    agent/AgentModePolicy.h
    agent/OrchestrationRegistry.h
    agent/AbstractLoop.h
    agent/SessionRuntime.h
    tools/AbstractToolSource.h
    tools/ToolTypes.h
    logging/LogManager.h
    config/SessionRuntime.fields.h
    config/ModelTokenDefaults.h
    config/ProcessSafety.h
    ir/CoreEvent.h
    providers/core/AbstractProvider.h
    providers/service/ProviderCredential.h
    providers/service/ProviderService.h
    skills/FileSkillLoader.h)
    if(NOT EXISTS "${_inc}/${_rel}")
        message(FATAL_ERROR "missing installed header: ${_inc}/${_rel}")
    endif()
endforeach()

if(NOT EXISTS "${_cmake}/AgentFrameworkConfig.cmake")
    message(FATAL_ERROR "missing ${_cmake}/AgentFrameworkConfig.cmake")
endif()
if(NOT EXISTS "${_cmake}/AgentFrameworkConfigVersion.cmake")
    message(FATAL_ERROR "missing ${_cmake}/AgentFrameworkConfigVersion.cmake")
endif()
if(NOT EXISTS "${_cmake}/AgentFrameworkTargets.cmake")
    message(FATAL_ERROR "missing ${_cmake}/AgentFrameworkTargets.cmake")
endif()
file(READ "${_cmake}/AgentFrameworkConfig.cmake" _cfg)
if(NOT _cfg MATCHES "set\\(AgentFramework_VERSION \"0\\.3\\.0\"\\)")
    message(FATAL_ERROR "AgentFrameworkConfig.cmake missing version 0.3.0:\n${_cfg}")
endif()

foreach(_rel IN ITEMS
    application/CoreApplicationService.h
    orchestration/LeaderTeamOrchestration.h
    config/AppPaths.h
    config/AppConfig.h
    config/SessionRuntimeKeys.h
    host/HostTypes/SessionRuntimeKeys.h
    agent/mode/DebugSessionState.h
    mode/AgentModePolicies.h
    host/HostBus.h
    host/HostTypes/HostCommand.h
    providers/deepseek/DeepSeekProvider.h
    providers/core/HttpSseChannel.h
    tools/BuiltinTools/ReadFileTool.h
    tools/SessionTools/ConfigTool.h
    agent/compact/CompactToolPair.h)
    if(EXISTS "${_inc}/${_rel}")
        message(FATAL_ERROR "product/internal header leaked into framework install: ${_rel}")
    endif()
endforeach()

if(NOT EXAMPLE_SRC)
    return()
endif()
if(NOT EXISTS "${EXAMPLE_SRC}/CMakeLists.txt")
    message(FATAL_ERROR "missing example: ${EXAMPLE_SRC}")
endif()

set(_ex_build "${PREFIX}/_example_build")
set(_prefix_path "${PREFIX}")
if(QT6_DIR)
    get_filename_component(_qt_root "${QT6_DIR}/../../.." ABSOLUTE)
    list(APPEND _prefix_path "${_qt_root}")
endif()
string(REPLACE ";" "\\;" _prefix_path_arg "${_prefix_path}")

set(_cfg_args
    "-S" "${EXAMPLE_SRC}"
    "-B" "${_ex_build}"
    "-DCMAKE_PREFIX_PATH=${_prefix_path_arg}")
if(CMAKE_GENERATOR)
    list(APPEND _cfg_args "-G" "${CMAKE_GENERATOR}")
endif()
if(CMAKE_CXX_COMPILER)
    list(APPEND _cfg_args "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}")
endif()
if(CMAKE_MAKE_PROGRAM)
    list(APPEND _cfg_args "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}")
endif()
if(CMAKE_BUILD_TYPE)
    list(APPEND _cfg_args "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")
else()
    list(APPEND _cfg_args "-DCMAKE_BUILD_TYPE=Debug")
endif()
if(QT6_DIR)
    list(APPEND _cfg_args "-DQt6_DIR=${QT6_DIR}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" ${_cfg_args}
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "example configure failed (${_rc}):\n${_out}\n${_err}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${_ex_build}"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "example build failed (${_rc}):\n${_out}\n${_err}")
endif()

if(WIN32)
    set(_ex_name "agent_framework_minimal.exe")
else()
    set(_ex_name "agent_framework_minimal")
endif()
set(_ex_bin "${_ex_build}/${_ex_name}")
if(NOT EXISTS "${_ex_bin}" AND CMAKE_BUILD_TYPE)
    set(_ex_bin "${_ex_build}/${CMAKE_BUILD_TYPE}/${_ex_name}")
endif()
if(NOT EXISTS "${_ex_bin}")
    message(FATAL_ERROR "example binary missing: ${_ex_bin}")
endif()

if(QT6_DIR)
    get_filename_component(_qt_bin "${QT6_DIR}/../../../bin" ABSOLUTE)
    if(WIN32)
        set(ENV{PATH} "${_qt_bin};$ENV{PATH}")
    else()
        set(ENV{PATH} "${_qt_bin}:$ENV{PATH}")
    endif()
endif()

execute_process(
    COMMAND "${_ex_bin}"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "example run failed (${_rc}):\n${_out}\n${_err}")
endif()

if(NOT PRODUCT_SRC)
    return()
endif()
if(NOT EXISTS "${PRODUCT_SRC}/CMakeLists.txt")
    message(FATAL_ERROR "missing product source: ${PRODUCT_SRC}")
endif()

set(_prod_build "${PREFIX}/_product_build")
set(_prod_args
    "-S" "${PRODUCT_SRC}"
    "-B" "${_prod_build}"
    "-DCMAKE_PREFIX_PATH=${_prefix_path_arg}"
    "-DAgentFramework_DIR=${_cmake}"
    "-DAGENT_QT_USE_INSTALLED_FRAMEWORK=ON"
    "-DAGENT_QT_BUILD_TESTS=ON")
if(CMAKE_GENERATOR)
    list(APPEND _prod_args "-G" "${CMAKE_GENERATOR}")
endif()
if(CMAKE_CXX_COMPILER)
    list(APPEND _prod_args "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}")
endif()
if(CMAKE_C_COMPILER)
    list(APPEND _prod_args "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}")
endif()
if(CMAKE_MAKE_PROGRAM)
    list(APPEND _prod_args "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}")
endif()
if(CMAKE_BUILD_TYPE)
    list(APPEND _prod_args "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")
else()
    list(APPEND _prod_args "-DCMAKE_BUILD_TYPE=Debug")
endif()
if(QT6_DIR)
    list(APPEND _prod_args "-DQt6_DIR=${QT6_DIR}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" ${_prod_args}
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "product-against-framework configure failed (${_rc}):\n${_out}\n${_err}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${_prod_build}"
            --target agent_app agent_framework_host_turn_tests
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "product-against-framework build agent_app/host_turn failed (${_rc}):\n${_out}\n${_err}")
endif()

if(WIN32)
    set(_host_name "agent_framework_host_turn_tests.exe")
else()
    set(_host_name "agent_framework_host_turn_tests")
endif()
set(_host_bin "")
foreach(_cand IN ITEMS
        "${_prod_build}/${_host_name}"
        "${_prod_build}/framework/tests/${_host_name}"
        "${_prod_build}/framework/tests/${CMAKE_BUILD_TYPE}/${_host_name}"
        "${_prod_build}/tests/framework_tests/${_host_name}")
    if(_cand AND EXISTS "${_cand}")
        set(_host_bin "${_cand}")
        break()
    endif()
endforeach()
if(NOT _host_bin)
    file(GLOB_RECURSE _hits LIST_DIRECTORIES false "${_prod_build}/*/${_host_name}")
    if(_hits)
        list(GET _hits 0 _host_bin)
    endif()
endif()
if(NOT _host_bin)
    message(FATAL_ERROR "product-against-framework host-turn missing under ${_prod_build}\nbuild:\n${_out}\n${_err}")
endif()
execute_process(
    COMMAND "${_host_bin}"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "product-against-framework host-turn run failed (${_rc}):\n${_out}\n${_err}")
endif()
