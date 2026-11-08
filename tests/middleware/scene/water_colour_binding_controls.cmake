# Scope: actual program selection, geometry/global-bind and terminal submit.
# Water simulation and the intervening resource/custom-uniform work are excluded.
set(_water_binding_path "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_sr_environment.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_water_binding_path}")
file(READ "${_water_binding_path}" _water_binding_source)
string(FIND "${_water_binding_source}" "void sr_draw_water(" _water_begin)
string(FIND "${_water_binding_source}" "void sr_draw_water(" _water_last REVERSE)
if(_water_begin LESS 0 OR NOT _water_begin EQUAL _water_last)
    message(FATAL_ERROR "Water binding fixture requires one actual draw owner")
endif()
string(SUBSTRING "${_water_binding_source}" ${_water_begin} -1 _water_binding_body)
string(FIND "${_water_binding_body}" "\n}\n\n/*" _water_end)
if(_water_end LESS 0)
    message(FATAL_ERROR "Water binding fixture lost the complete draw boundary")
endif()
math(EXPR _water_length "${_water_end} + 2")
string(SUBSTRING "${_water_binding_body}" 0 ${_water_length} _water_binding_body)

function(_jce_water_replace source needle replacement out)
    string(FIND "${source}" "${needle}" _first)
    string(FIND "${source}" "${needle}" _last REVERSE)
    if(_first LESS 0 OR NOT _first EQUAL _last)
        message(FATAL_ERROR "Water binding mutation requires one actual wire: ${needle}")
    endif()
    string(REPLACE "${needle}" "${replacement}" _changed "${source}")
    set(${out} "${_changed}" PARENT_SCOPE)
endfunction()
function(_jce_water_region source begin end out)
    string(FIND "${source}" "${begin}" _start)
    string(FIND "${source}" "${begin}" _last REVERSE)
    string(FIND "${source}" "${end}" _finish)
    if(_start LESS 0 OR NOT _start EQUAL _last OR _finish LESS_EQUAL _start)
        message(FATAL_ERROR "Water binding fixture requires its actual region: ${begin}")
    endif()
    math(EXPR _length "${_finish} - ${_start}")
    string(SUBSTRING "${source}" ${_start} ${_length} _body)
    set(${out} "${_body}" PARENT_SCOPE)
endfunction()

_jce_water_region("${_water_binding_source}"
    "static bgfx_program_handle_t sr_water_select_program("
    "/* Lazily acquire only the uniforms used by this water program. */"
    _water_program)
set(_water_program_path "${CMAKE_CURRENT_BINARY_DIR}/water_program.inc.h")
jce_test_write_if_changed("${_water_program_path}" "${_water_program}")

# Reject extra relevant writes outside the selected regions, rather than
# silently omitting a future duplicate in the intervening water code.
foreach(_water_redundant IN ITEMS "sr_bind_frame_shadow_state(" "jce_pbr_material_bind_view_mode(")
    string(FIND "${_water_binding_body}" "${_water_redundant}" _water_found)
    if(NOT _water_found LESS 0)
        message(FATAL_ERROR "Water draw contains a second owner for global bindings: ${_water_redundant}")
    endif()
