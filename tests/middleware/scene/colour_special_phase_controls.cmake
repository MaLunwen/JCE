# Complete private stage owner + exact actual dispatch/flush owner regions.
set(_colour_draw_path "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_sr_draw.c")
set(_colour_header_path "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_sr_colour_special_internal.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_colour_draw_path}" "${_colour_header_path}"
    "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_sr_types.h")
file(READ "${_colour_draw_path}" _colour_draw)
file(READ "${_colour_header_path}" _colour_header)
function(_jce_colour_region SOURCE BEGIN END OUT)
    string(FIND "${SOURCE}" "${BEGIN}" _start)
    string(FIND "${SOURCE}" "${BEGIN}" _last REVERSE)
    string(FIND "${SOURCE}" "${END}" _end)
    string(FIND "${SOURCE}" "${END}" _end_last REVERSE)
    if(_start LESS 0 OR NOT _start EQUAL _last OR _end LESS_EQUAL _start OR NOT _end EQUAL _end_last)
        message(FATAL_ERROR "Colour phase fixture requires unique complete owner boundaries: ${BEGIN}")
    endif()
    math(EXPR _length "${_end} - ${_start}")
    string(SUBSTRING "${SOURCE}" ${_start} ${_length} _body)
    set(${OUT} "${_body}" PARENT_SCOPE)
endfunction()
function(_jce_colour_replace SOURCE NEEDLE REPLACEMENT EXPECTED OUT)
    string(REPLACE "${NEEDLE}" "" _without "${SOURCE}")
    string(LENGTH "${SOURCE}" _before)
    string(LENGTH "${_without}" _after)
    string(LENGTH "${NEEDLE}" _width)
    math(EXPR _removed "${_before} - ${_after}")
    math(EXPR _required "${_width} * ${EXPECTED}")
    if(_width EQUAL 0 OR NOT _removed EQUAL _required)
        message(FATAL_ERROR "Colour phase mutation requires exact actual owner count: ${NEEDLE}")
    endif()
    string(REPLACE "${NEEDLE}" "${REPLACEMENT}" _changed "${SOURCE}")
    set(${OUT} "${_changed}" PARENT_SCOPE)
endfunction()
_jce_colour_region("${_colour_draw}" "JCE_INLINE bool sr_layer_masked_out("
    "#include \"jce_sr_colour_special_internal.h\"" _colour_layer)
string(FIND "${_colour_draw}" "void sr_draw_entities(" _colour_function_start)
if(_colour_function_start LESS 0)
    message(FATAL_ERROR "Colour fixture requires actual entity draw owner")
