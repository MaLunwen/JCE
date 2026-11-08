# Complete offscreen owner; actual session admission/accessors, typed CPU ports.
set(_resource_diag_renderer "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_renderer.c")
set(_resource_diag_offscreen "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_offscreen_target.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_resource_diag_renderer}" "${_resource_diag_offscreen}")
file(READ "${_resource_diag_renderer}" _resource_diag_renderer_source)
file(READ "${_resource_diag_offscreen}" _resource_diag_source)
set(_resource_diag_dir "${CMAKE_CURRENT_BINARY_DIR}/offscreen-resource-diag")
file(MAKE_DIRECTORY "${_resource_diag_dir}")
foreach(_resource_diag_part IN ITEMS session accessors)
    if(_resource_diag_part STREQUAL "session")
        set(_resource_diag_begin "bool jce_renderer_backend_create_begin_internal(void)")
        set(_resource_diag_end "void jce_renderer_backend_create_end_internal(bool created)")
    else()
        set(_resource_diag_begin "JceOffscreenTarget **jce_renderer_offscreen_head(const JceRenderer *r)")
        set(_resource_diag_end "bool jce_renderer_frame_admission_current_internal(const JceRenderer *r)")
    endif()
    string(FIND "${_resource_diag_renderer_source}" "${_resource_diag_begin}" _resource_diag_start)
    string(FIND "${_resource_diag_renderer_source}" "${_resource_diag_begin}" _resource_diag_last REVERSE)
    string(FIND "${_resource_diag_renderer_source}" "${_resource_diag_end}" _resource_diag_stop)
    string(FIND "${_resource_diag_renderer_source}" "${_resource_diag_end}" _resource_diag_stop_last REVERSE)
    if(_resource_diag_start LESS 0 OR NOT _resource_diag_start EQUAL _resource_diag_last OR
       _resource_diag_stop LESS_EQUAL _resource_diag_start OR
       NOT _resource_diag_stop EQUAL _resource_diag_stop_last)
        message(FATAL_ERROR "Offscreen diagnostic ${_resource_diag_part} requires unique actual owner boundaries")
    endif()
    math(EXPR _resource_diag_length "${_resource_diag_stop} - ${_resource_diag_start}")
    string(SUBSTRING "${_resource_diag_renderer_source}" ${_resource_diag_start}
        ${_resource_diag_length} _resource_diag_body)
    jce_test_write_if_changed("${_resource_diag_dir}/${_resource_diag_part}.inc.h" "${_resource_diag_body}")
endforeach()

