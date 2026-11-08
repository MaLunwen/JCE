# Exact project transition through RP publication, actual public close entry and
# actual pending-Play guard. Later localization/settings UI is outside this test.
function(_jce_bundle_retirement_extract source start end output)
    string(FIND "${source}" "${start}" _first)
    string(FIND "${source}" "${start}" _last REVERSE)
    string(FIND "${source}" "${end}" _end)
    if(_first LESS 0 OR NOT _first EQUAL _last OR NOT _end GREATER _first)
        message(FATAL_ERROR "Bundle retirement requires unique ordered actual boundaries: ${start}")
    endif()
    math(EXPR _length "${_end} - ${_first}")
    string(SUBSTRING "${source}" ${_first} ${_length} _body)
    set(${output} "${_body}" PARENT_SCOPE)
endfunction()
function(_jce_bundle_retirement_require_count source wire expected)
    string(REPLACE "${wire}" "" _without "${source}")
    string(LENGTH "${source}" _before)
    string(LENGTH "${_without}" _after)
    string(LENGTH "${wire}" _width)
    math(EXPR _removed "${_before} - ${_after}")
    math(EXPR _required "${_width} * ${expected}")
    if(NOT _removed EQUAL _required)
        message(FATAL_ERROR "Bundle retirement requires ${expected} actual wires: ${wire}")
    endif()
endfunction()
# Scope fixture variables independently of other included registration fragments.
function(_jce_register_project_bundle_retirement_controls)
set(_retire_dialog "${CMAKE_SOURCE_DIR}/editor/src/dialogs/jce_dialog_project.cpp")
set(_retire_scene "${CMAKE_SOURCE_DIR}/editor/src/io/jce_editor_scene_serial.cpp")
set(_retire_play "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_play.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_retire_dialog}" "${_retire_scene}" "${_retire_play}")
file(READ "${_retire_dialog}" _retire_dialog_source)
file(READ "${_retire_scene}" _retire_scene_source)
file(READ "${_retire_play}" _retire_play_source)
_jce_bundle_retirement_extract("${_retire_dialog_source}"
    "bool jce_editor_prepare_project_switch(const char *next_root)"
    "\nbool set_current_project_root" _retire_preflight)
_jce_bundle_retirement_extract("${_retire_dialog_source}"
    "bool set_current_project_root(const char *path)"
    "    /* Install the project's asset-decryption key" _retire_transition)
string(APPEND _retire_transition "    return true;\n}\n")
_jce_bundle_retirement_extract("${_retire_scene_source}"
    "bool jce_state_close_bundle_preview(void)"
    "\nbool jce_state_load_scene_from_jbundle" _retire_close)
_jce_bundle_retirement_extract("${_retire_play_source}"
    "bool stop_play_before_scene_swap(void)"
    "\nvoid jce_state_step(float dt)" _retire_stop)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/project-bundle-retirement")
foreach(_case IN ITEMS current old_missing_retire old_close_before_stop)
    set(_transition "${_retire_transition}")
    set(_close "${_retire_close}")
    set(_target test_jce_editor_project_bundle_retirement)
    set(_kind integration)
    if(_case STREQUAL "old_missing_retire")
        set(_wire "if (!jce_state_close_bundle_preview()) return false;")
        _jce_bundle_retirement_require_count("${_transition}" "${_wire}" 2)
        string(REPLACE "${_wire}" "/* old: no Bundle retirement */"
            _transition "${_transition}")
        _jce_bundle_retirement_require_count("${_transition}" "${_wire}" 0)
    elseif(_case STREQUAL "old_close_before_stop")
        set(_wire "    if (!stop_play_before_scene_swap()) return false;\n    jce_editor_scene_render_streaming_teardown();\n    close_active_bundle_mount();")
        _jce_bundle_retirement_require_count("${_close}" "${_wire}" 1)
        string(REPLACE "${_wire}"
            "    close_active_bundle_mount();\n    if (!stop_play_before_scene_swap()) return false;\n    jce_editor_scene_render_streaming_teardown();"
            _close "${_close}")
        _jce_bundle_retirement_require_count("${_close}" "${_wire}" 0)
    endif()
    if(NOT _case STREQUAL "current")
        string(APPEND _target "_${_case}")
        set(_kind contract)
    endif()
    set(_path "${CMAKE_CURRENT_BINARY_DIR}/project-bundle-retirement/${_case}.inc.h")
    set(_content "${_retire_stop}\n${_close}\n${_retire_preflight}\n${_transition}")
    jce_test_write_if_changed("${_path}" "${_content}")
    file(READ "${_path}" _written)
    if(NOT "${_written}" STREQUAL "${_content}")
        message(FATAL_ERROR "Bundle retirement generated ${_case} differs from its selected owner")
    endif()
    # Validate the actual compiler input, not only the temporary replacement.
    if(_case STREQUAL "old_missing_retire")
        _jce_bundle_retirement_require_count("${_written}" "${_wire}" 0)
        _jce_bundle_retirement_require_count("${_written}" "/* old: no Bundle retirement */" 2)
    elseif(_case STREQUAL "old_close_before_stop")
        _jce_bundle_retirement_require_count("${_written}"
            "    close_active_bundle_mount();\n    if (!stop_play_before_scene_swap()) return false;\n    jce_editor_scene_render_streaming_teardown();" 1)
        _jce_bundle_retirement_require_count("${_written}" "${_wire}" 0)
    endif()
    jce_add_cxx_test(${_target} KIND ${_kind}
        SOURCES test_jce_editor_project_bundle_retirement.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/io/jce_editor_bundle_render_pipeline.cpp"
            "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_render_pipeline.c"
        LINK jce_core jce_resource
        INCLUDES "${CMAKE_SOURCE_DIR}/editor/src" "${CMAKE_SOURCE_DIR}/engine/src")
    target_compile_definitions(${_target} PRIVATE JCE_TEST_PROJECT_BUNDLE_RETIREMENT_BODY="${_path}")
    target_compile_features(${_target} PRIVATE cxx_std_20)
    set_tests_properties(${_target} PROPERTIES ENVIRONMENT "${JCE_TEST_ENVIRONMENT};JCE_RP_FORCE=")
    if(NOT _case STREQUAL "current")
        set_tests_properties(${_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
endfunction()
_jce_register_project_bundle_retirement_controls()
