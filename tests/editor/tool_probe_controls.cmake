# Complete private owner, typed counted public process/clock ports. Mutants
# remove actual admission/cleanup wires and must fail ordinary assertions.
set(_tool_probe_owner "${CMAKE_SOURCE_DIR}/editor/src/core/jce_build_tool_probe.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_tool_probe_owner}")
file(READ "${_tool_probe_owner}" _tool_probe_source)

function(_jce_build_tool_probe_target NAME KIND OWNER)
    jce_add_cxx_test(${NAME} KIND ${KIND}
        SOURCES test_jce_build_tool_probe.cpp "${OWNER}"
        INCLUDES "${CMAKE_SOURCE_DIR}/editor/src/core"
        LINK jce_core)
    target_compile_definitions(${NAME} PRIVATE
        jce_process_spawn=jce_test_tool_probe_spawn
        jce_process_read_stdout=jce_test_tool_probe_stdout
        jce_process_read_stderr=jce_test_tool_probe_stderr
        jce_process_poll_exit=jce_test_tool_probe_poll
        jce_process_force_kill=jce_test_tool_probe_kill
        jce_process_destroy=jce_test_tool_probe_destroy
        jce_process_get_last_spawn_error=jce_test_tool_probe_spawn_error
        jce_time_ticks_ms=jce_test_tool_probe_ticks
        jce_thread_sleep_ms=jce_test_tool_probe_sleep)
endfunction()

_jce_build_tool_probe_target(test_jce_build_tool_probe unit "${_tool_probe_owner}")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tool-probe-controls")
foreach(_tool_probe_variant IN ITEMS deadline stderr output exit cleanup final_drain)
    if(_tool_probe_variant STREQUAL "deadline")
        set(_tool_probe_from [=[return jce_time_ticks_ms() - started >= JCE_BUILD_TOOL_PROBE_TIMEOUT_MS;]=])
        set(_tool_probe_to [=[return false;]=])
    elseif(_tool_probe_variant STREQUAL "stderr")
        set(_tool_probe_from "first_nonempty_line(result.stdout_prefix, stdout_size, result.version)")
        set(_tool_probe_to "first_nonempty_line(stdout_size ? result.stdout_prefix : result.stderr_prefix, stdout_size ? stdout_size : stderr_size, result.version)")
    elseif(_tool_probe_variant STREQUAL "output")
        set(_tool_probe_from "result.observed_bytes > JCE_BUILD_TOOL_PROBE_OUTPUT_LIMIT")
        set(_tool_probe_to "false")
    elseif(_tool_probe_variant STREQUAL "exit")
        set(_tool_probe_from "result.exit_code != 0")
        set(_tool_probe_to "false")
    elseif(_tool_probe_variant STREQUAL "cleanup")
        set(_tool_probe_from [=[jce_process_destroy(process);]=])
        set(_tool_probe_to "/* missing public cleanup wire */")
    elseif(_tool_probe_variant STREQUAL "final_drain")
        set(_tool_probe_from "was_exited && !progress")
        set(_tool_probe_to "result.exited && !progress")
    endif()
    # CMake's list separator must not count C++ semicolons as multiple matches.
    string(FIND "${_tool_probe_source}" "${_tool_probe_from}" _tool_probe_at)
    string(LENGTH "${_tool_probe_from}" _tool_probe_from_len)
    if(_tool_probe_at LESS 0)
        message(FATAL_ERROR "Tool probe ${_tool_probe_variant} control requires its actual owner wire")
    endif()
    math(EXPR _tool_probe_after "${_tool_probe_at} + ${_tool_probe_from_len}")
    string(SUBSTRING "${_tool_probe_source}" ${_tool_probe_after} -1 _tool_probe_tail)
    string(FIND "${_tool_probe_tail}" "${_tool_probe_from}" _tool_probe_duplicate)
    if(NOT _tool_probe_duplicate EQUAL -1)
        message(FATAL_ERROR "Tool probe ${_tool_probe_variant} control requires one unique owner wire")
    endif()
    string(REPLACE "${_tool_probe_from}" "${_tool_probe_to}" _tool_probe_mutant "${_tool_probe_source}")
    set(_tool_probe_mutant_path "${CMAKE_CURRENT_BINARY_DIR}/tool-probe-controls/jce_build_tool_probe_old_${_tool_probe_variant}.cpp")
    unset(_tool_probe_previous)
    if(EXISTS "${_tool_probe_mutant_path}")
        file(READ "${_tool_probe_mutant_path}" _tool_probe_previous)
    endif()
    if(NOT _tool_probe_previous STREQUAL _tool_probe_mutant)
        file(WRITE "${_tool_probe_mutant_path}" "${_tool_probe_mutant}")
    endif()
    set(_tool_probe_target "test_jce_build_tool_probe_old_${_tool_probe_variant}_wire")
    _jce_build_tool_probe_target(${_tool_probe_target} contract "${_tool_probe_mutant_path}")
    set_tests_properties(${_tool_probe_target} PROPERTIES WILL_FAIL TRUE)
endforeach()
