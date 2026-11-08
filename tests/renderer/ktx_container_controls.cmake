# Complete first-party bridge, actual bimg encoding and bounded malformed input.
set(_ktx_owner "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_ktx2_writer.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_ktx_owner}")
file(READ "${_ktx_owner}" _ktx_current)
set(_ktx_dir "${CMAKE_CURRENT_BINARY_DIR}/ktx-container-mutation")
file(MAKE_DIRECTORY "${_ktx_dir}")

function(_jce_ktx_mutate OUTPUT BEFORE AFTER)
    string(FIND "${_ktx_current}" "${BEFORE}" _first)
    string(FIND "${_ktx_current}" "${BEFORE}" _last REVERSE)
    if(_first LESS 0 OR NOT _first EQUAL _last)
        message(FATAL_ERROR "KTX ordinary control requires one exact production wire")
    endif()
    string(REPLACE "${BEFORE}" "${AFTER}" _changed "${_ktx_current}")
    jce_test_write_if_changed("${OUTPUT}" "${_changed}")
endfunction()

_jce_ktx_mutate("${_ktx_dir}/old_untyped_header.cpp"
    "ktx_write_u32_le(w.data + 16u, bytes_per_pixel == 8u ? 0x140bu : 0x1401u);\n    ktx_write_u32_le(w.data + 20u, bytes_per_pixel == 8u ? 2u : 1u);\n    ktx_write_u32_le(w.data + 24u, 0x1908u);"
    "ktx_write_u32_le(w.data + 16u, 0u);\n    ktx_write_u32_le(w.data + 20u, 1u);\n    ktx_write_u32_le(w.data + 24u, 0u);")
_jce_ktx_mutate("${_ktx_dir}/missing_header_checks.cpp"
    "if (!ktx_cube_header_matches(hdr)) return false;"
    "if (false) return false;")
_jce_ktx_mutate("${_ktx_dir}/missing_chain_guard.cpp"
    "if (mips != 1u && mips != levels) return false;"
    "/* Old partial chains cannot satisfy the backend hasMips contract. */")

foreach(_ktx_variant IN ITEMS current old_untyped_header missing_header_checks missing_chain_guard)
    set(_ktx_target test_jce_ktx_container)
    set(_ktx_kind unit)
    set(_ktx_sources test_jce_ktx_container.c)
    if(NOT _ktx_variant STREQUAL "current")
        string(APPEND _ktx_target "_${_ktx_variant}")
        set(_ktx_kind contract)
        list(APPEND _ktx_sources "${_ktx_dir}/${_ktx_variant}.cpp")
    endif()
    jce_add_unit_test(${_ktx_target} KIND ${_ktx_kind}
        SOURCES ${_ktx_sources} LINK JCE bgfx::bgfx
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src"
                 "${CMAKE_SOURCE_DIR}/engine/src/renderer")
    set_target_properties(${_ktx_target} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)
    if(MSVC)
        target_compile_options(${_ktx_target} PRIVATE /Zc:preprocessor)
    endif()
    set(_ktx_work "${_ktx_dir}/${_ktx_target}")
    file(MAKE_DIRECTORY "${_ktx_work}")
    set_tests_properties(${_ktx_target} PROPERTIES WORKING_DIRECTORY "${_ktx_work}")
    if(NOT _ktx_variant STREQUAL "current")
        set_tests_properties(${_ktx_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
foreach(_ktx_var IN ITEMS owner current dir variant target kind sources work)
    unset(_ktx_${_ktx_var})
endforeach()
unset(_ktx_var)
