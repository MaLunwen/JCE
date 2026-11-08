
# EOF must withdraw replay origin even without an intervening frame update.
set(_origin_record "${CMAKE_SOURCE_DIR}/engine/src/os/platform/jce_input_record.c")
file(READ "${_origin_record}" _origin_source)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_origin_record}")
set(_origin_wire "    jce_input_clear_snapshot_replayed(input);")
string(FIND "${_origin_source}" "${_origin_wire}" _origin_first)
string(FIND "${_origin_source}" "${_origin_wire}" _origin_last REVERSE)
if(_origin_first LESS 0 OR NOT _origin_first EQUAL _origin_last)
    message(FATAL_ERROR "Replay origin control requires one actual per-attempt reset")
endif()
string(REPLACE "${_origin_wire}" "    /* old owner retained the last applied origin at EOF */" _origin_old "${_origin_source}")
set(_origin_path "${CMAKE_CURRENT_BINARY_DIR}/input-record-mutation/stale_origin.c")
jce_test_write_if_changed("${_origin_path}" "${_origin_old}")
jce_add_unit_test(test_jce_input_record_stale_origin KIND contract
    SOURCES test_jce_input_record_roundtrip.c "${_origin_path}"
    LINK jce_platform jce_core)
target_include_directories(test_jce_input_record_stale_origin PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/os/platform")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/stale-origin-files")
set_tests_properties(test_jce_input_record_stale_origin PROPERTIES WILL_FAIL TRUE
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/stale-origin-files")
foreach(_origin_cleanup IN ITEMS record source wire first last old path)
    unset(_origin_${_origin_cleanup})
endforeach()
unset(_origin_cleanup)