foreach(_resource_diag_variant IN ITEMS current requested_as_actual missing_view missing_dedupe missing_gate old_single_line missing_rect_height)
    set(_resource_diag_selected "${_resource_diag_source}")
    set(_resource_diag_target test_jce_offscreen_resource_diag)
    set(_resource_diag_kind unit)
    if(NOT _resource_diag_variant STREQUAL "current")
        string(APPEND _resource_diag_target "_${_resource_diag_variant}")
        set(_resource_diag_kind contract)
        set(_resource_diag_expected 1)
        if(_resource_diag_variant STREQUAL "requested_as_actual")
            set(_resource_diag_needle "    view_resource_diag(bridge, width, height, rect_w, rect_h, fbo);")
            set(_resource_diag_replacement
                "    view_resource_diag(bridge, width, height, (uint16_t)width, (uint16_t)height, fbo);")
        elseif(_resource_diag_variant STREQUAL "missing_view")
            set(_resource_diag_needle "    view_resource_diag(bridge, width, height, rect_w, rect_h, fbo);")
            set(_resource_diag_replacement "    /* Ordinary missing observation wire. */")
        elseif(_resource_diag_variant STREQUAL "missing_dedupe")
            set(_resource_diag_needle
                "    if (bridge->view_diag_valid && bridge->view_diag_serial == bridge->identity.serial &&\n        bridge->view_diag_generation == bridge->identity.capture_generation &&\n        bridge->view_diag_fbo == fbo.idx && bridge->view_diag_w == rect_w &&\n        bridge->view_diag_h == rect_h) return;")
            set(_resource_diag_replacement "    /* Ordinary missing tuple reuse guard. */")
        elseif(_resource_diag_variant STREQUAL "missing_rect_height")
            set(_resource_diag_needle
                "    jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,\n        \"offscreen.forwarded_rect_height\", rect_h, id->serial, parent);")
            set(_resource_diag_replacement "    /* Ordinary missing configure rectangle-height field. */")
        elseif(_resource_diag_variant STREQUAL "old_single_line")
            set(_resource_diag_log_begin "    jce_log_write(JCE_LOG_LEVEL_INFO, LOG_TAG, __FILE__, __LINE__,\n        \"render-resource: schema=2 part=1")
            set(_resource_diag_log_end "        reason, destroy_kind, (unsigned)destroy_handle);")
            string(FIND "${_resource_diag_source}" "${_resource_diag_log_begin}" _resource_diag_log_start)
            string(FIND "${_resource_diag_source}" "${_resource_diag_log_begin}" _resource_diag_log_start_last REVERSE)
            string(FIND "${_resource_diag_source}" "${_resource_diag_log_end}" _resource_diag_log_stop)
            string(FIND "${_resource_diag_source}" "${_resource_diag_log_end}" _resource_diag_log_stop_last REVERSE)
            if(_resource_diag_log_start LESS 0 OR NOT _resource_diag_log_start EQUAL _resource_diag_log_start_last OR
               _resource_diag_log_stop LESS_EQUAL _resource_diag_log_start OR
               NOT _resource_diag_log_stop EQUAL _resource_diag_log_stop_last)
                message(FATAL_ERROR "Offscreen old single-line control requires unique complete log-call boundaries")
            endif()
            string(LENGTH "${_resource_diag_log_end}" _resource_diag_log_end_length)
            math(EXPR _resource_diag_log_length "${_resource_diag_log_stop} + ${_resource_diag_log_end_length} - ${_resource_diag_log_start}")
            string(SUBSTRING "${_resource_diag_source}" ${_resource_diag_log_start}
                ${_resource_diag_log_length} _resource_diag_needle)
            set(_resource_diag_replacement [=[    jce_log_write(JCE_LOG_LEVEL_INFO, LOG_TAG, __FILE__, __LINE__,
        "render-resource: phase=%s owner=offscreen creation_serial=%llu "
        "origin_capture_generation=%u view=%u fbo=%u color=%u depth=%u "
        "allocated_width=%u allocated_height=%u color_format=%u samples=1 "
        "requested_extent_known=%u requested_width=%u requested_height=%u "
        "allocation_committed=%u allocated_extent_known=%u "
        "forwarded_rect_known=%u forwarded_rect_x=0 forwarded_rect_y=0 "
        "forwarded_rect_width=%u forwarded_rect_height=%u submission_ordinal=%u "
        "reason=%u destroy_kind=%u destroy_handle=%u cpu_calls_returned=1 gpu_completion_known=0",
        phase, (unsigned long long)id->serial, id->capture_generation,
        (unsigned)bridge->view_id, (unsigned)fbo, (unsigned)id->color.idx,
        (unsigned)id->depth.idx, id->width, id->height, (unsigned)id->color_format,
        destroy_kind == 0u ? 1u : 0u, requested_w, requested_h,
        committed ? 1u : 0u, committed ? 1u : 0u, rect_known ? 1u : 0u,
        (unsigned)rect_w, (unsigned)rect_h, submission,
        reason, destroy_kind, (unsigned)destroy_handle);]=])
        else()
            set(_resource_diag_needle "    if (!bridge->resource_diag) return;")
            set(_resource_diag_replacement "    /* Ordinary missing default-off guard. */")
            set(_resource_diag_expected 2)
        endif()
        string(REPLACE "${_resource_diag_needle}" "" _resource_diag_without "${_resource_diag_source}")
        string(LENGTH "${_resource_diag_source}" _resource_diag_before_length)
        string(LENGTH "${_resource_diag_without}" _resource_diag_after_length)
        string(LENGTH "${_resource_diag_needle}" _resource_diag_needle_length)
        math(EXPR _resource_diag_removed "${_resource_diag_before_length} - ${_resource_diag_after_length}")
        math(EXPR _resource_diag_required "${_resource_diag_needle_length} * ${_resource_diag_expected}")
        if(NOT _resource_diag_removed EQUAL _resource_diag_required)
            message(FATAL_ERROR "Offscreen diagnostic ${_resource_diag_variant} requires exact actual owner wire count")
        endif()
        string(REPLACE "${_resource_diag_needle}" "${_resource_diag_replacement}"
            _resource_diag_selected "${_resource_diag_source}")
        if(_resource_diag_selected STREQUAL _resource_diag_source)
            message(FATAL_ERROR "Offscreen diagnostic ordinary control did not mutate its owner")
        endif()
    endif()
    jce_test_write_if_changed("${_resource_diag_dir}/${_resource_diag_variant}.inc.h" "${_resource_diag_selected}")
    jce_add_unit_test(${_resource_diag_target} KIND ${_resource_diag_kind}
        SOURCES test_jce_offscreen_resource_diag.c LINK jce_core sdl::sdl bgfx::bgfx
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/renderer")
    target_compile_definitions(${_resource_diag_target} PRIVATE JCE_DIST=1
        JCE_TEST_RESOURCE_SESSION_OWNER="${_resource_diag_dir}/session.inc.h"
        JCE_TEST_RESOURCE_ACCESSORS_OWNER="${_resource_diag_dir}/accessors.inc.h"
        JCE_TEST_OFFSCREEN_RESOURCE_OWNER="${_resource_diag_dir}/${_resource_diag_variant}.inc.h")
    if(MSVC)
        target_compile_options(${_resource_diag_target} PRIVATE /Zc:preprocessor)
    endif()
    if(_resource_diag_kind STREQUAL "contract")
        set_tests_properties(${_resource_diag_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
foreach(_resource_diag_var IN ITEMS renderer offscreen renderer_source source dir part begin end
    start last stop stop_last length body variant selected target kind expected needle replacement
    without before_length after_length needle_length removed required log_begin log_end log_start
    log_start_last log_stop log_stop_last log_end_length log_length)
    unset(_resource_diag_${_resource_diag_var})
endforeach()
unset(_resource_diag_var)
