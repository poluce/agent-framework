# Windows 独立配置 framework/ 时探测 Qt 6.11 MinGW（与产品根 CMake 同源候选）。
# add_subdirectory 进本产品时不要 include 本文件。

function(agent_fw_set_first_existing out_var)
    foreach(candidate IN ITEMS ${ARGN})
        if(candidate AND EXISTS "${candidate}")
            set(${out_var} "${candidate}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out_var} "" PARENT_SCOPE)
endfunction()

function(agent_fw_set_first_root_with out_var required_relative_path)
    foreach(candidate IN ITEMS ${ARGN})
        if(candidate AND EXISTS "${candidate}/${required_relative_path}")
            set(${out_var} "${candidate}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out_var} "" PARENT_SCOPE)
endfunction()

set(_qt_root_candidates
    "$ENV{AGENT_QT_QT_ROOT}"
    "E:/Qt6/6.11.1/mingw_64"
    "E:/Qt6/6.11.0/mingw_64"
    "D:/Qt5/Qt6.11/6.11.0/mingw_64")
set(_mingw_bin_candidates
    "$ENV{AGENT_QT_MINGW_BIN}"
    "E:/Qt6/Tools/mingw1310_64/bin"
    "D:/Qt5/Qt6.11/Tools/mingw1310_64/bin")
set(_ninja_candidates
    "$ENV{AGENT_QT_NINJA}"
    "E:/Qt6/Tools/Ninja/ninja.exe"
    "D:/Qt5/Qt6.11/Tools/Ninja/ninja.exe")

agent_fw_set_first_root_with(
    AGENT_QT_QT_ROOT "lib/cmake/Qt6/Qt6Config.cmake" ${_qt_root_candidates})
agent_fw_set_first_root_with(
    AGENT_QT_MINGW_BIN "g++.exe" ${_mingw_bin_candidates})
agent_fw_set_first_existing(AGENT_QT_NINJA ${_ninja_candidates})

if(AGENT_QT_QT_ROOT)
    list(PREPEND CMAKE_PREFIX_PATH "${AGENT_QT_QT_ROOT}")
endif()
if(AGENT_QT_MINGW_BIN AND EXISTS "${AGENT_QT_MINGW_BIN}/gcc.exe")
    set(CMAKE_C_COMPILER "${AGENT_QT_MINGW_BIN}/gcc.exe"
        CACHE FILEPATH "AgentFramework MinGW C" FORCE)
endif()
if(AGENT_QT_MINGW_BIN AND EXISTS "${AGENT_QT_MINGW_BIN}/g++.exe")
    set(CMAKE_CXX_COMPILER "${AGENT_QT_MINGW_BIN}/g++.exe"
        CACHE FILEPATH "AgentFramework MinGW C++" FORCE)
endif()
if(CMAKE_GENERATOR MATCHES "Ninja" AND AGENT_QT_NINJA AND EXISTS "${AGENT_QT_NINJA}")
    set(CMAKE_MAKE_PROGRAM "${AGENT_QT_NINJA}"
        CACHE FILEPATH "AgentFramework Ninja" FORCE)
endif()
