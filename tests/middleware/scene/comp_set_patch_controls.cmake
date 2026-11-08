# Real private patch owner, real cJSON allocator and real typed scene storage.
# Isolated complete-owner copies restore cloning, failed admission or generic
# line construction; helper copies break one real line admission/storage/set wire.
set(_comp_patch_owner "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_scene_json_patch_internal.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_comp_patch_owner}")
file(READ "${_comp_patch_owner}" _comp_patch_actual)

function(_jce_comp_patch_replace_once variable needle replacement)
    string(FIND "${${variable}}" "${needle}" _first)
    string(FIND "${${variable}}" "${needle}" _last REVERSE)
    if(_first LESS 0 OR NOT _first EQUAL _last)
        message(FATAL_ERROR "Component patch control requires one actual owner wire: ${needle}")
    endif()
    string(REPLACE "${needle}" "${replacement}" _changed "${${variable}}")
    set(${variable} "${_changed}" PARENT_SCOPE)
endfunction()

set(_comp_patch_extra_clone "${_comp_patch_actual}")
_jce_comp_patch_replace_once(_comp_patch_extra_clone
    "    jce_json_detach(array, first);"
    "    cJSON *extra_copy = cJSON_Duplicate(first, 1);\n    if (!extra_copy) { cJSON_Delete(array); cJSON_Delete(patch); return false; }\n    first = extra_copy;")

set(_comp_patch_failopen "${_comp_patch_actual}")
_jce_comp_patch_replace_once(_comp_patch_failopen
    "    if (!array) { cJSON_Delete(patch); return false; }"
    "    if (!array) { descriptor->parse(scene, entity, patch); cJSON_Delete(patch); return true; }")
_jce_comp_patch_replace_once(_comp_patch_failopen
    "    if (!cJSON_IsObject(first)) {\n        cJSON_Delete(array);\n        cJSON_Delete(patch);\n        return false;\n    }"
    "    if (!cJSON_IsObject(first)) {\n        cJSON_Delete(array);\n        descriptor->parse(scene, entity, patch);\n        cJSON_Delete(patch);\n        return true;\n    }")
_jce_comp_patch_replace_once(_comp_patch_failopen
    "            if (!rs_json_merge(current, item)) return false;"
    "            (void)rs_json_merge(current, item);")
_jce_comp_patch_replace_once(_comp_patch_failopen
    "            if (!copy) return false;"
    "            if (!copy) continue;")
# Keep cleanup even in the negative: ordinary acceptance/state assertions,
# not a deliberate leak/crash, must reject the former ignored failure.
_jce_comp_patch_replace_once(_comp_patch_failopen
    "            if (!attached) {\n                cJSON_Delete(copy);\n                return false;\n            }"
    "            if (!attached) cJSON_Delete(copy);")

set(_comp_patch_scalar_allocation "${_comp_patch_actual}")
_jce_comp_patch_replace_once(_comp_patch_scalar_allocation
    "        if (rs_json_same_scalar(current, item)) continue;"
    "        /* Old path duplicates even identical primitive leaves. */")

set(_comp_patch_line_generic "${_comp_patch_actual}")
_jce_comp_patch_replace_once(_comp_patch_line_generic
    "        scene_line_try_color_patch(scene, entity, patch_fields)) {"
    "        false && scene_line_try_color_patch(scene, entity, patch_fields)) {")

target_include_directories(test_jce_comp_set_partial PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src"
    "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene")
target_link_libraries(test_jce_comp_set_partial PRIVATE cjson::cjson flecs::flecs_static)

