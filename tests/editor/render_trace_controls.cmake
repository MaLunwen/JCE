# Exact production admission prefixes + complete private CPU trace helpers.
# Counter ports do not replace the actual bounded trace ring.
set(_editor_trace_core "${CMAKE_SOURCE_DIR}/editor/src/core")
set(_editor_trace_scene "${CMAKE_SOURCE_DIR}/editor/src/scene")
set(_editor_trace_dir "${CMAKE_CURRENT_BINARY_DIR}/render-trace-controls")
file(MAKE_DIRECTORY "${_editor_trace_dir}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_editor_trace_core}/jce_editor_cpu_scope.h"
    "${_editor_trace_core}/jce_editor_operation_metrics.h"
    "${_editor_trace_core}/jce_editor_viewport_trace.h"
    "${_editor_trace_core}/jce_editor_viewport_work.h"
    "${_editor_trace_core}/jce_editor_play.cpp"
    "${_editor_trace_core}/jce_editor.cpp"
    "${_editor_trace_core}/jce_editor_state.cpp"
    "${_editor_trace_core}/jce_editor_history.cpp"
    "${_editor_trace_scene}/jce_editor_probe_maintenance.h"
    "${CMAKE_SOURCE_DIR}/editor/src/panels/jce_panel_inspector_lighting.cpp"
    "${CMAKE_SOURCE_DIR}/editor/src/panels/jce_panel_reflection_probes.cpp"
    "${_editor_trace_scene}/jce_editor_scene_render.cpp"
    "${_editor_trace_scene}/jce_editor_game_render.cpp")

function(_jce_editor_trace_write PATH CONTENT)
    jce_test_write_if_changed("${PATH}" "${CONTENT}")
endfunction()

function(_jce_editor_trace_extract OUTPUT INPUT START END)
    string(FIND "${INPUT}" "${START}" _begin)
    string(FIND "${INPUT}" "${START}" _begin_last REVERSE)
    string(FIND "${INPUT}" "${END}" _end)
    string(FIND "${INPUT}" "${END}" _end_last REVERSE)
    if(_begin LESS 0 OR NOT _begin EQUAL _begin_last OR
        NOT _end EQUAL _end_last OR _end LESS_EQUAL _begin)
        message(FATAL_ERROR "Editor trace actual-body bounds must be unique: ${START} -> ${END}")
    endif()
    math(EXPR _size "${_end} - ${_begin}")
    string(SUBSTRING "${INPUT}" ${_begin} ${_size} _body)
    set(${OUTPUT} "${_body}" PARENT_SCOPE)
endfunction()

function(_jce_editor_trace_replace_once OUTPUT INPUT FROM TO)
    string(FIND "${INPUT}" "${FROM}" _first)
    if(_first LESS 0)
        message(FATAL_ERROR "Editor trace exact production replacement is missing: ${FROM}")
    endif()
    string(LENGTH "${FROM}" _from_size)
    math(EXPR _tail_at "${_first} + ${_from_size}")
    string(SUBSTRING "${INPUT}" ${_tail_at} -1 _tail)
    string(FIND "${_tail}" "${FROM}" _second)
    if(NOT _second LESS 0)
        message(FATAL_ERROR "Editor trace production replacement is not unique: ${FROM}")
    endif()
    string(REPLACE "${FROM}" "${TO}" _changed "${INPUT}")
    set(${OUTPUT} "${_changed}" PARENT_SCOPE)
endfunction()

file(READ "${_editor_trace_scene}/jce_editor_scene_render.cpp" _probe_owner)
_jce_editor_trace_extract(_probe_wrappers "${_probe_owner}"
    "/* Render-admitted probe maintenance and current-frame modal ownership. */"
    "/* Sub-attribution of the editor-side viewport frame:")
set(_editor_trace_bodies "static JceEditorProbeMaintenanceState s_probe_maintenance;\n${_probe_wrappers}")
set(_editor_trace_missing_admission "${_editor_trace_bodies}")
foreach(_view IN ITEMS scene game)
    file(READ "${_editor_trace_scene}/jce_editor_${_view}_render.cpp" _owner)
    set(_start "void jce_editor_${_view}_render_frame(uint32_t width, uint32_t height)")
    if(_view STREQUAL "scene")
        set(_end "    /* iGPU ADAPTIVE dynamic resolution")
    else()
        set(_end "    const float frame_dt =")
    endif()
    string(FIND "${_owner}" "${_start}" _begin)
    string(FIND "${_owner}" "${_end}" _finish)
    if(_begin LESS 0 OR _finish LESS_EQUAL _begin)
        message(FATAL_ERROR "Editor ${_view} render admission extraction bounds changed")
    endif()
    math(EXPR _size "${_finish} - ${_begin}")
    string(SUBSTRING "${_owner}" ${_begin} ${_size} _body)
    _jce_editor_trace_replace_once(_body "${_body}" "${_start}"
        "static void production_${_view}_admission(uint32_t width, uint32_t height)")
    string(APPEND _body "    ++ports.${_view}_admitted;\n}\n")
    _jce_editor_trace_replace_once(_mutant "${_body}" "render_trace.admitted();"
        "/* ordinary missing admission observation wire */")
    string(APPEND _editor_trace_bodies "${_body}")
    string(APPEND _editor_trace_missing_admission "${_mutant}")
