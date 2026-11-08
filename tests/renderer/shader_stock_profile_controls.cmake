# Complete actual stock tail and headers; typed provider ports, no device.
set(_stock_owner "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_shaders.c")
set(_stock_table "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_shader_stock_table_internal.h")
set(_stock_set "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_shader_set_owned_internal.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_stock_owner}" "${_stock_table}" "${_stock_set}")
file(READ "${_stock_owner}" _stock_source)
set(_stock_marker [=[#include "jce_shader_stock_table_internal.h"]=])
string(FIND "${_stock_source}" "${_stock_marker}" _stock_start)
if(_stock_start LESS 0)
    message(FATAL_ERROR "Actual shader stock owner marker is missing")
endif()
string(SUBSTRING "${_stock_source}" ${_stock_start} -1 _stock_tail)
set(_stock_dir "${CMAKE_CURRENT_BINARY_DIR}/shader-stock-profile")
file(MAKE_DIRECTORY "${_stock_dir}")
jce_test_write_if_changed("${_stock_dir}/entries.inc.h" "${_stock_tail}")
file(READ "${_stock_table}" _stock_table_source)
file(READ "${_stock_set}" _stock_set_source)
function(_jce_stock_exact_replace source_name needle replacement expected)
    string(REPLACE "${needle}" "" _removed "${${source_name}}")
    string(LENGTH "${${source_name}}" _before)
    string(LENGTH "${_removed}" _after)
    string(LENGTH "${needle}" _width)
    math(EXPR _actual "${_before} - ${_after}")
    math(EXPR _expected "${_width} * ${expected}")
    if(NOT _actual EQUAL _expected)
        message(FATAL_ERROR "Stock mutation no longer matches complete actual owner")
    endif()
    string(REPLACE "${needle}" "${replacement}" _changed "${${source_name}}")
    set(${source_name} "${_changed}" PARENT_SCOPE)
endfunction()
set(_stock_missing_filter "${_stock_table_source}")
_jce_stock_exact_replace(_stock_missing_filter
    "if (!jce_shader_stock_variant_selected(profile, variant, key)) continue;"
    "if (false) continue; /* Ordinary missing compatibility filter. */" 1)
jce_test_write_if_changed("${_stock_dir}/missing-filter.h" "${_stock_missing_filter}")
set(_stock_old_zero "${_stock_set_source}")
_jce_stock_exact_replace(_stock_old_zero
    "set->member_ = JCE_INVALID_SHADER;" "set->member_ = (JceShaderHandle){0};" 1)
_jce_stock_exact_replace(_stock_old_zero
    "set->variant[v][k] = JCE_INVALID_SHADER;" "set->variant[v][k] = (JceShaderHandle){0};" 1)
jce_test_write_if_changed("${_stock_dir}/old-zero.h" "${_stock_old_zero}")
foreach(_stock_case IN ITEMS current missing_filter old_zero)
    set(_stock_target test_jce_shader_set_owned)
    set(_stock_kind unit)
    if(NOT _stock_case STREQUAL "current")
        set(_stock_target "test_jce_shader_stock_${_stock_case}")
        set(_stock_kind contract)
    endif()
    jce_add_unit_test(${_stock_target} KIND ${_stock_kind}
        SOURCES test_jce_shader_set_owned.c LINK jce_core
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/renderer"
        "$<TARGET_PROPERTY:bgfx::bgfx,INTERFACE_INCLUDE_DIRECTORIES>")
    target_compile_definitions(${_stock_target} PRIVATE
        JCE_TEST_STOCK_ENTRY_OWNER="${_stock_dir}/entries.inc.h")
    if(MSVC)
        target_compile_options(${_stock_target} PRIVATE /Zc:preprocessor)
    endif()
    if(_stock_case STREQUAL "missing_filter")
        target_compile_definitions(${_stock_target} PRIVATE
            JCE_TEST_SHADER_STOCK_OWNER="${_stock_dir}/missing-filter.h")
    elseif(_stock_case STREQUAL "old_zero")
        target_compile_definitions(${_stock_target} PRIVATE
            JCE_TEST_SHADER_SET_OWNER="${_stock_dir}/old-zero.h")
    endif()
endforeach()
set_tests_properties(test_jce_shader_stock_missing_filter test_jce_shader_stock_old_zero
    PROPERTIES WILL_FAIL TRUE)
