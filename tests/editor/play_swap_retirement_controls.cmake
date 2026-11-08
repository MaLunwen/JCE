# Actual Play transaction, history capture/restore and entry admission bodies.
function(_jce_play_swap_extract source start finish output)
    string(FIND "${source}" "${start}" _begin)
    string(FIND "${source}" "${start}" _last REVERSE)
    if(_begin LESS 0 OR NOT _begin EQUAL _last)
        message(FATAL_ERROR "Play retirement requires one source start: ${start}")
    endif()
    string(SUBSTRING "${source}" ${_begin} -1 _tail)
    string(FIND "${_tail}" "${finish}" _length)
    if(NOT _length GREATER 0)
        message(FATAL_ERROR "Play retirement requires unique ordered source boundaries: ${start}")
    endif()
    string(SUBSTRING "${source}" ${_begin} ${_length} _body)
    set(${output} "${_body}" PARENT_SCOPE)
endfunction()
function(_jce_play_swap_replace source old replacement output)
    string(FIND "${source}" "${old}" _first)
    string(FIND "${source}" "${old}" _last REVERSE)
    if(_first LESS 0 OR NOT _first EQUAL _last)
        message(FATAL_ERROR "Play retirement control requires exactly one actual wire: ${old}")
    endif()
    string(REPLACE "${old}" "${replacement}" _body "${source}")
    set(${output} "${_body}" PARENT_SCOPE)
endfunction()
set(_ps_core "${CMAKE_SOURCE_DIR}/editor/src/core")
set(_ps_io "${CMAKE_SOURCE_DIR}/editor/src/io")
set(_ps_dialog "${CMAKE_SOURCE_DIR}/editor/src/dialogs/jce_dialog_project.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_ps_core}/jce_editor_play.cpp" "${_ps_core}/jce_editor_history.cpp"
    "${_ps_core}/jce_editor_state.cpp" "${_ps_io}/jce_editor_scene_serial.cpp"
    "${_ps_io}/jce_editor_scene_parse.cpp" "${_ps_dialog}")
file(READ "${_ps_core}/jce_editor_play.cpp" _ps_play)
file(READ "${_ps_core}/jce_editor_history.cpp" _ps_history)
file(READ "${_ps_core}/jce_editor_state.cpp" _ps_state)
file(READ "${_ps_io}/jce_editor_scene_serial.cpp" _ps_scene)
file(READ "${_ps_io}/jce_editor_scene_parse.cpp" _ps_parse)
file(READ "${_ps_dialog}" _ps_project)
_jce_play_swap_extract("${_ps_history}" "bool history_capture_snapshot(EditorHistorySnapshot *out)"
    "/* One entity's components" _ps_capture)
_jce_play_swap_extract("${_ps_history}" "bool history_restore_snapshot(const EditorHistorySnapshot &snapshot,"
    "/* ── Undo / Redo" _ps_restore)
_jce_play_swap_extract("${_ps_play}" "void jce_state_stop(void)" "\nvoid jce_state_step(float dt)" _ps_stop)
_jce_play_swap_extract("${_ps_scene}" "bool jce_state_close_bundle_preview(void)"
    "\nbool jce_state_load_scene_from_jbundle" _ps_close)
_jce_play_swap_extract("${_ps_project}" "bool set_current_project_root(const char *path)"
    "    const char *root = jce_editor_current_project_root();" _ps_root)
string(APPEND _ps_root "    return true;\n}\n")
# Exact entry prefixes execute the shared gate. Check every destructive owner
# wire remains after it in the full source, rather than testing a parallel gate.
_jce_play_swap_extract("${_ps_scene}" "bool jce_state_load_scene_file(const char *scene_path)"
    "\nstruct DeferredSceneLoad" _ps_sync)
_jce_play_swap_extract("${_ps_scene}" "bool jce_state_load_scene_file_async(const char *scene_path)"
    "\nbool jce_state_is_scene_loading" _ps_async)
_jce_play_swap_extract("${_ps_scene}" "bool apply_scene_bytes(const char *display_path,"
    "} /* namespace */" _ps_bytes)
_jce_play_swap_extract("${_ps_scene}" "bool jce_state_load_scene_from_jbundle(const char *jbundle_path)"
    "\nbool jce_state_load_scene_from_catalog" _ps_bundle)
# Catalog is the last entry in this TU; its complete tail still supplies ordering.
string(FIND "${_ps_scene}" "bool jce_state_load_scene_from_catalog(const char *catalog_path," _ps_catalog_start)
string(SUBSTRING "${_ps_scene}" ${_ps_catalog_start} -1 _ps_catalog)
_jce_play_swap_extract("${_ps_parse}" "bool load_scene_from_parsed_root(const JceJson *root,"
    "    /* Clear editor state" _ps_parsed)
_jce_play_swap_extract("${_ps_state}" "bool jce_state_new_default_scene_for_project(bool is_2d)"
    "\nvoid jce_editor_state_shutdown(void)" _ps_new)