set(_comp_patch_dir "${CMAKE_CURRENT_BINARY_DIR}/comp-set-patch")
file(MAKE_DIRECTORY "${_comp_patch_dir}")
foreach(_comp_patch_variant IN ITEMS extra_clone failopen scalar_allocation line_generic)
    set(_comp_patch_path "${_comp_patch_dir}/${_comp_patch_variant}.inc.h")
    unset(_comp_patch_previous)
    if(EXISTS "${_comp_patch_path}")
        file(READ "${_comp_patch_path}" _comp_patch_previous)
    endif()
    if(NOT _comp_patch_previous STREQUAL _comp_patch_${_comp_patch_variant})
        file(WRITE "${_comp_patch_path}" "${_comp_patch_${_comp_patch_variant}}")
    endif()
    set(_comp_patch_target "test_jce_comp_set_partial_old_${_comp_patch_variant}")
    jce_add_unit_test(${_comp_patch_target} KIND contract
        SOURCES test_jce_comp_set_partial.c
        LINK jce_core jce_scene jce_resource cjson::cjson flecs::flecs_static
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src"
                 "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene")
    target_compile_definitions(${_comp_patch_target} PRIVATE
        JCE_COMPONENT_PATCH_OWNER="${_comp_patch_path}")
    set_tests_properties(${_comp_patch_target} PROPERTIES WILL_FAIL TRUE)
endforeach()
# Include the complete actual helper before the complete patch owner, exactly
# as the positive fixture does. The private include guard prevents a second copy.
set(_comp_patch_line_owner "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_scene_line_colors_internal.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_comp_patch_line_owner}")
file(READ "${_comp_patch_line_owner}" _comp_patch_line_actual)
set(_comp_patch_line_storage "${_comp_patch_line_actual}")
_jce_comp_patch_replace_once(_comp_patch_line_storage
    "    const size_t material_size = (size_t)(end - current->material_path) + 1;\n    memset(value.material_path + material_size, 0, sizeof value.material_path - material_size);\n    memset(value.positions + value.position_count, 0,\n           (JCE_LINE_MAX_POINTS - (size_t)value.position_count) * sizeof value.positions[0]);"
    "    /* Incorrectly retain storage discarded by the ordinary parser. */")
set(_comp_patch_line_admission "${_comp_patch_line_actual}")
_jce_comp_patch_replace_once(_comp_patch_line_admission
    "        if (i == 8 || (seen & (1u << i)) || !jce_json_is_number(item) ||"
    "        if (!jce_json_is_number(item) ||")
set(_comp_patch_line_setter "${_comp_patch_line_actual}")
_jce_comp_patch_replace_once(_comp_patch_line_setter
    "    jce_scene_set_line_renderer(scene, entity, &value);"
    "    /* Incorrectly omit the canonical typed write/OnSet. */")
set(_comp_patch_line_missing_read "${_comp_patch_line_actual}")
_jce_comp_patch_replace_once(_comp_patch_line_missing_read
    "        line->color_end[i] = (float)jce_json_get_number(json, scene_line_color_keys[i + 4],\n            partial ? line->color_end[i] : 1.0);"
    "        (void)jce_json_get_number(json, scene_line_color_keys[i + 4], 1.0);")
foreach(_comp_patch_variant IN ITEMS line_storage line_admission line_setter line_missing_read)
    set(_comp_patch_path "${_comp_patch_dir}/${_comp_patch_variant}.inc.h")
    unset(_comp_patch_previous)
    if(EXISTS "${_comp_patch_path}")
        file(READ "${_comp_patch_path}" _comp_patch_previous)
    endif()
    if(NOT _comp_patch_previous STREQUAL _comp_patch_${_comp_patch_variant})
        file(WRITE "${_comp_patch_path}" "${_comp_patch_${_comp_patch_variant}}")
    endif()
    set(_comp_patch_target "test_jce_comp_set_partial_old_${_comp_patch_variant}")
    jce_add_unit_test(${_comp_patch_target} KIND contract
        SOURCES test_jce_comp_set_partial.c
        LINK jce_core jce_scene jce_resource cjson::cjson flecs::flecs_static
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src"
                 "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene")
    target_compile_definitions(${_comp_patch_target} PRIVATE
        JCE_LINE_COLORS_OWNER="${_comp_patch_path}")
    set_tests_properties(${_comp_patch_target} PROPERTIES WILL_FAIL TRUE)
endforeach()
foreach(_comp_patch_variable IN ITEMS owner actual extra_clone failopen scalar_allocation line_generic
    line_owner line_actual line_storage line_admission line_setter line_missing_read dir variant
    path previous target)
    unset(_comp_patch_${_comp_patch_variable})
endforeach()
unset(_comp_patch_variable)
