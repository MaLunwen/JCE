# Complete shipping writer mutations. All must fail ordinary unchanged CPU
# assertions after real sink/executor owners have been released, never link or
# compile errors. The current positive target is registered by CMakeLists.txt.
set(_kpi_writer_owner "${CMAKE_SOURCE_DIR}/engine/src/application/jce_frame_kpi_writer.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_kpi_writer_owner}")
file(READ "${_kpi_writer_owner}" _kpi_writer_source)
foreach(_kpi_writer_variant IN ITEMS missing_failure_snapshot inferred_occupancy old_immediate_dispatch)
    if(_kpi_writer_variant STREQUAL "missing_failure_snapshot")
        set(_kpi_writer_needle "first->captured = true;")
        set(_kpi_writer_replacement "first->captured = false;")
    elseif(_kpi_writer_variant STREQUAL "old_immediate_dispatch")
        set(_kpi_writer_needle "if (!ready) return true;")
        set(_kpi_writer_replacement "if (false && !ready) return true;")
    else()
        set(_kpi_writer_needle "first->queued_bytes = w->status.queued_bytes;")
        set(_kpi_writer_replacement "first->queued_bytes = (size_t)(w->status.accepted_bytes - w->status.written_bytes);")
    endif()
    string(FIND "${_kpi_writer_source}" "${_kpi_writer_needle}" _kpi_writer_first)
    string(FIND "${_kpi_writer_source}" "${_kpi_writer_needle}" _kpi_writer_last REVERSE)
    if(_kpi_writer_first LESS 0 OR NOT _kpi_writer_first EQUAL _kpi_writer_last)
        message(FATAL_ERROR "KPI writer control requires exactly one ${_kpi_writer_variant} wire")
    endif()
    string(REPLACE "${_kpi_writer_needle}" "${_kpi_writer_replacement}"
        _kpi_writer_mutated "${_kpi_writer_source}")
    if(_kpi_writer_mutated STREQUAL _kpi_writer_source)
        message(FATAL_ERROR "KPI writer ${_kpi_writer_variant} mutation did not change the owner")
    endif()
    set(_kpi_writer_path "${CMAKE_CURRENT_BINARY_DIR}/frame-kpi-writer-${_kpi_writer_variant}.c")
    unset(_kpi_writer_previous)
    if(EXISTS "${_kpi_writer_path}")
        file(READ "${_kpi_writer_path}" _kpi_writer_previous)
    endif()
    if(NOT _kpi_writer_previous STREQUAL _kpi_writer_mutated)
        file(WRITE "${_kpi_writer_path}" "${_kpi_writer_mutated}")
    endif()
    set(_kpi_writer_target "test_jce_frame_kpi_writer_${_kpi_writer_variant}")
    jce_add_unit_test(${_kpi_writer_target} KIND contract
        SOURCES test_jce_frame_kpi_writer.c
            "${CMAKE_SOURCE_DIR}/engine/src/application/jce_frame_kpi.c"
        LINK jce_core sdl::sdl
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src"
            "${CMAKE_SOURCE_DIR}/engine/src/application")
    target_compile_definitions(${_kpi_writer_target} PRIVATE
        JCE_TEST_KPI_WRITER_OWNER="${_kpi_writer_path}")
    set_tests_properties(${_kpi_writer_target} PROPERTIES WILL_FAIL TRUE)
endforeach()
foreach(_kpi_writer_variable IN ITEMS owner source variant needle replacement first last mutated path previous target)
    unset(_kpi_writer_${_kpi_writer_variable})
endforeach()
unset(_kpi_writer_variable)
