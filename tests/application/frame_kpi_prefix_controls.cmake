# Complete encoder with typed test ports and its exact former row formatting.
set(_kpi_prefix_owner "${CMAKE_SOURCE_DIR}/engine/src/application/jce_frame_kpi.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_kpi_prefix_owner}")
file(READ "${_kpi_prefix_owner}" _kpi_prefix_current)

# Count literal occurrences through byte lengths: C semicolons must not become
# list separators, and a missing/duplicated owner wire must fail configure.
function(_jce_kpi_prefix_replace input old replacement expected output)
    string(LENGTH "${old}" _old_length)
    string(LENGTH "${input}" _before_length)
    string(REPLACE "${old}" "" _removed "${input}")
    string(LENGTH "${_removed}" _after_length)
    math(EXPR _count "(${_before_length} - ${_after_length}) / ${_old_length}")
    if(NOT _count EQUAL expected)
        message(FATAL_ERROR "KPI prefix control needs ${expected} exact owner wires, got ${_count}: ${old}")
    endif()
    string(REPLACE "${old}" "${replacement}" _changed "${input}")
    set(${output} "${_changed}" PARENT_SCOPE)
endfunction()

_jce_kpi_prefix_replace("${_kpi_prefix_current}"
    "size_t *used, const char *frame_prefix,"
    "size_t *used, uint64_t frame," 1 _kpi_prefix_old)
_jce_kpi_prefix_replace("${_kpi_prefix_old}"
    "\"%s%s,%.4f%s\\n\", frame_prefix, name, ms, suffix"
    "\"%llu,%s,%.4f%s\\n\", (unsigned long long)frame, name, ms, suffix"
    1 _kpi_prefix_old)
_jce_kpi_prefix_replace("${_kpi_prefix_old}"
    "append_row(buffer, &used, frame_prefix,"
    "append_row(buffer, &used, frame," 5 _kpi_prefix_old)
set(_kpi_prefix_block "    /* One uint64 conversion per packet, including every growth retry. */\n    char frame_prefix[22]; /* 20 decimal digits, comma and terminating NUL. */\n    int prefix_length = snprintf(frame_prefix, sizeof frame_prefix, \"%llu,\",\n                                 (unsigned long long)frame);\n    if (prefix_length < 0 || (size_t)prefix_length >= sizeof frame_prefix) return false;\n")
_jce_kpi_prefix_replace("${_kpi_prefix_old}" "${_kpi_prefix_block}" "" 1 _kpi_prefix_old)
string(FIND "${_kpi_prefix_old}" "frame_prefix" _kpi_prefix_remaining)
if(NOT _kpi_prefix_remaining EQUAL -1)
    message(FATAL_ERROR "KPI old-per-row control left a prefix wire behind")
endif()
set(_kpi_prefix_dir "${CMAKE_CURRENT_BINARY_DIR}/kpi-prefix-mutation")
file(MAKE_DIRECTORY "${_kpi_prefix_dir}")
set(_kpi_prefix_old_path "${_kpi_prefix_dir}/jce_frame_kpi_old_per_row.c")
unset(_kpi_prefix_previous)
if(EXISTS "${_kpi_prefix_old_path}")
    file(READ "${_kpi_prefix_old_path}" _kpi_prefix_previous)
endif()
if(NOT _kpi_prefix_previous STREQUAL _kpi_prefix_old)
    file(WRITE "${_kpi_prefix_old_path}" "${_kpi_prefix_old}")
endif()
foreach(_kpi_prefix_variant IN ITEMS current old_per_row)
    set(_kpi_prefix_target test_jce_frame_kpi_prefix)
    set(_kpi_prefix_selected "${_kpi_prefix_owner}")
    set(_kpi_prefix_kind unit)
    if(_kpi_prefix_variant STREQUAL "old_per_row")
        string(APPEND _kpi_prefix_target "_old_per_row")
        set(_kpi_prefix_selected "${_kpi_prefix_old_path}")
        set(_kpi_prefix_kind contract)
    endif()
    jce_add_unit_test(${_kpi_prefix_target} KIND ${_kpi_prefix_kind}
        SOURCES test_jce_frame_kpi_prefix.c LINK jce_core sdl::sdl
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/application")
    target_compile_definitions(${_kpi_prefix_target} PRIVATE
        JCE_TEST_FRAME_KPI_OWNER="${_kpi_prefix_selected}"
        JCE_TEST_FRAME_KPI_OLD_OWNER="${_kpi_prefix_old_path}")
    if(_kpi_prefix_variant STREQUAL "old_per_row")
        set_tests_properties(${_kpi_prefix_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
foreach(_kpi_prefix_variable IN ITEMS owner current old block remaining dir old_path previous
    variant target selected kind)
    unset(_kpi_prefix_${_kpi_prefix_variable})
endforeach()
unset(_kpi_prefix_variable)