set(_ps_entries "")
foreach(_entry IN ITEMS sync async bytes bundle catalog parsed new)
    set(_body "${_ps_${_entry}}")
    set(_wire "if (!stop_play_before_scene_swap()) return false;")
    string(FIND "${_body}" "${_wire}" _gate)
    string(FIND "${_body}" "${_wire}" _last REVERSE)
    if(_gate LESS 0 OR NOT _gate EQUAL _last)
        message(FATAL_ERROR "Play retirement ${_entry} requires one early refusal gate")
    endif()
    foreach(_destructive IN ITEMS "close_active_bundle_mount();" "clear_scene_entities();"
        "jce_scene_load_stream_step(" "jce_editor_scene_render_streaming_teardown();")
        string(FIND "${_body}" "${_destructive}" _action)
        if(NOT _action LESS 0 AND NOT _gate LESS _action)
            message(FATAL_ERROR "Play retirement ${_entry} gate must precede ${_destructive}")
        endif()
    endforeach()
    string(LENGTH "${_wire}" _wire_length)
    math(EXPR _length "${_gate} + ${_wire_length}")
    string(SUBSTRING "${_body}" 0 ${_length} _prefix)
    string(APPEND _ps_entries "${_prefix}\n    ++scene_commits; return true;\n}\n")
endforeach()
set(_ps_dir "${CMAKE_CURRENT_BINARY_DIR}/play-swap-retirement")
file(MAKE_DIRECTORY "${_ps_dir}")
jce_test_write_if_changed("${_ps_dir}/history.inc.h" "${_ps_capture}\n${_ps_restore}")
jce_test_write_if_changed("${_ps_dir}/project.inc.h" "${_ps_root}")
jce_test_write_if_changed("${_ps_dir}/entries.inc.h" "${_ps_entries}")
# Actual Stop pulls Runtime and Engine owners, so consume the supported JCE
# facade rather than a partial Application dependency list. This executable has
# no embedded assets; use the same zero-size blob contract as the empty project
# template and full-engine headless fixtures. ECS/SceneSerial remain real.
set(_ps_empty_pak "${_ps_dir}/empty_assets.c")
jce_test_write_if_changed("${_ps_empty_pak}"
    "#include <stddef.h>\nconst unsigned char assets_pak_data[1] = {0};\nconst size_t assets_pak_data_size = 0;\n")
foreach(_case IN ITEMS current old_skip_retained old_ignore_refusal old_false_terminal_success old_unconditional_terminal_stop)
    set(_stop "${_ps_stop}")
    set(_close "${_ps_close}")
    set(_target test_jce_editor_play_swap_retirement)
    set(_kind integration)
    if(_case STREQUAL "old_skip_retained")
        _jce_play_swap_replace("${_stop}" " || s_play_host.viewport.pending ||\n        s_play_snapshot_valid) {"
            " || s_play_host.viewport.pending) {" _stop)
    elseif(_case STREQUAL "old_ignore_refusal")
        _jce_play_swap_replace("${_close}" "if (!stop_play_before_scene_swap()) return false;"
            "(void)stop_play_before_scene_swap(); /* old ignored recovery refusal */" _close)
    elseif(_case STREQUAL "old_false_terminal_success")
        _jce_play_swap_replace("${_stop}" "shutdown_scope.result(JCE_TRACE_TASK_FAILED);"
            "/* old: falsely successful terminal discard */" _stop)
    elseif(_case STREQUAL "old_unconditional_terminal_stop")
        _jce_play_swap_replace("${_stop}"
            "    if (s.play_state != JCE_PLAY_STOPPED || s_play_host.viewport.pending ||\n        s_play_snapshot_valid)\n        jce_state_stop();"
            "    jce_state_stop(); /* old: repeat Stop for an already retired owner */" _stop)
    endif()
    if(NOT _case STREQUAL "current")
        string(APPEND _target "_${_case}")
        set(_kind contract)
    endif()
    jce_test_write_if_changed("${_ps_dir}/${_case}-stop.inc.h" "${_stop}")
    jce_test_write_if_changed("${_ps_dir}/${_case}-close.inc.h" "${_close}")
    jce_add_cxx_test(${_target} KIND ${_kind}
        SOURCES test_jce_editor_play_swap_retirement.cpp "${_ps_empty_pak}"
        LINK JCE
        INCLUDES "${CMAKE_SOURCE_DIR}/editor/src")
    target_compile_features(${_target} PRIVATE cxx_std_20)
    target_compile_definitions(${_target} PRIVATE
        JCE_TEST_PLAY_SWAP_HISTORY="${_ps_dir}/history.inc.h"
        JCE_TEST_PLAY_SWAP_STOP="${_ps_dir}/${_case}-stop.inc.h"
        JCE_TEST_PLAY_SWAP_CLOSE="${_ps_dir}/${_case}-close.inc.h"
        JCE_TEST_PLAY_SWAP_PROJECT="${_ps_dir}/project.inc.h"
        JCE_TEST_PLAY_SWAP_ENTRIES="${_ps_dir}/entries.inc.h")
    if(NOT _case STREQUAL "current")
        set_tests_properties(${_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
