set(_probe_lifetime_capture "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_scene_probe_capture.c")
set(_probe_lifetime_readback "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_render_readback.c")
set(_probe_lifetime_renderer "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_renderer.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_probe_lifetime_capture}" "${_probe_lifetime_readback}" "${_probe_lifetime_renderer}")
file(READ "${_probe_lifetime_capture}" _probe_lifetime_capture_source)
file(READ "${_probe_lifetime_readback}" _probe_lifetime_readback_source)
file(READ "${_probe_lifetime_renderer}" _probe_lifetime_renderer_source)
set(_probe_lifetime_begin "bool jce_renderer_frame_admission_current_internal(")
set(_probe_lifetime_end "void jce_renderer_destroy_impl(")
string(FIND "${_probe_lifetime_renderer_source}" "${_probe_lifetime_begin}" _probe_lifetime_first)
string(FIND "${_probe_lifetime_renderer_source}" "${_probe_lifetime_begin}" _probe_lifetime_first_last REVERSE)
string(FIND "${_probe_lifetime_renderer_source}" "${_probe_lifetime_end}" _probe_lifetime_last)
string(FIND "${_probe_lifetime_renderer_source}" "${_probe_lifetime_end}" _probe_lifetime_last_last REVERSE)
if(_probe_lifetime_first LESS 0 OR NOT _probe_lifetime_first EQUAL _probe_lifetime_first_last OR
   _probe_lifetime_last LESS_EQUAL _probe_lifetime_first OR NOT _probe_lifetime_last EQUAL _probe_lifetime_last_last)
    message(FATAL_ERROR "Probe lifetime controls require the complete unique central submission owner")
endif()
math(EXPR _probe_lifetime_length "${_probe_lifetime_last} - ${_probe_lifetime_first}")
string(SUBSTRING "${_probe_lifetime_renderer_source}" ${_probe_lifetime_first} ${_probe_lifetime_length}
    _probe_lifetime_submission_source)
function(_jce_probe_lifetime_replace_once variable needle replacement)
    string(FIND "${${variable}}" "${needle}" _first)
    string(FIND "${${variable}}" "${needle}" _last REVERSE)
    if(_first LESS 0 OR NOT _first EQUAL _last)
        message(FATAL_ERROR "Probe lifetime control needs one actual owner wire: ${needle}")
    endif()
    string(REPLACE "${needle}" "${replacement}" _changed "${${variable}}")
    set(${variable} "${_changed}" PARENT_SCOPE)
endfunction()
set(_probe_lifetime_old_pending_free "${_probe_lifetime_readback_source}")
_jce_probe_lifetime_replace_once(_probe_lifetime_old_pending_free
    "        if(queue_pending(queue,current,&ambiguous)) {\n            queue->destroy_requested=true;queue_retire(queue);\n            readback_trace_ownership();\n            return;\n        }"
    "        (void)current; (void)ambiguous; /* historical unsafe immediate destination disposal */")
set(_probe_lifetime_missing_collect "${_probe_lifetime_submission_source}")
_jce_probe_lifetime_replace_once(_probe_lifetime_missing_collect
    "    jce_render_readback_collect(r, s_device.frame_index);"
    "    /* missing actual central ready-retirement wire */")
set(_probe_lifetime_old_format "${_probe_lifetime_capture_source}")
_jce_probe_lifetime_replace_once(_probe_lifetime_old_format
    "const JceRenderFormat source_format = source_hdr ? JCE_RENDER_FORMAT_RGBA16F : JCE_RENDER_FORMAT_RGBA8;"
    "const JceRenderFormat source_format = g_cap.requested_hdr ? JCE_RENDER_FORMAT_RGBA16F : JCE_RENDER_FORMAT_RGBA8;")
