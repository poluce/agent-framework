# Compile a probe .cpp with the given include dirs.
# Pass only if the compiler cannot find the forbidden header
# (missing-header diagnostic). A successful compile, or a failure after the
# header was found, means the surface leaked.

if(NOT COMPILER OR NOT SOURCE OR NOT INCLUDES_FILE OR NOT FORBIDDEN)
    message(FATAL_ERROR "need COMPILER, SOURCE, INCLUDES_FILE, FORBIDDEN")
endif()

if(NOT EXISTS "${INCLUDES_FILE}")
    message(FATAL_ERROR "missing includes file: ${INCLUDES_FILE}")
endif()

file(STRINGS "${INCLUDES_FILE}" _raw)
set(_includes)
foreach(_line IN LISTS _raw)
    string(STRIP "${_line}" _line)
    if(_line AND NOT _line MATCHES "^#")
        list(APPEND _includes "-I${_line}")
    endif()
endforeach()

set(_out "${CMAKE_CURRENT_BINARY_DIR}/client_surface_probe.o")
execute_process(
    COMMAND "${COMPILER}" -c -std=c++20 "${SOURCE}" -o "${_out}" ${_includes}
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE)

set(_log "${_stdout}\n${_stderr}")
message(STATUS "compiler rc=${_rc}")
message(STATUS "${_log}")

if(_rc EQUAL 0)
    message(FATAL_ERROR "forbidden include compiled; presentation surface leaked ${FORBIDDEN}")
endif()

string(REPLACE "\\" "/" _log_norm "${_log}")
string(REPLACE "\\" "/" _forb_norm "${FORBIDDEN}")
if(NOT _log_norm MATCHES "No such file")
    if(NOT _log_norm MATCHES "cannot open")
        if(NOT _log_norm MATCHES "file not found")
            message(FATAL_ERROR "compile failed but not with a missing-header error:\n${_log}")
        endif()
    endif()
endif()
if(NOT _log_norm MATCHES "${_forb_norm}")
    message(FATAL_ERROR "compile failed but diagnostic did not name ${FORBIDDEN} (header may have been found):\n${_log}")
endif()