endif()
string(SUBSTRING "${_colour_draw}" ${_colour_function_start} -1 _colour_function)
set(_colour_opaque_wire "    sr_colour_special_submit_opaque(sr, scene, camera, list, view_id, cfg,\n        &colour_special_pass, grass_gate_pass_ptr);\n")
set(_colour_transparent_wire "    sr_colour_special_submit_transparent(sr, scene, camera, list, transparent_view,\n        &colour_special_pass);\n")
set(_colour_dir "${CMAKE_CURRENT_BINARY_DIR}/colour-special-phase")
file(MAKE_DIRECTORY "${_colour_dir}")
foreach(_colour_variant IN ITEMS current old_early_opaque old_early_transparent missing_consumption missing_layer missing_enabled missing_empty_gate)
    set(_colour_selected "${_colour_function}")
    set(_colour_selected_header "${_colour_header}")
    if(_colour_variant STREQUAL "old_early_opaque")
        _jce_colour_replace("${_colour_selected}" "${_colour_opaque_wire}" "" 1 _colour_selected)
        _jce_colour_replace("${_colour_selected}" "    sr_prim_inst_flush(sr, view_id);"
            "${_colour_opaque_wire}    sr_prim_inst_flush(sr, view_id);" 1 _colour_selected)
    elseif(_colour_variant STREQUAL "old_early_transparent")
        _jce_colour_replace("${_colour_selected}" "${_colour_transparent_wire}" "" 1 _colour_selected)
        _jce_colour_replace("${_colour_selected}" "${_colour_opaque_wire}"
            "${_colour_transparent_wire}${_colour_opaque_wire}" 1 _colour_selected)
    elseif(_colour_variant STREQUAL "missing_consumption")
        foreach(_colour_mask IN ITEMS OPAQUE TRANSPARENT)
            _jce_colour_replace("${_colour_selected_header}"
                "        sr->ecull[i].colour_special = (uint8_t)(flags & ~SR_COLOUR_${_colour_mask});"
                "        /* Old per-view dispatch marks retained. */" 1 _colour_selected_header)
        endforeach()
    elseif(_colour_variant STREQUAL "missing_layer")
        _jce_colour_replace("${_colour_selected}"
            "        if (sr_layer_masked_out(scene, e, sr->frame_culling_mask)) continue;"
            "        /* Ordinary missing layer gate. */" 1 _colour_selected)
    elseif(_colour_variant STREQUAL "missing_enabled")
        _jce_colour_replace("${_colour_selected}"
            "        if (kc ? (ec->render_kind == SR_RK_DISABLED) : !entity_enabled(scene, e)) {"
            "        if (false) { /* Ordinary missing enabled gate. */" 1 _colour_selected)
    elseif(_colour_variant STREQUAL "missing_empty_gate")
        foreach(_colour_gate IN ITEMS opaque transparent)
            _jce_colour_replace("${_colour_selected_header}" "    if (!pass->${_colour_gate}) return;"
                "    /* Ordinary missing no-special guard. */" 1 _colour_selected_header)
        endforeach()
    endif()
    _jce_colour_region("${_colour_selected}" "    SrGrassGatePass grass_gate_pass;"
        "    /* Water phase:" _colour_begin)
    _jce_colour_region("${_colour_selected}" "    const uint8_t *fastb ="
        "        if (!all_visible && !visible[i]) {" _colour_dispatch)
    _jce_colour_region("${_colour_selected}" "#define JCE_FLUSH_MARK(name)"
        "    /* Flush sprite batch whenever" _colour_flush)
    set(_colour_special "${_colour_selected_header}")
    foreach(_colour_part IN ITEMS layer special begin dispatch flush)
        jce_test_write_if_changed("${_colour_dir}/${_colour_variant}_${_colour_part}.inc.h" "${_colour_${_colour_part}}")
    endforeach()
    set(_colour_target test_jce_colour_special_phase)
    set(_colour_kind unit)
    if(NOT _colour_variant STREQUAL "current")
        string(APPEND _colour_target "_${_colour_variant}")
        set(_colour_kind contract)
    endif()
    jce_add_unit_test(${_colour_target} KIND ${_colour_kind}
        SOURCES test_jce_colour_special_phase.c
        LINK JCE bgfx::bgfx flecs::flecs_static
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene")
    foreach(_colour_part IN ITEMS layer special begin dispatch flush)
        string(TOUPPER "${_colour_part}" _colour_define)
        target_compile_definitions(${_colour_target} PRIVATE
            "JCE_TEST_COLOUR_${_colour_define}_OWNER=\"${_colour_dir}/${_colour_variant}_${_colour_part}.inc.h\"")
    endforeach()
    if(MSVC)
        target_compile_options(${_colour_target} PRIVATE /Zc:preprocessor)
    endif()
    if(NOT _colour_variant STREQUAL "current")
        set_tests_properties(${_colour_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
foreach(_colour_var IN ITEMS draw_path header_path draw header layer function_start function
    opaque_wire transparent_wire dir variant selected selected_header gate mask begin dispatch flush
    special part target kind define)
    unset(_colour_${_colour_var})
endforeach()
unset(_colour_var)