endforeach()
_jce_editor_trace_write("${_editor_trace_dir}/admission_normal.inc.h" "${_editor_trace_bodies}")
_jce_editor_trace_write("${_editor_trace_dir}/admission_missing.inc.h" "${_editor_trace_missing_admission}")

file(READ "${_editor_trace_core}/jce_editor.cpp" _probe_host)
_jce_editor_trace_extract(_probe_host_wire "${_probe_host}"
    "    /* Render-admitted modal capture maintenance. Never move to CPU services. */"
    "    /* Draw editor panels.  ed_layout spans")
_jce_editor_trace_write("${_editor_trace_dir}/probe_host.inc.h" "${_probe_host_wire}")
_jce_editor_trace_replace_once(_probe_host_missing "${_probe_host_wire}"
    "jce_editor_scene_render_maintenance(w > 0 && h > 0 &&\n        (!io.AppFocusLost || jce_editor_viewport_capture_requested(false)));"
    "(void)w; (void)h; (void)io; /* ordinary omitted render-admitted pump */")
_jce_editor_trace_write("${_editor_trace_dir}/probe_host_missing.inc.h" "${_probe_host_missing}")
file(READ "${_editor_trace_scene}/jce_editor_probe_maintenance.h" _probe_helper)
_jce_editor_trace_replace_once(_probe_missing_busy "${_probe_helper}"
    "    state.busy_this_frame = true;" "    state.busy_this_frame = false; /* ordinary missing frame latch */")
_jce_editor_trace_write("${_editor_trace_dir}/probe_missing_busy.h" "${_probe_missing_busy}")
_jce_editor_trace_replace_once(_probe_missing_cancel "${_editor_trace_bodies}"
    "    jce_editor_probe_maintenance_cancel(s_probe_maintenance);" "    /* ordinary omitted safe cancellation wire */")
_jce_editor_trace_write("${_editor_trace_dir}/admission_missing_cancel.inc.h" "${_probe_missing_cancel}")

# Destructive callsite prefixes retain the actual cancellation order.
_jce_editor_trace_extract(_probe_shutdown "${_probe_owner}"
    "void jce_editor_scene_render_shutdown(void)" "    sr_destroy_ui_canvas();\n    if (s_sr.pick_pass)")
string(REPLACE "void jce_editor_scene_render_shutdown(void)" "static void production_probe_shutdown_prefix()"
    _probe_shutdown "${_probe_shutdown}")
string(APPEND _probe_shutdown "    probe_renderer_release_port();\n}\n")
file(READ "${_editor_trace_core}/jce_editor_state.cpp" _probe_state)
_jce_editor_trace_extract(_probe_clear "${_probe_state}" "void clear_scene_entities(void)"
    "/* When true, jce_state_add_component()")
_jce_editor_trace_extract(_probe_state_stop "${_probe_state}" "void jce_editor_state_shutdown(void)"
    "    jce_state_close_bundle_preview();\n    g_entity_order.clear();")
string(REPLACE "void jce_editor_state_shutdown(void)" "static void production_probe_state_shutdown_prefix()"
    _probe_state_stop "${_probe_state_stop}")
string(APPEND _probe_state_stop "}\n")
file(READ "${_editor_trace_core}/jce_editor_history.cpp" _probe_history)
_jce_editor_trace_extract(_probe_history_clear "${_probe_history}"
    "    HistorySuspendScope suspend;\n\n    /* Keep the scene object and its shared mutable asset caches alive. Terrain"
    "    /* Reloaded entities receive fresh handles")
_jce_editor_trace_write("${_editor_trace_dir}/probe_destructive.inc.h"
    "${_probe_shutdown}\n${_probe_clear}\n${_probe_state_stop}\nstatic bool production_probe_history_clear_prefix()\n{\n    bool *mutated = nullptr; JceJson *root = nullptr;\n${_probe_history_clear}\n    return true;\n}\n")

# Actual Inspector bake branch: counted UI requests do not certify a native click.
file(READ "${CMAKE_SOURCE_DIR}/editor/src/panels/jce_panel_inspector_lighting.cpp" _probe_inspector)
_jce_editor_trace_extract(_probe_inspector_body "${_probe_inspector}"
    "    static JceReflectionProbeBakeHandle s_active_handle = 0;"
    "void draw_comp_light_probe_group(")
_jce_editor_trace_write("${_editor_trace_dir}/probe_inspector.inc.h"
    "static void production_probe_inspector(JceScene *scene, JceEntity e, JceReflectionProbeComponent *r)\n{\n${_probe_inspector_body}")
_jce_editor_trace_replace_once(_probe_inspector_old_identity "${_probe_inspector_body}"
    "jce_editor_scene_render_probe_capture_matches(s_capture_request, scene, e)"
    "jce_scene_probe_capture_in_flight()")
