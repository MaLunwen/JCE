# Exact actual ecull/cache owners with real Scene and Space consumers.
set(_shadow_renderer_path "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_scene_renderer.c")
set(_shadow_bounds_path "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_sr_bounds_cache.c")
set(_shadow_ecull_path "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_sr_ecull.c")
set(_shadow_pass_path "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_sr_shadow.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_shadow_renderer_path}" "${_shadow_bounds_path}" "${_shadow_ecull_path}" "${_shadow_pass_path}"
    "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_sr_internal.h")
file(READ "${_shadow_renderer_path}" _shadow_renderer)
file(READ "${_shadow_bounds_path}" _shadow_bounds)
file(READ "${_shadow_ecull_path}" _shadow_ecull)
file(READ "${_shadow_pass_path}" _shadow_pass)
function(_jce_shadow_region SOURCE BEGIN END OUT)
    string(FIND "${SOURCE}" "${BEGIN}" _start)
    string(FIND "${SOURCE}" "${BEGIN}" _last REVERSE)
    string(FIND "${SOURCE}" "${END}" _end)
    string(FIND "${SOURCE}" "${END}" _end_last REVERSE)
    if(_start LESS 0 OR NOT _start EQUAL _last OR _end LESS_EQUAL _start OR NOT _end EQUAL _end_last)
        message(FATAL_ERROR "Shadow fixture requires unique actual owner boundaries: ${BEGIN}")
    endif()
    math(EXPR _length "${_end} - ${_start}")
    string(SUBSTRING "${SOURCE}" ${_start} ${_length} _body)
    set(${OUT} "${_body}" PARENT_SCOPE)
endfunction()
function(_jce_shadow_replace SOURCE NEEDLE REPLACEMENT EXPECTED OUT)
    string(REPLACE "${NEEDLE}" "" _without "${SOURCE}")
    string(LENGTH "${SOURCE}" _before)
    string(LENGTH "${_without}" _after)
    string(LENGTH "${NEEDLE}" _width)
    math(EXPR _removed "${_before} - ${_after}")
    math(EXPR _required "${_width} * ${EXPECTED}")
    if(_width EQUAL 0 OR NOT _removed EQUAL _required)
        message(FATAL_ERROR "Shadow mutation requires exact actual owner count: ${NEEDLE}")
    endif()
    string(REPLACE "${NEEDLE}" "${REPLACEMENT}" _changed "${SOURCE}")
    set(${OUT} "${_changed}" PARENT_SCOPE)
