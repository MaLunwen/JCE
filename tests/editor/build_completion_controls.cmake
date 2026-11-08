# Actual Build state/log/pipe/release/spawn/stop/poll bodies; no compiler/UI.
_jce_project_gate_extract(_build_log_source "struct Build {"
    "\n/* ---------------------------------------------------------------- *\n * Native project-build pipeline state" _build_completion_state)
_jce_project_gate_extract(_build_log_source "struct QueuedStep {"
    "\nvoid clear_build_outputs()" _build_completion_pipeline)
_jce_project_gate_extract(_build_log_source "constexpr int kGracefulStopMs"
    "\n\n#if JCE_PLATFORM_WINDOWS" _build_completion_grace)
_jce_project_gate_extract(_build_log_source "void drain_pipes()"
    "\n/* Refusal belongs to the new request" _build_completion_pipes)
_jce_project_gate_extract(_build_log_source "bool spawn_tool(JceBuildStage stage, const char *exe,"
    "\n/* Resolve which artifact candidate" _build_completion_spawn)
_jce_project_gate_extract(_build_log_source "void poll_state()"
    "\n} // namespace" _build_completion_poll)
_jce_project_gate_extract(_build_log_source "void jce_build_manager_request_stop(void)"
    "\nbool jce_build_manager_is_running" _build_completion_stop)
_jce_project_gate_extract(_build_log_source "void jce_build_manager_poll(void)"
    "\nbool jce_build_manager_repack_game_assets" _build_completion_public_poll)
string(REPLACE "void jce_build_manager_request_stop(void)" "void probe_request_stop(void)"
    _build_completion_stop "${_build_completion_stop}")
string(REPLACE "void jce_build_manager_poll(void)" "void probe_manager_poll(void)"
    _build_completion_public_poll "${_build_completion_public_poll}")

_jce_project_gate_once(_build_completion_poll "const bool cancelled = g_build.stopping")
string(REPLACE "const bool cancelled = g_build.stopping" "const bool cancelled = false"
    _build_completion_old_poll "${_build_completion_poll}")
if(_build_completion_old_poll STREQUAL _build_completion_poll)
    message(FATAL_ERROR "Build completion control must remove its actual Stop wire")
endif()
_jce_project_gate_once(_build_log_lines "log_line(build_output_level(line), origin + line)")
string(REPLACE "log_line(build_output_level(line), origin + line)"
    "log_line(level, origin + line)" _build_completion_old_output "${_build_log_lines}")
if(_build_completion_old_output STREQUAL _build_log_lines)
    message(FATAL_ERROR "Build output control must remove its actual diagnostic severity wire")
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/build-completion-controls")
foreach(_build_completion_variant IN ITEMS normal old_stop old_output)
    set(_build_completion_lines "${_build_log_lines}")
    if(_build_completion_variant STREQUAL "normal")
        set(_build_completion_body "${_build_completion_poll}")
        set(_build_completion_target test_jce_build_process_completion)
        set(_build_completion_kind integration)
    elseif(_build_completion_variant STREQUAL "old_stop")
        set(_build_completion_body "${_build_completion_old_poll}")
        set(_build_completion_target test_jce_build_process_completion_old_stop_wire)
        set(_build_completion_kind contract)
    else()
        set(_build_completion_body "${_build_completion_poll}")
        set(_build_completion_lines "${_build_completion_old_output}")
        set(_build_completion_target test_jce_build_process_completion_old_output_severity)
        set(_build_completion_kind contract)
    endif()
    set(_build_completion_contents "${_build_completion_grace}\n${_build_completion_state}\n${_build_completion_pipeline}\n${_build_completion_lines}\n${_build_completion_pipes}\n${_build_completion_spawn}\n${_build_completion_body}\n${_build_completion_stop}\n${_build_completion_public_poll}\n")
    set(_build_completion_path "${CMAKE_CURRENT_BINARY_DIR}/build-completion-controls/${_build_completion_variant}.inc")
    unset(_build_completion_previous)
    if(EXISTS "${_build_completion_path}")
        file(READ "${_build_completion_path}" _build_completion_previous)
    endif()
    if(NOT _build_completion_previous STREQUAL _build_completion_contents)
        file(WRITE "${_build_completion_path}" "${_build_completion_contents}")
    endif()
    jce_add_cxx_test(${_build_completion_target} KIND ${_build_completion_kind}
        SOURCES test_jce_build_process_completion.cpp LINK jce_core)
    target_compile_features(${_build_completion_target} PRIVATE cxx_std_20)
    target_compile_definitions(${_build_completion_target} PRIVATE
        JCE_TEST_BUILD_COMPLETION_BODIES="${_build_completion_path}")
endforeach()
set_tests_properties(test_jce_build_process_completion_old_stop_wire PROPERTIES WILL_FAIL TRUE)
set_tests_properties(test_jce_build_process_completion_old_output_severity PROPERTIES WILL_FAIL TRUE)