_jce_editor_trace_write("${_editor_trace_dir}/probe_inspector_old_identity.inc.h"
    "static void production_probe_inspector(JceScene *scene, JceEntity e, JceReflectionProbeComponent *r)\n{\n${_probe_inspector_old_identity}")
file(READ "${CMAKE_SOURCE_DIR}/editor/src/panels/jce_panel_reflection_probes.cpp" _probe_panel)
_jce_editor_trace_extract(_probe_panel_body "${_probe_panel}"
    "static std::vector<JceEntity> &rp_bake_queue(void)"
    "extern \"C\" void jce_editor_panel_reflection_probes_content(void)")
_jce_editor_trace_write("${_editor_trace_dir}/probe_panel.inc.h" "${_probe_panel_body}")
foreach(_probe_entry IN ITEMS _probe_panel _probe_inspector)
    string(FIND "${${_probe_entry}}" "jce_scene_reflection_probe_capture(" _probe_direct)
    if(NOT _probe_direct LESS 0)
        message(FATAL_ERROR "Editor probe entry must use its shared request identity owner")
    endif()
endforeach()

file(READ "${_editor_trace_core}/jce_editor_cpu_scope.h" _cpu_scope)
_jce_editor_trace_replace_once(_cpu_old "${_cpu_scope}"
    "if (!jce_trace_enabled()) return;" "return; /* ordinary omitted CPU trace scope */")
_jce_editor_trace_write("${_editor_trace_dir}/jce_editor_cpu_scope_missing.h" "${_cpu_old}")
file(READ "${_editor_trace_core}/jce_editor_operation_metrics.h" _operation)
_jce_editor_trace_replace_once(_operation_old "${_operation}"
    "#include \"jce_editor_cpu_scope.h\""
    "#include \"${_editor_trace_dir}/jce_editor_cpu_scope_missing.h\"")
_jce_editor_trace_write("${_editor_trace_dir}/jce_editor_operation_missing.h" "${_operation_old}")

# Actual Play scope placement must remain in all three synchronous owners.
# CPU fixture checks the complete RAII/ring, not actual Runtime/GUI execution.
file(READ "${_editor_trace_core}/jce_editor_play.cpp" _play_owner)
foreach(_phase IN ITEMS request begin stop)
    set(_wire "JceEditorCpuScope play_scope(\"editor.play.${_phase}\");")
    _jce_editor_trace_replace_once(_unused "${_play_owner}" "${_wire}" "")
endforeach()

foreach(_variant IN ITEMS normal missing_cpu missing_admission missing_maintenance missing_busy missing_cancel old_capture_identity)
    if(_variant STREQUAL "normal")
        set(_target test_jce_editor_render_trace)
        set(_kind integration)
    else()
        set(_target "test_jce_editor_render_trace_${_variant}")
        set(_kind contract)
    endif()
    jce_add_cxx_test(${_target} KIND ${_kind}
        SOURCES test_jce_editor_render_trace.cpp
        LINK jce_core jce_tools_imgui)
    target_compile_features(${_target} PRIVATE cxx_std_20)
    set(_admission normal)
    set(_inspector probe_inspector)
    if(_variant STREQUAL "missing_admission")
        set(_admission missing)
    elseif(_variant STREQUAL "missing_cancel")
        set(_admission missing_cancel)
    elseif(_variant STREQUAL "missing_cpu")
        target_compile_definitions(${_target} PRIVATE
            JCE_TEST_EDITOR_OPERATION_METRICS_HEADER="${_editor_trace_dir}/jce_editor_operation_missing.h")
    elseif(_variant STREQUAL "old_capture_identity")
        set(_inspector probe_inspector_old_identity)
    endif()
    target_compile_definitions(${_target} PRIVATE
        JCE_TEST_EDITOR_RENDER_ADMISSION_BODIES="${_editor_trace_dir}/admission_${_admission}.inc.h"
        JCE_TEST_EDITOR_PROBE_DESTRUCTIVE_BODIES="${_editor_trace_dir}/probe_destructive.inc.h"
        JCE_TEST_EDITOR_PROBE_INSPECTOR_BODY="${_editor_trace_dir}/${_inspector}.inc.h"
        JCE_TEST_EDITOR_PROBE_PANEL_BODY="${_editor_trace_dir}/probe_panel.inc.h")
    if(_variant STREQUAL "missing_maintenance")
        target_compile_definitions(${_target} PRIVATE
            JCE_TEST_EDITOR_PROBE_HOST_BODY="${_editor_trace_dir}/probe_host_missing.inc.h")
    else()
        target_compile_definitions(${_target} PRIVATE
            JCE_TEST_EDITOR_PROBE_HOST_BODY="${_editor_trace_dir}/probe_host.inc.h")
    endif()
    if(_variant STREQUAL "missing_busy")
        target_compile_definitions(${_target} PRIVATE
            JCE_TEST_EDITOR_PROBE_MAINTENANCE_HEADER="${_editor_trace_dir}/probe_missing_busy.h")
    endif()
    if(NOT _variant STREQUAL "normal")
        set_tests_properties(${_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
