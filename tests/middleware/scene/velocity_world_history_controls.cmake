# Exact private world-history owner, host/reset admission and retained-cache
# wires. No scene draw, native window or graphics backend is executed here.
set(_vworld_cull_path "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_sr_cull.c")
set(_vworld_owner_path "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_sr_velocity_world.c")
set(_vworld_renderer_path "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_scene_renderer.c")
set(_vworld_reset_path "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_sr_environment.c")
foreach(_vworld_part IN ITEMS cull owner renderer reset)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_vworld_${_vworld_part}_path}")
    file(READ "${_vworld_${_vworld_part}_path}" _vworld_${_vworld_part})
endforeach()
function(_jce_vworld_between OUTPUT SOURCE BEGIN END)
    string(FIND "${SOURCE}" "${BEGIN}" _begin)
    string(FIND "${SOURCE}" "${BEGIN}" _last_begin REVERSE)
    string(FIND "${SOURCE}" "${END}" _end)
    string(FIND "${SOURCE}" "${END}" _last_end REVERSE)
    if(_begin LESS 0 OR NOT _begin EQUAL _last_begin OR _end LESS_EQUAL _begin OR NOT _end EQUAL _last_end)
        message(FATAL_ERROR "Velocity world fixture requires unique ordered actual owner boundaries: ${OUTPUT}")
    endif()
    math(EXPR _length "${_end} - ${_begin}")
    string(SUBSTRING "${SOURCE}" ${_begin} ${_length} _text)
    set(${OUTPUT} "${_text}" PARENT_SCOPE)
endfunction()
function(_jce_vworld_replace_once VARIABLE NEEDLE REPLACEMENT)
    set(_before "${${VARIABLE}}")
    string(REPLACE "${NEEDLE}" "" _without "${_before}")
    string(LENGTH "${_before}" _before_length)
    string(LENGTH "${_without}" _without_length)
    string(LENGTH "${NEEDLE}" _needle_length)
    math(EXPR _removed "${_before_length} - ${_without_length}")
    if(NOT _removed EQUAL _needle_length)
        message(FATAL_ERROR "Velocity world control requires exactly one actual wire: ${VARIABLE}")
    endif()
    string(REPLACE "${NEEDLE}" "${REPLACEMENT}" _after "${_before}")
    set(${VARIABLE} "${_after}" PARENT_SCOPE)
endfunction()
_jce_vworld_between(_vworld_host "${_vworld_renderer}"
    "void jce_scene_renderer_begin_velocity_frame(JceSceneRenderer *sr)"
    "uint16_t jce_scene_renderer_get_velocity_texture(")
_jce_vworld_between(_vworld_reset_owner "${_vworld_reset}"
    "void jce_scene_renderer_reset_entity_caches(JceSceneRenderer *sr)"
    "    sr_release_prepass_targets(sr, SR_PREPASS_TARGET_RESET);")
string(APPEND _vworld_reset_owner "}\n")
_jce_vworld_between(_vworld_cache "${_vworld_cull}"
    "    const uint64_t pre_lg = sr->frame_list_gen;"
    "    bgfx_set_view_rect(v, 0, 0, sr->ssao_w, sr->ssao_h);")
_jce_vworld_between(_vworld_publication "${_vworld_cull}"
    "    if (s_pre_cache_env) {\n        pcslot->valid     = true;"
    "    }\n}\n\n\n/* ── Distance/importance light selection")
string(APPEND _vworld_publication "    }\n")
# A truncated call-site argument defeats a full-width table key before lookup.
if(_vworld_cull MATCHES "sr_prev_xform_(find|store)\\(sr, \\(uint32_t\\)")
    message(FATAL_ERROR "Velocity world call sites must retain complete entity generations")