endfunction()
set(_shadow_dir "${CMAKE_CURRENT_BINARY_DIR}/shadow-cache-owner")
file(MAKE_DIRECTORY "${_shadow_dir}")
foreach(_shadow_variant IN ITEMS current old_hit_fold old_static_restore old_noncaster_key
        old_grid_full_key old_metadata_hit old_metadata_seed old_grid_mode old_grid_list old_insert_publish
        old_participation missing_skeletal missing_terrain missing_opaque missing_alias
        missing_cluster_hook missing_cluster_time always_cluster_time missing_cluster_geometry
        disabled_space disabled_incremental missing_dirty_base)
    set(_shadow_selected "${_shadow_renderer}")
    set(_shadow_selected_pass "${_shadow_pass}")
    if(_shadow_variant STREQUAL "missing_dirty_base")
        _jce_shadow_replace("${_shadow_selected}"
            "    if (!s_incr_on || !base_matches || ovf || rn == 0) return false;"
            "    (void)base_matches;\n    if (!s_incr_on || ovf || rn == 0) return false;"
            1 _shadow_selected)
    elseif(_shadow_variant STREQUAL "missing_cluster_hook")
        _jce_shadow_replace("${_shadow_selected}"
            "            const uint64_t cluster_key = sr_prepare_foliage_cluster_shadows(sr, scene, &list);\n            sr->shadow_caster_key = sr_shadow_fold_part(sr->shadow_caster_key, cluster_key);\n            sr->shadow_static_caster_key =\n                sr_shadow_fold_part(sr->shadow_static_caster_key, cluster_key);\n"
            "" 1 _shadow_selected)
    elseif(_shadow_variant STREQUAL "missing_cluster_time")
        _jce_shadow_replace("${_shadow_selected_pass}"
            "            key = sr_shadow_fold_part(key, time_bits);"
            "            /* Old: animated leaf time absent from cached depth identity. */"
            1 _shadow_selected_pass)
    elseif(_shadow_variant STREQUAL "always_cluster_time")
        _jce_shadow_replace("${_shadow_selected_pass}" "        if (leaf_wind) {"
            "        if (true) {" 1 _shadow_selected_pass)
    elseif(_shadow_variant STREQUAL "missing_cluster_geometry")
        _jce_shadow_replace("${_shadow_selected_pass}"
            "        key = sr_shadow_fold_part(key, sr_foliage_hash(fc, &origin));"
            "        /* Old: current placement geometry absent from cached depth identity. */"
            1 _shadow_selected_pass)
    elseif(_shadow_variant STREQUAL "old_participation")
        _jce_shadow_replace("${_shadow_selected_pass}"
            "    return jce_component_count() != count;"
            "    return true; /* Old: every non-mesh row casts. */"
            1 _shadow_selected_pass)
    elseif(_shadow_variant STREQUAL "missing_skeletal")
        _jce_shadow_replace("${_shadow_selected_pass}"
            "    if (jce_scene_has_skeletal_animator(scene, e) ||"
            "    if (false ||" 1 _shadow_selected_pass)
    elseif(_shadow_variant STREQUAL "missing_terrain")
        _jce_shadow_replace("${_shadow_selected_pass}"
            "        jce_scene_has_terrain(scene, e)) return true;"
            "        false) return true;" 1 _shadow_selected_pass)
    elseif(_shadow_variant STREQUAL "missing_opaque")
        _jce_shadow_replace("${_shadow_selected_pass}"
            "            jce_scene_has_comp(scene, e, id)) return true;"
            "            false) return true;" 1 _shadow_selected_pass)
    elseif(_shadow_variant STREQUAL "missing_alias")
        _jce_shadow_replace("${_shadow_selected_pass}"
            "(!flag || jce_component_from_legacy_flag(flag) != id)"
            "(!flag)" 1 _shadow_selected_pass)
    elseif(_shadow_variant STREQUAL "old_hit_fold")
        _jce_shadow_replace("${_shadow_selected}"
            "                sr->shadow_static_caster_key =\n                    sr_shadow_fold_part(sr->shadow_static_caster_key,\n                                        sr->ecull_key_parts[ci]);"
            "                /* Old static-hit partial omitted. */" 1 _shadow_selected)
        _jce_shadow_replace("${_shadow_selected}"
            "                            sr->shadow_static_caster_key =\n                                sr_shadow_fold_caster(sr->shadow_static_caster_key,\n                                                      (uint64_t)e, e_gen);"
            "                            /* Old serial static hit omitted. */" 1 _shadow_selected)
    elseif(_shadow_variant STREQUAL "old_static_restore")
        _jce_shadow_replace("${_shadow_selected}"
            "        if (old_caster && !old->is_dyn_caster) return false;"
            "        /* Old static repair admitted. */" 1 _shadow_selected)
        _jce_shadow_replace("${_shadow_selected}"
            "        if (now->casts_shadow && !now->is_dyn_caster) return false;"
            "        /* Old changed static key restored. */" 1 _shadow_selected)
    elseif(_shadow_variant STREQUAL "old_noncaster_key")
        _jce_shadow_replace("${_shadow_selected}" "sr->shadow_caster_key = caster_dirty"
            "sr->shadow_caster_key = true" 1 _shadow_selected)
    elseif(_shadow_variant STREQUAL "old_grid_full_key")
        _jce_shadow_replace("${_shadow_selected_pass}"
            "const uint64_t caster_key = pc_on ? sr->shadow_static_caster_key\n                                      : sr->shadow_caster_key;"
            "const uint64_t caster_key = sr->shadow_caster_key;" 1 _shadow_selected_pass)
    elseif(_shadow_variant STREQUAL "old_metadata_hit")
        _jce_shadow_replace("${_shadow_selected}"
            "SrWorldCacheEntry *hit = sr->ecull_metadata_trusted\n            ? sr_wcache_find(sr, e, c->struct_epoch, e_gen) : NULL;"
            "SrWorldCacheEntry *hit = sr_wcache_find(sr, e, c->struct_epoch, e_gen);"
            1 _shadow_selected)
        _jce_shadow_replace("${_shadow_selected}" "if (wcache_on && sr->ecull_metadata_trusted) {"
            "if (wcache_on) {" 1 _shadow_selected)
    elseif(_shadow_variant STREQUAL "old_metadata_seed")
        _jce_shadow_replace("${_shadow_selected}"
            "const uint64_t caster_seed = sr_shadow_fold_part(1469598103934665603ull,\n        sr_ecull_mix64(sr->ecull_frz_enable));"
            "const uint64_t caster_seed = 1469598103934665603ull;" 1 _shadow_selected)
    elseif(_shadow_variant STREQUAL "old_grid_mode")
        _jce_shadow_replace("${_shadow_selected_pass}"
            "        && sr->shadow_space_static_only == pc_on\n" "" 1 _shadow_selected_pass)
    elseif(_shadow_variant STREQUAL "old_grid_list")
        _jce_shadow_replace("${_shadow_selected_pass}"
            "        && sr->shadow_space_list_gen     == sr->frame_list_gen\n" "" 1 _shadow_selected_pass)
    elseif(_shadow_variant STREQUAL "old_insert_publish")
        _jce_shadow_replace("${_shadow_selected_pass}"
            "        if (!jce_space_insert(sr->shadow_space, b, (uint32_t)i)) return false;"
            "        (void)jce_space_insert(sr->shadow_space, b, (uint32_t)i);"
            1 _shadow_selected_pass)
    endif()
    _jce_shadow_region("${_shadow_bounds}" "SrWorldCacheEntry *sr_wcache_find("
        "/* ── Persistent focus-cull world-AABB cache" _shadow_world)
    _jce_shadow_region("${_shadow_bounds}" "struct SrFocusAabbEntry *sr_focus_aabb_find("
        "void sr_bounds_cache_reset(" _shadow_focus)
    _jce_shadow_region("${_shadow_selected}" "#define SR_ECULL_PAR_CHUNK"
        "/* ── Does any water body" _shadow_hits)
    _jce_shadow_region("${_shadow_selected}" "    bool ecull_frozen = false;"
        "    {\n        /* Slice 2 (DOTS floor): latch" _shadow_admission)
    _jce_shadow_region("${_shadow_selected}" "        if (ecull_par) {\n            SrEcullBuildCtx bctx;"
        "    /* Focus-AABB persistence (EVERY" _shadow_build)
    string(PREPEND _shadow_build "    {\n")
    _jce_shadow_region("${_shadow_selected}" "    /* Memoize the freshly built caster key"
        "    /* In-asset auto-LOD" _shadow_commit)
    _jce_shadow_region("${_shadow_selected}"
        "            const uint64_t foliage_key = sr_prepare_scatter_shadows"
        "            if (!grass_complete) {" _shadow_content)
    _jce_shadow_region("${_shadow_selected_pass}" "bool sr_entity_casts_shadow("
        "/* ── Shadow VP / CSM helpers" _shadow_participation)
    _jce_shadow_region("${_shadow_selected_pass}" "#define SR_SHADOW_DYN_MAX"
        "/* Dual shadow maps (JCE_SHADOW_DUAL" _shadow_mode)
    _jce_shadow_region("${_shadow_selected_pass}" "static bool sr_build_shadow_space("
        "/* Conservative WORLD AABB" _shadow_grid)
    _jce_shadow_region("${_shadow_selected_pass}" "    static int s_shadowcache_disabled = -1;"
        "    /* Per-frame counters for the cascade gather" _shadow_cascade)
    foreach(_shadow_part IN ITEMS world focus hits admission build commit content participation mode grid cascade ecull)
        jce_test_write_if_changed("${_shadow_dir}/${_shadow_variant}_${_shadow_part}.inc.h"
            "${_shadow_${_shadow_part}}")
    endforeach()
    set(_shadow_target test_jce_shadow_cache_owner)
    set(_shadow_kind unit)
    if(NOT _shadow_variant STREQUAL "current")
        string(APPEND _shadow_target "_${_shadow_variant}")
        set(_shadow_kind contract)
    endif()
    jce_add_unit_test(${_shadow_target} KIND ${_shadow_kind}
        SOURCES test_jce_shadow_cache_owner.c LINK JCE bgfx::bgfx flecs::flecs_static
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene")
    foreach(_shadow_part IN ITEMS world focus hits admission build commit content participation mode grid cascade ecull)
        string(TOUPPER "${_shadow_part}" _shadow_define)
        target_compile_definitions(${_shadow_target} PRIVATE
            "JCE_TEST_SHADOW_${_shadow_define}_OWNER=\"${_shadow_dir}/${_shadow_variant}_${_shadow_part}.inc.h\"")
    endforeach()
    if(_shadow_variant STREQUAL "disabled_space")
        target_compile_definitions(${_shadow_target} PRIVATE JCE_TEST_SHADOW_SPACE_DISABLED=1)
    elseif(_shadow_variant STREQUAL "disabled_incremental")
        target_compile_definitions(${_shadow_target} PRIVATE JCE_TEST_SHADOW_INCREMENTAL_DISABLED=1)
    elseif(NOT _shadow_variant STREQUAL "current")
        set_tests_properties(${_shadow_target} PROPERTIES WILL_FAIL TRUE)
    endif()
    if(MSVC)
        target_compile_options(${_shadow_target} PRIVATE /Zc:preprocessor)
    endif()
endforeach()
foreach(_shadow_variable IN ITEMS renderer_path bounds_path ecull_path pass_path renderer bounds ecull pass dir variant
    selected selected_pass world focus hits admission build commit content participation mode grid cascade part target kind define)
    unset(_shadow_${_shadow_variable})
endforeach()
unset(_shadow_variable)
