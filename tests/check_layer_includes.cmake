# 分层 include 门禁：下层源文件不得 include 上层短路径。
# 需要 SRC_ROOT（本仓根）。

if(NOT SRC_ROOT)
    message(FATAL_ERROR "need SRC_ROOT")
endif()

function(agent_fw_forbid_includes rel_dir pattern label)
    file(GLOB_RECURSE _files
        "${SRC_ROOT}/src/runtime/${rel_dir}/*.h"
        "${SRC_ROOT}/src/runtime/${rel_dir}/*.cpp")
    foreach(_f IN LISTS _files)
        file(READ "${_f}" _txt)
        if(_txt MATCHES "${pattern}")
            message(FATAL_ERROR "${label}: ${_f}")
        endif()
    endforeach()
endfunction()

# tools / types / providers / skills 都不得碰 agent/
agent_fw_forbid_includes("tools" "#include[ \t]*[\"<]agent/" "tools must not include agent/")
agent_fw_forbid_includes("types" "#include[ \t]*[\"<]agent/" "types must not include agent/")
agent_fw_forbid_includes("providers" "#include[ \t]*[\"<]agent/" "providers must not include agent/")
agent_fw_forbid_includes("skills" "#include[ \t]*[\"<]agent/" "skills must not include agent/")

# types 不得碰 providers / skills
agent_fw_forbid_includes("types" "#include[ \t]*[\"<]providers/" "types must not include providers/")
agent_fw_forbid_includes("types" "#include[ \t]*[\"<]skills/" "types must not include skills/")

# providers 不得碰 skills
agent_fw_forbid_includes("providers" "#include[ \t]*[\"<]skills/" "providers must not include skills/")
