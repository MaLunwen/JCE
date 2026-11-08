# Complete public Engine RED/GREEN fixture. The frozen old result body changes
# production publication only; both targets consume the identical expectations.
set(_interval_owner "${CMAKE_SOURCE_DIR}/engine/src/application/jce_engine.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_interval_owner}")
file(READ "${_interval_owner}" _interval_source)
set(_interval_start "static JceAppResult engine_application_result(JceEngine *e, JceAppResult result)\n{")
set(_interval_end "static void jce_engine_reset_frame_clock(JceEngine *e)")
string(FIND "${_interval_source}" "${_interval_start}" _interval_first)
string(FIND "${_interval_source}" "${_interval_start}" _interval_first_last REVERSE)
string(FIND "${_interval_source}" "${_interval_end}" _interval_last)
string(FIND "${_interval_source}" "${_interval_end}" _interval_last_last REVERSE)
if(_interval_first LESS 0 OR _interval_last LESS_EQUAL _interval_first
   OR NOT _interval_first EQUAL _interval_first_last OR NOT _interval_last EQUAL _interval_last_last)
    message(FATAL_ERROR "Disposition control requires one complete actual Engine result owner")
endif()
math(EXPR _interval_length "${_interval_last} - ${_interval_first}")
string(SUBSTRING "${_interval_source}" ${_interval_first} ${_interval_length} _interval_result)
set(_interval_old_result [=[static JceAppResult engine_application_result(JceEngine *e, JceAppResult result)
{
    if (result == JCE_APP_CONTINUE) return result;
    result = jce_engine_quit_resolve(result);
    e->finalizing = true;
    if (!engine_export_pending_interval(e, jce_time_perf_counter()))
        result = JCE_APP_FAILURE;
    JceAppResult finalized = jce_engine_kpi_capture_result(&e->frame_kpi, result);
    if (e->kpi_clock.enabled && !e->kpi_clock.reported) {
        const bool valid = jce_engine_kpi_clock_finish(&e->kpi_clock, e->frame_kpi.index);
        e->kpi_clock.reported = true;
        jce_log_write(JCE_LOG_LEVEL_INFO, LOG_TAG, __FILE__, __LINE__,
            "frame KPI clock v1: frequency=%llu timer_start=%llu "
            "timer_checked=%llu render_start=%llu render_end=%llu "
            "render_ticks=%llu render_intervals=%llu omitted_ticks=%llu valid=%u errors=%u",
            (unsigned long long)e->kpi_clock.frequency,
            (unsigned long long)e->kpi_clock.timer_start,
            (unsigned long long)e->kpi_clock.timer_checked,
            (unsigned long long)e->kpi_clock.render_start,
            (unsigned long long)e->kpi_clock.render_end,
            (unsigned long long)e->kpi_clock.render_ticks,
            (unsigned long long)e->kpi_clock.render_intervals,
            (unsigned long long)e->kpi_clock.omitted_ticks, (unsigned)valid,
            (unsigned)e->kpi_clock.errors);
    }
    e->finalizing = false;
    /* A completion/lifecycle callback may request failure during finalization. */
    return jce_engine_quit_resolve(finalized);
}

]=])
string(REPLACE "${_interval_result}" "${_interval_old_result}" _interval_old_source "${_interval_source}")
# Direct teardown is the other current terminal owner. Retain its real cleanup
# while restoring the old absence of disposition publication on that path too.
set(_interval_destroy_report "    (void)jce_engine_perf_disposition_report(&e->perf_hitch);\n")
string(FIND "${_interval_old_source}" "${_interval_destroy_report}" _interval_destroy_first)
string(FIND "${_interval_old_source}" "${_interval_destroy_report}" _interval_destroy_last REVERSE)
if(_interval_destroy_first LESS 0 OR NOT _interval_destroy_first EQUAL _interval_destroy_last)
    message(FATAL_ERROR "Old terminal control requires one actual teardown publication wire")
endif()
string(REPLACE "${_interval_destroy_report}" "" _interval_old_source "${_interval_old_source}")
set(_interval_old_path "${CMAKE_CURRENT_BINARY_DIR}/interval-owner/jce_engine_old_terminal.c")
jce_test_write_if_changed("${_interval_old_path}" "${_interval_old_source}")
set_property(SOURCE "${_interval_old_path}" APPEND PROPERTY COMPILE_DEFINITIONS JCE_BUILDING_ENGINE=1)
jce_add_unit_test(test_jce_engine_kpi_disposition_old_terminal KIND contract
    SOURCES test_jce_engine_kpi_lifecycle.c "${_interval_old_path}"
    LINK JCE sdl::sdl bgfx::bgfx
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/application"
        "${CMAKE_SOURCE_DIR}/tests/os/core/stubs")
