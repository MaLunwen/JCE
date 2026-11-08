# Complete Scene owner and real ECS; only isolated negative owners are changed.
set(_point_write_scene "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_scene.c")
set(_point_write_helper "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_scene_point_light_write_internal.h")
set(_point_write_draw "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_sr_draw.c")
set(_point_write_lighting "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_lighting_system.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_point_write_scene}" "${_point_write_helper}" "${_point_write_draw}" "${_point_write_lighting}")
file(READ "${_point_write_scene}" _point_write_current)
file(READ "${_point_write_draw}" _point_write_draw_source)
file(READ "${_point_write_lighting}" _point_write_lighting_source)

function(_jce_point_write_replace_once variable needle replacement)
    string(FIND "${${variable}}" "${needle}" _first)
    string(FIND "${${variable}}" "${needle}" _last REVERSE)
    if(_first LESS 0 OR NOT _first EQUAL _last)
        message(FATAL_ERROR "PointLight control requires one actual owner wire: ${needle}")
    endif()
    string(REPLACE "${needle}" "${replacement}" _changed "${${variable}}")
    set(${variable} "${_changed}" PARENT_SCOPE)
endfunction()

function(_jce_point_write_extract variable begin end output)
    string(FIND "${${variable}}" "${begin}" _first)
    string(FIND "${${variable}}" "${begin}" _first_last REVERSE)
    string(FIND "${${variable}}" "${end}" _last)
    string(FIND "${${variable}}" "${end}" _last_last REVERSE)
    if(_first LESS 0 OR NOT _first EQUAL _first_last OR
       _last LESS_EQUAL _first OR NOT _last EQUAL _last_last)
        message(FATAL_ERROR "PointLight controls require unique ordered actual payload boundaries")
    endif()
    math(EXPR _length "${_last} - ${_first}")
    string(SUBSTRING "${${variable}}" ${_first} ${_length} _block)
    set(${output} "${_block}" PARENT_SCOPE)
endfunction()

# The standard selected-light payload only; Forward+ is outside this CPU port.
_jce_point_write_extract(_point_write_draw_source
    "            if (jce_scene_has_point_light(scene, e) &&"
    "                    /* Forward+ cluster: every visible point light up to capacity,"
    _point_write_gather)
string(APPEND _point_write_gather "                }\n            }\n")
_jce_point_write_extract(_point_write_lighting_source
    "    memset(env->packed_point, 0, sizeof(env->packed_point));"
    "    /* Spot lights: 4 vec4s per light."
    _point_write_pack)

set(_point_write_old_unconditional_bump "${_point_write_current}")
_jce_point_write_replace_once(_point_write_old_unconditional_bump
    "#define JCE_COMP_CULL_APPLY(s) do { if (_point_light_cull_changed) JCE_COMP_CULL_BUMP(s); } while (0)"
    "#define JCE_COMP_CULL_APPLY(s) JCE_COMP_CULL_BUMP(s)")
set(_point_write_skip_setter "${_point_write_current}")
_jce_point_write_replace_once(_point_write_skip_setter
    "#define JCE_COMP_CULL_PREPARE(s, re, TYPE, v) const bool _point_light_cull_changed = scene_point_light_cull_changed(ecs_get((s)->world, (re), TYPE), (v))"
    "#define JCE_COMP_CULL_PREPARE(s, re, TYPE, v) const bool _point_light_cull_changed = scene_point_light_cull_changed(ecs_get((s)->world, (re), TYPE), (v)); if (!_point_light_cull_changed) return")

set(_point_write_dir "${CMAKE_CURRENT_BINARY_DIR}/point-light-cull-write")
file(MAKE_DIRECTORY "${_point_write_dir}")
jce_test_write_if_changed("${_point_write_dir}/gather.inc.h" "${_point_write_gather}")
jce_test_write_if_changed("${_point_write_dir}/pack.inc.h" "${_point_write_pack}")
foreach(_point_write_variant IN ITEMS current old_unconditional_bump skip_setter)
    set(_point_write_owner "${_point_write_dir}/${_point_write_variant}_scene.inc.h")
    jce_test_write_if_changed("${_point_write_owner}" "${_point_write_${_point_write_variant}}")
    set(_point_write_target test_jce_point_light_cull_write)
    set(_point_write_kind unit)
    if(NOT _point_write_variant STREQUAL "current")
        string(APPEND _point_write_target "_${_point_write_variant}")
        set(_point_write_kind contract)
    endif()
    jce_add_unit_test(${_point_write_target} KIND ${_point_write_kind}
        SOURCES test_jce_point_light_cull_write.c
        LINK jce_core jce_scene jce_resource jce_renderer bgfx::bgfx flecs::flecs_static
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src"
                 "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene")
    target_compile_definitions(${_point_write_target} PRIVATE
        JCE_POINT_LIGHT_SCENE_OWNER="${_point_write_owner}"
        JCE_POINT_LIGHT_GATHER_OWNER="${_point_write_dir}/gather.inc.h"
        JCE_POINT_LIGHT_PACK_OWNER="${_point_write_dir}/pack.inc.h")
    if(MSVC)
        target_compile_options(${_point_write_target} PRIVATE /Zc:preprocessor)
    endif()
    if(NOT _point_write_variant STREQUAL "current")
        set_tests_properties(${_point_write_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
foreach(_point_write_variable IN ITEMS scene helper draw lighting current draw_source lighting_source
    gather pack old_unconditional_bump skip_setter dir variant owner target kind)
    unset(_point_write_${_point_write_variable})
endforeach()
unset(_point_write_variable)