endforeach()
set(_water_geometry "    bgfx_vertex_buffer_handle_t vbh = { (uint16_t)jce_mesh_get_vbh(mesh) };\n    bgfx_index_buffer_handle_t  ibh = { (uint16_t)jce_mesh_get_ibh(mesh) };\n    if (!BGFX_HANDLE_IS_VALID(vbh) || !BGFX_HANDLE_IS_VALID(ibh)) return;")
foreach(_water_variant IN ITEMS current duplicate_shadow duplicate_view_mode late_geometry)
    set(_water_selected "${_water_binding_body}")
    if(_water_variant STREQUAL "duplicate_shadow")
        _jce_water_replace("${_water_selected}" "\n        sr_bind_cloud_shadow(sr);\n"
            "\n    sr_bind_frame_shadow_state(sr);\n        sr_bind_cloud_shadow(sr);\n" _water_selected)
    elseif(_water_variant STREQUAL "duplicate_view_mode")
        _jce_water_replace("${_water_selected}"
            "    bgfx_submit(view_id, draw_program, 0, BGFX_DISCARD_ALL);"
            "    jce_pbr_material_bind_view_mode();\n    bgfx_submit(view_id, draw_program, 0, BGFX_DISCARD_ALL);"
            _water_selected)
    elseif(_water_variant STREQUAL "late_geometry")
        _jce_water_replace("${_water_selected}" "${_water_geometry}" "" _water_selected)
        set(_water_vertex_choice "    if (compat_fft_vertices)\n        bgfx_set_transient_vertex_buffer(")
        _jce_water_replace("${_water_selected}" "${_water_vertex_choice}"
            "${_water_geometry}\n${_water_vertex_choice}" _water_selected)
    endif()
    _jce_water_region("${_water_selected}"
        "    /* Geometry admission precedes the first material or uniform write. */"
        "    /* One render-resource cache," _water_admission)
    _jce_water_region("${_water_selected}"
        "    /* The atlas producer discards encoder state after its submits."
        "    if (!legacy_gl21) {\n        const JceWaterSpectrumComponent *spectrum =" _water_global)
    set(_water_prefix "${_water_admission}${_water_global}")
    _jce_water_region("${_water_selected}"
        "    /* Global shadows and view mode were already published by the PBR bind. */"
        "\n}" _water_terminal)
    set(_water_prefix_path "${CMAKE_CURRENT_BINARY_DIR}/water_${_water_variant}_prefix.inc.h")
    set(_water_terminal_path "${CMAKE_CURRENT_BINARY_DIR}/water_${_water_variant}_terminal.inc.h")
    jce_test_write_if_changed("${_water_prefix_path}" "${_water_prefix}")
    jce_test_write_if_changed("${_water_terminal_path}" "${_water_terminal}")
    set(_water_target test_jce_water_colour_binding)
    set(_water_kind unit)
    if(NOT _water_variant STREQUAL "current")
        string(APPEND _water_target "_${_water_variant}")
        set(_water_kind contract)
    endif()
    jce_add_unit_test(${_water_target} KIND ${_water_kind}
        SOURCES "${CMAKE_SOURCE_DIR}/tests/middleware/scene/test_jce_water_colour_binding.c"
        LINK jce_core
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src"
            "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene")
    target_link_libraries(${_water_target} PRIVATE bgfx::bgfx)
    target_include_directories(${_water_target} SYSTEM PRIVATE
        "$<TARGET_PROPERTY:sdl::sdl,INTERFACE_INCLUDE_DIRECTORIES>")
    target_compile_definitions(${_water_target} PRIVATE
        "JCE_TEST_WATER_PROGRAM=\"${_water_program_path}\""
        "JCE_TEST_WATER_BINDING_PREFIX=\"${_water_prefix_path}\""
        "JCE_TEST_WATER_BINDING_TERMINAL=\"${_water_terminal_path}\"")
    if(MSVC)
        target_compile_options(${_water_target} PRIVATE /Gy /Gw /Zc:preprocessor)
        target_link_options(${_water_target} PRIVATE /OPT:REF)
    else()
        target_compile_options(${_water_target} PRIVATE -ffunction-sections -fdata-sections)
        if(APPLE)
            target_link_options(${_water_target} PRIVATE -Wl,-dead_strip)
        else()
            target_link_options(${_water_target} PRIVATE -Wl,--gc-sections)
        endif()
        if(WIN32)
            target_compile_options(${_water_target} PRIVATE -flto)
            target_link_options(${_water_target} PRIVATE -flto)
        endif()
    endif()
    if(NOT _water_variant STREQUAL "current")
        set_tests_properties(${_water_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
foreach(_water_var IN ITEMS binding_path binding_source begin last binding_body end length redundant found
        geometry variant selected admission global prefix terminal prefix_path terminal_path target kind)
    unset(_water_${_water_var})
endforeach()
unset(_water_var)