target_compile_definitions(test_jce_engine_kpi_disposition_old_terminal PRIVATE
    JCE_ENGINE_KPI_TEST_OUTPUT="${_kpi_lifecycle_output}/old-terminal")
if(MSVC)
    target_compile_options(test_jce_engine_kpi_disposition_old_terminal PRIVATE /Zc:preprocessor)
endif()
file(MAKE_DIRECTORY "${_kpi_lifecycle_output}/old-terminal")
add_test(NAME test_jce_engine_kpi_disposition COMMAND test_jce_engine_kpi_lifecycle)
set_property(GLOBAL APPEND PROPERTY JCE_TESTS_WITH_ENV test_jce_engine_kpi_disposition)
set_tests_properties(test_jce_engine_kpi_disposition PROPERTIES
    TIMEOUT 60 LABELS "unit" JCE_TEST_KIND integration
    ENVIRONMENT "${_kpi_lifecycle_env};JCE_MAX_SECONDS=240;JCE_ENGINE_KPI_LIFECYCLE_CASE=disposition;JCE_KPI_FRAME_LOG=${_kpi_lifecycle_output}/disposition.csv;JCE_LOG_FILE=${_kpi_lifecycle_output}/disposition.log")
set_tests_properties(test_jce_engine_kpi_disposition_old_terminal PROPERTIES
    WILL_FAIL TRUE
    ENVIRONMENT "${_kpi_lifecycle_env};JCE_MAX_SECONDS=240;JCE_ENGINE_KPI_LIFECYCLE_CASE=disposition;JCE_KPI_FRAME_LOG=${_kpi_lifecycle_output}/old-terminal/disposition.csv;JCE_LOG_FILE=${_kpi_lifecycle_output}/old-terminal/disposition.log")
add_test(NAME test_jce_engine_kpi_disposition_direct_destroy COMMAND test_jce_engine_kpi_lifecycle)
add_test(NAME test_jce_engine_kpi_disposition_old_terminal_direct_destroy
    COMMAND test_jce_engine_kpi_disposition_old_terminal)
set_property(GLOBAL APPEND PROPERTY JCE_TESTS_WITH_ENV
    test_jce_engine_kpi_disposition_direct_destroy test_jce_engine_kpi_disposition_old_terminal_direct_destroy)
set_tests_properties(test_jce_engine_kpi_disposition_direct_destroy PROPERTIES
    TIMEOUT 60 LABELS "unit" JCE_TEST_KIND integration
    ENVIRONMENT "${_kpi_lifecycle_env};JCE_MAX_SECONDS=240;JCE_ENGINE_KPI_LIFECYCLE_CASE=disposition-direct;JCE_KPI_FRAME_LOG=${_kpi_lifecycle_output}/disposition-direct.csv;JCE_LOG_FILE=${_kpi_lifecycle_output}/disposition-direct.log")
set_tests_properties(test_jce_engine_kpi_disposition_old_terminal_direct_destroy PROPERTIES
    TIMEOUT 60 LABELS "unit" JCE_TEST_KIND contract WILL_FAIL TRUE
    ENVIRONMENT "${_kpi_lifecycle_env};JCE_MAX_SECONDS=240;JCE_ENGINE_KPI_LIFECYCLE_CASE=disposition-direct;JCE_KPI_FRAME_LOG=${_kpi_lifecycle_output}/old-terminal/disposition-direct.csv;JCE_LOG_FILE=${_kpi_lifecycle_output}/old-terminal/disposition-direct.log")
set(_direct_clock_wire "    engine_report_kpi_clock(e, frame_kpi_records);\n")
string(FIND "${_interval_source}" "${_direct_clock_wire}" _direct_clock_first)
string(FIND "${_interval_source}" "${_direct_clock_wire}" _direct_clock_last REVERSE)
if(_direct_clock_first LESS 0 OR NOT _direct_clock_first EQUAL _direct_clock_last)
    message(FATAL_ERROR "Direct clock negative requires one actual teardown reporter wire")
endif()
string(REPLACE "${_direct_clock_wire}" "    (void)frame_kpi_records; /* Former direct teardown omitted clock publication. */\n"
    _direct_clock_old "${_interval_source}")
