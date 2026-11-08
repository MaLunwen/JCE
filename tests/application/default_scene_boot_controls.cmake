# Complete stock startup-scene selection/load owner; only typed service ports.
set(_scene_boot_owner "${CMAKE_SOURCE_DIR}/engine/include/jce/application/jce_default_main.inc.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_scene_boot_owner}")
file(READ "${_scene_boot_owner}" _scene_boot_source)
set(_scene_boot_begin "static bool jce_default_load_startup_scene(")
set(_scene_boot_end "/* World-streamer entity callbacks")
foreach(_edge IN ITEMS begin end)
    string(FIND "${_scene_boot_source}" "${_scene_boot_${_edge}}" _scene_boot_${_edge}_pos)
    string(FIND "${_scene_boot_source}" "${_scene_boot_${_edge}}" _scene_boot_last REVERSE)
    if(_scene_boot_${_edge}_pos LESS 0 OR NOT _scene_boot_${_edge}_pos EQUAL _scene_boot_last)
        message(FATAL_ERROR "Scene boot control requires unique complete owner boundaries")
    endif()
endforeach()
math(EXPR _scene_boot_size "${_scene_boot_end_pos} - ${_scene_boot_begin_pos}")
string(SUBSTRING "${_scene_boot_source}" ${_scene_boot_begin_pos} ${_scene_boot_size} _scene_boot_body)
set(_scene_boot_witness [=[    /* Explicit diagnostics observe actual admission, not packaged intent. */
#if !defined(JCE_LOG_NONE)
    const char *perf_log = getenv("JCE_PERF_LOG");
    const char *frame_log = getenv("JCE_KPI_FRAME_LOG");
    if (jce_env_flag_enabled(perf_log) ||
        (frame_log && frame_log[0]) || jce_trace_enabled()) {
        jce_log_write(JCE_LOG_LEVEL_INFO, "app", __FILE__, __LINE__,
                      "startup scene loaded (runtime assets): %s", scene_path);
    } else
#endif
    {
        LOG_INFO("app", "startup scene loaded (runtime assets): %s", scene_path);
    }
]=])
string(FIND "${_scene_boot_body}" "${_scene_boot_witness}" _scene_boot_wire_pos)
string(FIND "${_scene_boot_body}" "${_scene_boot_witness}" _scene_boot_wire_last REVERSE)
if(_scene_boot_wire_pos LESS 0 OR NOT _scene_boot_wire_pos EQUAL _scene_boot_wire_last)
    message(FATAL_ERROR "Scene boot controls require the actual unique diagnostic wire")
endif()
set(_scene_boot_load "    if (!jce_scene_serial_load_vfs(s_scene, s_runtime_fs, scene_path)) {")
string(FIND "${_scene_boot_body}" "${_scene_boot_load}" _scene_boot_load_pos)
string(FIND "${_scene_boot_body}" "${_scene_boot_load}" _scene_boot_load_last REVERSE)
if(_scene_boot_load_pos LESS 0 OR NOT _scene_boot_load_pos EQUAL _scene_boot_load_last)
    message(FATAL_ERROR "Scene boot controls require the actual unique serial-load boundary")
endif()
set(_scene_boot_dir "${CMAKE_CURRENT_BINARY_DIR}/default-scene-boot")
file(MAKE_DIRECTORY "${_scene_boot_dir}")
foreach(_variant IN ITEMS dist release no_logging packaged old_missing early_success)
    set(_body "${_scene_boot_body}")
    set(_kind unit)
    set(_target test_jce_default_scene_boot_witness)
    if(NOT _variant STREQUAL "dist")
        string(APPEND _target "_${_variant}")
    endif()
    if(_variant STREQUAL "old_missing")
        string(REPLACE "${_scene_boot_witness}"
            "    LOG_INFO(\"app\", \"startup scene loaded (runtime assets): %s\", scene_path);\n"
            _body "${_body}")
        set(_kind contract)
    elseif(_variant STREQUAL "early_success")
        string(REPLACE "${_scene_boot_witness}" "" _body "${_body}")
        string(REPLACE "${_scene_boot_load}" "${_scene_boot_witness}${_scene_boot_load}" _body "${_body}")
        set(_kind contract)
    endif()
    set(_path "${_scene_boot_dir}/${_variant}.inc.h")
    jce_test_write_if_changed("${_path}" "${_body}")
    jce_add_unit_test(${_target} KIND ${_kind}
        SOURCES test_jce_default_scene_boot_witness.c)
    target_compile_definitions(${_target} PRIVATE
        JCE_TEST_DEFAULT_SCENE_BOOT_OWNER="${_path}")
    if(_variant STREQUAL "no_logging")
        target_compile_definitions(${_target} PRIVATE JCE_LOG_NONE=1)
    elseif(NOT _variant STREQUAL "release")
        target_compile_definitions(${_target} PRIVATE JCE_DIST=1)
    endif()
    if(_variant STREQUAL "packaged")
        target_compile_definitions(${_target} PRIVATE JCE_DEFAULT_MAIN_PACKAGED_ONLY=1)
    else()
        target_compile_definitions(${_target} PRIVATE JCE_DEFAULT_MAIN_PACKAGED_ONLY=0)
    endif()
    if(_kind STREQUAL "contract")
        set_tests_properties(${_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
foreach(_name IN ITEMS owner source begin end begin_pos end_pos last size body witness wire_pos wire_last
    load load_pos load_last dir)
    unset(_scene_boot_${_name})
endforeach()
unset(_edge)
unset(_variant)
unset(_body)
unset(_kind)
unset(_target)
unset(_path)
unset(_name)