endif()
set(_vworld_dir "${CMAKE_CURRENT_BINARY_DIR}/velocity-world-history")
file(MAKE_DIRECTORY "${_vworld_dir}")
foreach(_vworld_variant IN ITEMS current old_store old_view_prune old_entity_generation missing_reset old_cache_stop old_publication)
    set(_vworld_case_owner "${_vworld_owner}")
    set(_vworld_case_reset "${_vworld_reset_owner}")
    set(_vworld_case_cache "${_vworld_cache}")
    set(_vworld_case_publication "${_vworld_publication}")
    if(_vworld_variant STREQUAL "old_store")
        _jce_vworld_replace_once(_vworld_case_owner
            "            e->current_world = *world;\n            e->touched = true;"
            "            e->prev_world = *world; e->prev_valid = true; /* Former per-view overwrite. */\n            e->current_world = *world;\n            e->touched = true;")
    elseif(_vworld_variant STREQUAL "old_view_prune")
        _jce_vworld_replace_once(_vworld_case_owner
            "    if (sr->prev_xform_gen_valid && sr->prev_xform_gen == sr->vel_frame_gen) return;\n" "")
    elseif(_vworld_variant STREQUAL "old_entity_generation")
        _jce_vworld_replace_once(_vworld_case_owner
            "jce_mat4 *sr_prev_xform_find(JceSceneRenderer *sr, JceEntity entity)\n{"
            "jce_mat4 *sr_prev_xform_find(JceSceneRenderer *sr, JceEntity entity)\n{\n    entity = (uint32_t)entity;")
        _jce_vworld_replace_once(_vworld_case_owner
            "void sr_prev_xform_store(JceSceneRenderer *sr, JceEntity entity,\n                                const jce_mat4 *world)\n{"
            "void sr_prev_xform_store(JceSceneRenderer *sr, JceEntity entity,\n                                const jce_mat4 *world)\n{\n    entity = (uint32_t)entity;")
    elseif(_vworld_variant STREQUAL "missing_reset")
        _jce_vworld_replace_once(_vworld_case_reset "    sr_prev_xform_reset(sr);\n" "")
    elseif(_vworld_variant STREQUAL "old_cache_stop")
        _jce_vworld_replace_once(_vworld_case_cache "pcslot->previous_xform == pre_xf &&" "")
    elseif(_vworld_variant STREQUAL "old_publication")
        _jce_vworld_replace_once(_vworld_case_publication
            "        pcslot->previous_xform_valid = pre_cache_identity;\n        if (pre_cache_identity) pcslot->previous_xform = pcslot->xform;"
            "        pcslot->previous_xform_valid = true;\n        pcslot->previous_xform = pcslot->xform; /* Former unrelated slot counter. */")
    endif()
    foreach(_vworld_piece IN ITEMS owner reset cache)
        jce_test_write_if_changed("${_vworld_dir}/${_vworld_variant}-${_vworld_piece}.inc.h"
            "${_vworld_case_${_vworld_piece}}")
    endforeach()
    jce_test_write_if_changed("${_vworld_dir}/host.inc.h" "${_vworld_host}")
    set(_vworld_publication_file "${_vworld_dir}/publication.inc.h")
    if(_vworld_variant STREQUAL "old_publication")
        set(_vworld_publication_file "${_vworld_dir}/old_publication.inc.h")
    endif()
    jce_test_write_if_changed("${_vworld_publication_file}" "${_vworld_case_publication}")
    set(_vworld_target test_jce_velocity_world_history)
    set(_vworld_kind unit)
    if(NOT _vworld_variant STREQUAL "current")
        string(APPEND _vworld_target "_${_vworld_variant}")
        set(_vworld_kind contract)
    endif()
    jce_add_unit_test(${_vworld_target} KIND ${_vworld_kind}
        SOURCES test_jce_velocity_world_history.c
        LINK JCE bgfx::bgfx flecs::flecs_static
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src"
            "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene")
    target_compile_definitions(${_vworld_target} PRIVATE
        JCE_TEST_VELOCITY_WORLD_OWNER="${_vworld_dir}/${_vworld_variant}-owner.inc.h"
        JCE_TEST_VELOCITY_HOST_OWNER="${_vworld_dir}/host.inc.h"
        JCE_TEST_VELOCITY_RESET_OWNER="${_vworld_dir}/${_vworld_variant}-reset.inc.h"
        JCE_TEST_VELOCITY_CACHE_ADMISSION="${_vworld_dir}/${_vworld_variant}-cache.inc.h"
        JCE_TEST_VELOCITY_CACHE_PUBLICATION="${_vworld_publication_file}")
    if(MSVC)
        target_compile_options(${_vworld_target} PRIVATE /Zc:preprocessor)
    endif()
    if(NOT _vworld_variant STREQUAL "current")
        set_tests_properties(${_vworld_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
foreach(_vworld_variable IN ITEMS cull_path owner_path renderer_path reset_path part cull renderer reset owner host
    reset_owner cache publication dir variant case_owner case_reset case_cache case_publication
    publication_file piece target kind)
    unset(_vworld_${_vworld_variable})
endforeach()
unset(_vworld_variable)