set(_direct_clock_path "${CMAKE_CURRENT_BINARY_DIR}/interval-owner/jce_engine_old_direct_clock.c")
jce_test_write_if_changed("${_direct_clock_path}" "${_direct_clock_old}")
set_property(SOURCE "${_direct_clock_path}" APPEND PROPERTY COMPILE_DEFINITIONS JCE_BUILDING_ENGINE=1)
jce_add_unit_test(test_jce_engine_kpi_disposition_old_direct_clock KIND contract
    SOURCES test_jce_engine_kpi_lifecycle.c "${_direct_clock_path}"
    LINK JCE sdl::sdl bgfx::bgfx
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/application"
        "${CMAKE_SOURCE_DIR}/tests/os/core/stubs")
target_compile_definitions(test_jce_engine_kpi_disposition_old_direct_clock PRIVATE
    JCE_ENGINE_KPI_TEST_OUTPUT="${_kpi_lifecycle_output}/old-direct-clock")
if(MSVC)
    target_compile_options(test_jce_engine_kpi_disposition_old_direct_clock PRIVATE /Zc:preprocessor)
endif()
file(MAKE_DIRECTORY "${_kpi_lifecycle_output}/old-direct-clock")
set_tests_properties(test_jce_engine_kpi_disposition_old_direct_clock PROPERTIES
    WILL_FAIL TRUE
    ENVIRONMENT "${_kpi_lifecycle_env};JCE_MAX_SECONDS=240;JCE_ENGINE_KPI_LIFECYCLE_CASE=disposition-direct;JCE_KPI_FRAME_LOG=${_kpi_lifecycle_output}/old-direct-clock/disposition-direct.csv;JCE_LOG_FILE=${_kpi_lifecycle_output}/old-direct-clock/disposition-direct.log")
unset(_direct_clock_wire)
unset(_direct_clock_first)
unset(_direct_clock_last)
unset(_direct_clock_old)
unset(_direct_clock_path)
set(_disposition_owner "${CMAKE_SOURCE_DIR}/engine/src/application/jce_engine_perf_hitch.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_disposition_owner}")
file(READ "${_disposition_owner}" _disposition_source)
set(_disposition_release "    d->released = true;")
string(FIND "${_disposition_source}" "${_disposition_release}" _disposition_first)
string(FIND "${_disposition_source}" "${_disposition_release}" _disposition_last REVERSE)
if(_disposition_first LESS 0 OR NOT _disposition_first EQUAL _disposition_last)
    message(FATAL_ERROR "Disposition negative requires one actual release witness")
endif()
string(REPLACE "${_disposition_release}" "    d->released = false; /* Former missing release witness. */"
    _disposition_old "${_disposition_source}")
set(_disposition_path "${CMAKE_CURRENT_BINARY_DIR}/interval-owner/jce_engine_perf_hitch_old_release.c")
jce_test_write_if_changed("${_disposition_path}" "${_disposition_old}")
jce_add_unit_test(test_jce_engine_perf_hitch_old_disposition_release KIND contract
    SOURCES test_jce_engine_perf_hitch.c LINK jce_core
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/application")
target_compile_definitions(test_jce_engine_perf_hitch_old_disposition_release PRIVATE
    JCE_TEST_PERF_HITCH_OWNER="${_disposition_path}")
set_tests_properties(test_jce_engine_perf_hitch_old_disposition_release PROPERTIES WILL_FAIL TRUE)
unset(_disposition_owner)
unset(_disposition_source)
unset(_disposition_release)
unset(_disposition_first)
unset(_disposition_last)
unset(_disposition_old)
unset(_disposition_path)
unset(_interval_owner)
unset(_interval_source)
unset(_interval_start)
unset(_interval_end)
unset(_interval_first)
unset(_interval_first_last)
unset(_interval_last)
unset(_interval_last_last)
unset(_interval_length)
unset(_interval_result)
unset(_interval_old_result)
unset(_interval_old_source)
unset(_interval_old_path)
unset(_interval_destroy_report)
unset(_interval_destroy_first)
unset(_interval_destroy_last)

function(jce_test_background_interval_controls)
    set(_wire "        jce_engine_perf_disposition_reason(&e->perf_hitch,\n            JCE_ENGINE_DISPOSITION_DRAWABLE + e->host_lifecycle.drawable_reason);\n")
    _jce_background_replace("${_min_branch}" "${_wire}" ""
        _background_old_disposition)
    _jce_background_fixture(test_jce_engine_background_old_disposition_wire
        "${_background_old_disposition}" "${_background_drawable}"
        "${_background_watch}" "${_background_lifecycle}" contract)
    set_tests_properties(test_jce_engine_background_old_disposition_wire PROPERTIES WILL_FAIL TRUE)
endfunction()
