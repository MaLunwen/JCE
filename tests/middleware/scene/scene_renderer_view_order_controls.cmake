# Complete production view-order owner; only isolated negative copies restore
# the former depth order or unchecked finite-view admission.
set(_view_order_path "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_scene_renderer_view_order.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_view_order_path}")
file(READ "${_view_order_path}" _view_order_actual)

function(_jce_view_order_replace_once variable needle replacement)
    string(FIND "${${variable}}" "${needle}" _first)
    string(FIND "${${variable}}" "${needle}" _last REVERSE)
    if(_first LESS 0 OR NOT _first EQUAL _last)
        message(FATAL_ERROR "View-order control requires one actual owner wire: ${needle}")
    endif()
    string(REPLACE "${needle}" "${replacement}" _changed "${${variable}}")
    set(${variable} "${_changed}" PARENT_SCOPE)
endfunction()

set(_view_order_old_order "${_view_order_actual}")
_jce_view_order_replace_once(_view_order_old_order
    "    uint16_t max_view = (uint16_t)(view_id_base + 1u);"
    "    uint16_t max_view = view_id_base;")
_jce_view_order_replace_once(_view_order_old_order
    "    if (!order_push(out, (uint16_t)(view_id_base + 1u)))\n        return false;\n"
    "")

set(_view_order_old_boundary "${_view_order_actual}")
_jce_view_order_replace_once(_view_order_old_boundary
    "    if (view_id >= JCE_SCENE_RENDERER_VIEW_ID_LIMIT)\n        return false;\n"
    "")
_jce_view_order_replace_once(_view_order_old_boundary
    "    if (view_id_base >= JCE_SCENE_RENDERER_VIEW_ID_LIMIT - 1u)\n        return false;\n"
    "")
_jce_view_order_replace_once(_view_order_old_boundary
    "    if (max_view >= JCE_SCENE_RENDERER_VIEW_ID_LIMIT ||\n        range_count > JCE_SCENE_RENDERER_VIEW_ORDER_MAX)"
    "    if (range_count > JCE_SCENE_RENDERER_VIEW_ORDER_MAX)")

set(_view_order_old_cloud_order "${_view_order_actual}")
_jce_view_order_replace_once(_view_order_old_cloud_order
    "    if (include_cloud_temporal &&\n        !order_push(out, (uint16_t)(view_id_base + JCE_VIEW_SR_CLOUD_OPAQUE_OFFSET)))\n        return false;\n"
    "")

set(_view_order_dir "${CMAKE_CURRENT_BINARY_DIR}/scene-renderer-view-order")
file(MAKE_DIRECTORY "${_view_order_dir}")
foreach(_view_order_variant IN ITEMS old_order old_boundary old_cloud_order)
    set(_view_order_generated "${_view_order_dir}/${_view_order_variant}.c")
    jce_test_write_if_changed("${_view_order_generated}" "${_view_order_${_view_order_variant}}")
    set(_view_order_target "test_jce_scene_renderer_view_order_${_view_order_variant}")
    jce_add_unit_test(${_view_order_target} KIND contract
        SOURCES test_jce_scene_renderer_view_order.c "${_view_order_generated}"
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene")
    set_tests_properties(${_view_order_target} PROPERTIES WILL_FAIL TRUE)
endforeach()
foreach(_view_order_variable IN ITEMS path actual old_order old_boundary old_cloud_order dir variant generated target)
    unset(_view_order_${_view_order_variable})
endforeach()
unset(_view_order_variable)
