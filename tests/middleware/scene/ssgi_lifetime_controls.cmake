# Complete SSGI retirement owner and exact existing scene call-site wires.
set(_ssgi_root "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene")
foreach(_ssgi_part IN ITEMS owner renderer reset)
    if(_ssgi_part STREQUAL "owner")
        set(_ssgi_path "${_ssgi_root}/jce_sr_ssgi_lifetime.c")
    elseif(_ssgi_part STREQUAL "renderer")
        set(_ssgi_path "${_ssgi_root}/jce_scene_renderer.c")
    else()
        set(_ssgi_path "${_ssgi_root}/jce_sr_environment.c")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_ssgi_path}")
    file(READ "${_ssgi_path}" _ssgi_${_ssgi_part})
endforeach()
function(_jce_ssgi_between OUTPUT SOURCE BEGIN END)
    string(FIND "${SOURCE}" "${BEGIN}" _begin)
    string(FIND "${SOURCE}" "${BEGIN}" _last_begin REVERSE)
    string(FIND "${SOURCE}" "${END}" _end)
    string(FIND "${SOURCE}" "${END}" _last_end REVERSE)
    if(_begin LESS 0 OR NOT _begin EQUAL _last_begin OR _end LESS_EQUAL _begin OR NOT _end EQUAL _last_end)
        message(FATAL_ERROR "SSGI fixture requires unique ordered actual boundaries: ${OUTPUT}")
    endif()
    math(EXPR _length "${_end} - ${_begin}")
    string(SUBSTRING "${SOURCE}" ${_begin} ${_length} _text)
    set(${OUTPUT} "${_text}" PARENT_SCOPE)
endfunction()
function(_jce_ssgi_replace_once VARIABLE NEEDLE REPLACEMENT)
    set(_before "${${VARIABLE}}")
    string(REPLACE "${NEEDLE}" "" _without "${_before}")
    string(LENGTH "${_before}" _before_length)
    string(LENGTH "${_without}" _without_length)
    string(LENGTH "${NEEDLE}" _needle_length)
    math(EXPR _removed "${_before_length} - ${_without_length}")
    if(NOT _removed EQUAL _needle_length)
        message(FATAL_ERROR "SSGI fixture requires one complete actual wire: ${VARIABLE}")
    endif()
    string(REPLACE "${NEEDLE}" "${REPLACEMENT}" _after "${_before}")
    set(${VARIABLE} "${_after}" PARENT_SCOPE)
endfunction()
_jce_ssgi_between(_ssgi_prepare "${_ssgi_renderer}"
    "                if (want_ssgi) {"
    "                    if (sr->ssgi) {")
string(APPEND _ssgi_prepare "                }\n")
_jce_ssgi_between(_ssgi_frame_reset "${_ssgi_renderer}"
    "    sr->ssgi_active_frame = false;"
    "    sr->velocity_valid_frame = false;")
_jce_ssgi_between(_ssgi_retire_wire "${_ssgi_renderer}"
    "        sr_ssgi_retire_unused(sr, reduced, want_ssgi);"
    "        if (sr->ssao && !want_ssao) {")
_jce_ssgi_between(_ssgi_destroy_wire "${_ssgi_renderer}"
    "    sr_ssgi_release(sr);"
    "    if (sr->planar)    jce_planar_reflection_destroy(sr->planar);")
_jce_ssgi_between(_ssgi_reset_wire "${_ssgi_reset}"
    "    sr_ssgi_reset(sr);"
    "    sr->resource_prepared_valid = false;")
set(_ssgi_dir "${CMAKE_CURRENT_BINARY_DIR}/ssgi-lifetime")
file(MAKE_DIRECTORY "${_ssgi_dir}")
foreach(_ssgi_piece IN ITEMS prepare frame_reset retire_wire destroy_wire reset_wire)
    jce_test_write_if_changed("${_ssgi_dir}/${_ssgi_piece}.inc.h" "${_ssgi_${_ssgi_piece}}")
endforeach()
foreach(_ssgi_variant IN ITEMS current old_immediate per_render)
    set(_ssgi_case_owner "${_ssgi_owner}")
    if(_ssgi_variant STREQUAL "old_immediate")
        _jce_ssgi_replace_once(_ssgi_case_owner
            "    if (!sr || reduced) return;"
            "    if (!sr) return;\n    if (sr->ssgi && !wanted) { sr_ssgi_release(sr); return; }\n    if (reduced) return;")
    elseif(_ssgi_variant STREQUAL "per_render")
        _jce_ssgi_replace_once(_ssgi_case_owner
            "    const uint32_t frame = jce_renderer_get_frame_index(sr->renderer);"
            "    const uint32_t frame = idle->valid ? idle->frame + 1u : 0u;")
    endif()
    jce_test_write_if_changed("${_ssgi_dir}/${_ssgi_variant}-owner.inc.h" "${_ssgi_case_owner}")
    set(_ssgi_target test_jce_ssgi_lifetime)
    set(_ssgi_kind unit)
    if(NOT _ssgi_variant STREQUAL "current")
        string(APPEND _ssgi_target "_${_ssgi_variant}")
        set(_ssgi_kind contract)
    endif()
    jce_add_unit_test(${_ssgi_target} KIND ${_ssgi_kind}
        SOURCES test_jce_ssgi_lifetime.c
        LINK jce_core bgfx::bgfx flecs::flecs_static
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src"
            "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene")
    target_compile_definitions(${_ssgi_target} PRIVATE
        JCE_TEST_SSGI_LIFETIME_OWNER="${_ssgi_dir}/${_ssgi_variant}-owner.inc.h"
        JCE_TEST_SSGI_PREPARE="${_ssgi_dir}/prepare.inc.h"
        JCE_TEST_SSGI_FRAME_RESET="${_ssgi_dir}/frame_reset.inc.h"
        JCE_TEST_SSGI_RETIRE_WIRE="${_ssgi_dir}/retire_wire.inc.h"
        JCE_TEST_SSGI_DESTROY_WIRE="${_ssgi_dir}/destroy_wire.inc.h"
        JCE_TEST_SSGI_RESET_WIRE="${_ssgi_dir}/reset_wire.inc.h")
    if(MSVC)
        target_compile_options(${_ssgi_target} PRIVATE /Zc:preprocessor)
    endif()
    if(NOT _ssgi_variant STREQUAL "current")
        set_tests_properties(${_ssgi_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
foreach(_ssgi_variable IN ITEMS root part path owner renderer reset prepare frame_reset
        retire_wire destroy_wire reset_wire dir piece variant case_owner target kind)
    unset(_ssgi_${_ssgi_variable})
endforeach()
unset(_ssgi_variable)
