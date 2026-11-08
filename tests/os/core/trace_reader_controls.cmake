# Fixed reader diagnostic controls consume the complete capture owner.
file(READ "${CMAKE_SOURCE_DIR}/engine/src/os/core/jce_trace_capture.c" _reader_capture_owner)
function(jce_reader_diagnostic_negative name wire replacement)
    string(FIND "${_reader_capture_owner}" "${wire}" first)
    string(FIND "${_reader_capture_owner}" "${wire}" last REVERSE)
    if(first LESS 0 OR NOT first EQUAL last)
        message(FATAL_ERROR "${name} requires one actual reader diagnostic wire")
    endif()
    string(REPLACE "${wire}" "${replacement}" mutant "${_reader_capture_owner}")
    set(path "${CMAKE_CURRENT_BINARY_DIR}/trace-capture-mutation/${name}.c")
    jce_test_write_if_changed("${path}" "${mutant}")
    jce_add_unit_test(${name} KIND contract SOURCES test_jce_trace_capture.c
        LINK jce_core sdl::sdl INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/os/core")
    target_compile_definitions(${name} PRIVATE JCE_TEST_TRACE_CAPTURE_OWNER="${path}"
        JCE_TEST_TRACE_CAPTURE_FILE_PREFIX="${name}" JCE_TEST_TRACE_CAPTURE_READER_ONLY=1)
    set_tests_properties(${name} PROPERTIES WILL_FAIL TRUE)
endfunction()
jce_reader_diagnostic_negative(test_jce_trace_capture_missing_reader_metadata
    "\"\\\"slowest_requested_bytes\\\":%llu},\\\"reader_diagnostics\\\":%s,\""
    "\"\\\"slowest_requested_bytes\\\":%llu},\\\"reader_diagnostics_omitted\\\":%s,\"")
jce_reader_diagnostic_negative(test_jce_trace_capture_missing_first_loss
    "if (!capture->reader_loss_reads) capture->first_loss = witness;"
    "if (false) capture->first_loss = witness; /* Missing first-loss retention. */")
unset(_reader_capture_owner)