set(_probe_lifetime_old_linear "${_probe_lifetime_capture_source}")
_jce_probe_lifetime_replace_once(_probe_lifetime_old_linear
    "jce_scene_renderer_render_linear(sr, scene, cam, JCE_VIEW_EDITOR_SCENE,"
    "jce_scene_renderer_render(sr, scene, cam, JCE_VIEW_EDITOR_SCENE,")
set(_probe_lifetime_dir "${CMAKE_CURRENT_BINARY_DIR}/probe-capture-lifetime")
file(MAKE_DIRECTORY "${_probe_lifetime_dir}")
foreach(_probe_lifetime_variant IN ITEMS current old_pending_free missing_collect old_format old_linear)
    set(_probe_lifetime_target test_jce_probe_capture_lifetime)
    set(_probe_lifetime_kind unit)
    if(NOT _probe_lifetime_variant STREQUAL "current")
        string(APPEND _probe_lifetime_target "_${_probe_lifetime_variant}")
        set(_probe_lifetime_kind contract)
    endif()
    foreach(_probe_lifetime_part IN ITEMS capture readback submission)
        set(_probe_lifetime_content "${_probe_lifetime_${_probe_lifetime_part}_source}")
        if(_probe_lifetime_variant STREQUAL "old_pending_free" AND _probe_lifetime_part STREQUAL "readback")
            set(_probe_lifetime_content "${_probe_lifetime_old_pending_free}")
        elseif(_probe_lifetime_variant STREQUAL "missing_collect" AND _probe_lifetime_part STREQUAL "submission")
            set(_probe_lifetime_content "${_probe_lifetime_missing_collect}")
        elseif(_probe_lifetime_variant STREQUAL "old_format" AND _probe_lifetime_part STREQUAL "capture")
            set(_probe_lifetime_content "${_probe_lifetime_old_format}")
        elseif(_probe_lifetime_variant STREQUAL "old_linear" AND _probe_lifetime_part STREQUAL "capture")
            set(_probe_lifetime_content "${_probe_lifetime_old_linear}")
        endif()
        set(_probe_lifetime_output "${_probe_lifetime_dir}/${_probe_lifetime_variant}_${_probe_lifetime_part}.inc.h")
        unset(_probe_lifetime_previous)
        if(EXISTS "${_probe_lifetime_output}")
            file(READ "${_probe_lifetime_output}" _probe_lifetime_previous)
        endif()
        if(NOT _probe_lifetime_previous STREQUAL _probe_lifetime_content)
            file(WRITE "${_probe_lifetime_output}" "${_probe_lifetime_content}")
        endif()
    endforeach()
    jce_add_unit_test(${_probe_lifetime_target} KIND ${_probe_lifetime_kind}
        SOURCES test_jce_probe_capture_lifetime.c LINK JCE bgfx::bgfx
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/renderer")
    target_compile_definitions(${_probe_lifetime_target} PRIVATE
        JCE_TEST_PROBE_SUBMISSION_OWNER="${_probe_lifetime_dir}/${_probe_lifetime_variant}_submission.inc.h"
        JCE_TEST_PROBE_READBACK_OWNER="${_probe_lifetime_dir}/${_probe_lifetime_variant}_readback.inc.h"
        JCE_TEST_PROBE_CAPTURE_OWNER="${_probe_lifetime_dir}/${_probe_lifetime_variant}_capture.inc.h")
    if(MSVC)
        target_compile_options(${_probe_lifetime_target} PRIVATE /Zc:preprocessor)
    endif()
    if(NOT _probe_lifetime_variant STREQUAL "current")
        set_tests_properties(${_probe_lifetime_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
foreach(_probe_lifetime_variable IN ITEMS capture readback renderer capture_source readback_source renderer_source
    begin end first first_last last last_last length submission_source old_pending_free missing_collect old_format old_linear
    dir variant target kind part content output previous)
    unset(_probe_lifetime_${_probe_lifetime_variable})
endforeach()
unset(_probe_lifetime_variable)

include("${CMAKE_CURRENT_SOURCE_DIR}/scene_linear_controls.cmake")
