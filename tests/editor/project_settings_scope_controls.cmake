# Whole project-settings owner; restore only the former root-selection body.
# The same assertions must fail normally after compilation, without UI or IO.
function(_jce_editor_project_settings_scope_controls)
    set(_owner "${CMAKE_SOURCE_DIR}/editor/src/core/jce_project_settings.cpp")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_owner}")
    file(READ "${_owner}" _source)
    set(_actual [=[void jce_project_settings_set_root(const char *project_root)
{
    const char *next_root = project_root ? project_root : "";
    if (jce_editor_project_path_equal(s_project_root, next_root)) return;

    snprintf(s_project_root, sizeof(s_project_root), "%s", next_root);
    /* A snapshot belongs to one root; selecting an owner never loads another
     * file or mutates already-applied runtime state. Same-root edits survive. */
    s_ps_have = false;
    memset(&s_ps_current, 0, sizeof(s_ps_current));
}
]=])
    set(_old [=[void jce_project_settings_set_root(const char *project_root)
{
    snprintf(s_project_root, sizeof(s_project_root), "%s",
             project_root ? project_root : "");
}
]=])
    string(FIND "${_source}" "${_actual}" _first)
    string(FIND "${_source}" "${_actual}" _last REVERSE)
    if(_first LESS 0 OR NOT _first EQUAL _last)
        message(FATAL_ERROR "Project-settings scope control requires one complete root setter")
    endif()
    string(REPLACE "${_actual}" "${_old}" _body "${_source}")
    if(_body STREQUAL _source)
        message(FATAL_ERROR "Project-settings old root control did not change the owner")
    endif()
    set(_output "${CMAKE_CURRENT_BINARY_DIR}/project-settings-scope/old_set_root.cpp")
    jce_test_write_if_changed("${_output}" "${_body}")
    file(READ "${_output}" _written)
    if(NOT _written STREQUAL _body)
        message(FATAL_ERROR "Project-settings compiler input differs from the selected owner")
    endif()
    jce_add_cxx_test(test_jce_editor_project_settings_old_set_root KIND contract
        SOURCES test_jce_editor_project_settings.cpp "${_output}"
        INCLUDES "${CMAKE_SOURCE_DIR}/editor/src/core"
        LINK jce_core jce_physics jce_resource)
    set_tests_properties(test_jce_editor_project_settings_old_set_root PROPERTIES WILL_FAIL TRUE)
endfunction()
_jce_editor_project_settings_scope_controls()
