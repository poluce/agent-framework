# 框架测试。
# 公开面：只链 agent_framework，安装包路径也可跑。
# 内核源：需本树 runtime 私有头，TARGET agent_runtime 且源码在 framework/src。

get_filename_component(_fw_root "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
get_filename_component(_cmake_src "${CMAKE_SOURCE_DIR}" ABSOLUTE)
set(_fw_example "${_fw_root}/examples/minimal")
set(_fw_check "${CMAKE_CURRENT_SOURCE_DIR}/check_framework_install.cmake")
set(_standalone OFF)
if(_cmake_src STREQUAL _fw_root)
    set(_standalone ON)
endif()

if(TARGET agent_framework)
    set(_fw_lib agent_framework)
elseif(TARGET AgentFramework::agent_framework)
    set(_fw_lib AgentFramework::agent_framework)
else()
    message(FATAL_ERROR "framework/tests 需要 agent_framework 或 AgentFramework::agent_framework")
endif()

set(_fw_all_targets "")

function(agent_fw_add_public_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE ${_fw_lib} Qt6::Core Qt6::Test ${ARGN})
    if(TARGET agent_framework_stage)
        add_dependencies(${name} agent_framework_stage)
    endif()
    add_test(NAME ${name} COMMAND ${name})
    set(_acc ${_fw_all_targets})
    list(APPEND _acc ${name})
    set(_fw_all_targets "${_acc}" PARENT_SCOPE)
endfunction()

agent_fw_add_public_test(agent_framework_surface_tests AgentFrameworkSurfaceTests.cpp)
agent_fw_add_public_test(agent_log_manager_injection_tests LogManagerInjectionTests.cpp)
agent_fw_add_public_test(agent_session_runtime_fields_tests SessionRuntimeFieldsTests.cpp)
agent_fw_add_public_test(agent_model_context_meta_tests ModelContextMetaStoreTests.cpp)
agent_fw_add_public_test(agent_provider_protocol_tests ProviderProtocolMigrationTests.cpp)

add_executable(agent_framework_host_turn_tests "${_fw_example}/main.cpp")
target_link_libraries(agent_framework_host_turn_tests PRIVATE
    ${_fw_lib} Qt6::Core Qt6::Network)
set_target_properties(agent_framework_host_turn_tests PROPERTIES AUTOMOC OFF)
if(TARGET agent_framework_stage)
    add_dependencies(agent_framework_host_turn_tests agent_framework_stage)
endif()
add_test(NAME agent_framework_host_turn_tests COMMAND agent_framework_host_turn_tests)
list(APPEND _fw_all_targets agent_framework_host_turn_tests)

if(TARGET agent_framework_stage)
    file(GENERATE
        OUTPUT "${CMAKE_BINARY_DIR}/framework_include_dirs.txt"
        CONTENT "$<JOIN:$<TARGET_PROPERTY:agent_framework,INTERFACE_INCLUDE_DIRECTORIES>,\n>
")
endif()

# 内核私有头测试：只在本树编 runtime 时（安装包没有 PatchUtils / 厂商适配器头）
set(_fw_have_src OFF)
if(TARGET agent_runtime AND EXISTS "${_fw_root}/src/runtime/agent/Agent.cpp")
    set(_fw_have_src ON)
endif()

if(_fw_have_src)
    function(agent_fw_add_kernel_test name source)
        add_executable(${name} ${source})
        target_include_directories(${name} PRIVATE
            "${_fw_root}/src/runtime"
            "${_fw_root}/src")
        target_link_libraries(${name} PRIVATE
            agent_runtime Qt6::Core Qt6::Network Qt6::Test)
        add_test(NAME ${name} COMMAND ${name})
        set(_acc ${_fw_all_targets})
        list(APPEND _acc ${name})
        set(_fw_all_targets "${_acc}" PARENT_SCOPE)
    endfunction()

    agent_fw_add_kernel_test(agent_patch_utils_tests PatchUtilsTests.cpp)
    agent_fw_add_kernel_test(agent_ansi_strip_tests AnsiStripTests.cpp)
    agent_fw_add_kernel_test(agent_builtin_read_glob_grep_tests BuiltinToolsReadGlobGrepTests.cpp)
    agent_fw_add_kernel_test(agent_write_concurrency_tests WriteConcurrencyTests.cpp)
    agent_fw_add_kernel_test(agent_write_tools_enhancement_tests WriteToolsEnhancementTests.cpp)
    agent_fw_add_kernel_test(agent_provider_retry_tests ProviderRetryTests.cpp)
    agent_fw_add_kernel_test(agent_summary_job_queue_tests SummaryJobQueueTests.cpp)
    agent_fw_add_kernel_test(agent_inbox_delivery_tests InboxDeliveryTests.cpp)
    agent_fw_add_kernel_test(agent_skill_tests SkillTests.cpp)
    agent_fw_add_kernel_test(agent_script_tool_source_tests ScriptToolSourceTests.cpp)
    agent_fw_add_kernel_test(agent_config_tool_tests ConfigToolTests.cpp)
    agent_fw_add_kernel_test(agent_tool_group_visibility_tests ToolGroupVisibilityTests.cpp)
endif()

set(_fw_test_names ${_fw_all_targets})

if(NOT AGENT_QT_USE_INSTALLED_FRAMEWORK)
    set(_install_args
        "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
        "-DPREFIX=${CMAKE_BINARY_DIR}/_agent_framework_prefix"
        "-DEXAMPLE_SRC=${_fw_example}"
        "-DCMAKE_GENERATOR=${CMAKE_GENERATOR}"
        "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
        "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
        "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
        "-DQT6_DIR=${Qt6_DIR}")
    add_test(
        NAME agent_framework_install_layout
        COMMAND ${CMAKE_COMMAND} ${_install_args} -P "${_fw_check}")
    set_tests_properties(agent_framework_install_layout PROPERTIES TIMEOUT 360)
    list(APPEND _fw_test_names agent_framework_install_layout)
endif()

set(_surface_check "${CMAKE_CURRENT_SOURCE_DIR}/check_client_include_surface.cmake")
if(EXISTS "${_surface_check}" AND TARGET agent_framework_stage)
    add_test(
        NAME agent_framework_forbidden_deepseek
        COMMAND ${CMAKE_COMMAND}
            "-DCOMPILER=${CMAKE_CXX_COMPILER}"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/FrameworkSurfaceForbiddenDeepSeek.cpp"
            "-DINCLUDES_FILE=${CMAKE_BINARY_DIR}/framework_include_dirs.txt"
            "-DFORBIDDEN=providers/deepseek/DeepSeekProvider.h"
            -P "${_surface_check}")
    set_tests_properties(agent_framework_forbidden_deepseek PROPERTIES TIMEOUT 60)
    list(APPEND _fw_test_names agent_framework_forbidden_deepseek)
endif()

list(REMOVE_DUPLICATES _fw_test_names)
list(REMOVE_ITEM _fw_test_names "")
set_tests_properties(${_fw_test_names} PROPERTIES LABELS "framework")

if(NOT _standalone AND _fw_all_targets)
    set_target_properties(${_fw_all_targets}
        PROPERTIES EXCLUDE_FROM_ALL TRUE EXCLUDE_FROM_DEFAULT_BUILD TRUE)
endif()

if(_standalone AND _fw_all_targets)
    add_custom_target(agent_framework_tests DEPENDS ${_fw_all_targets})
endif()

set(AGENT_FRAMEWORK_TEST_TARGETS "${_fw_all_targets}" PARENT_SCOPE)
