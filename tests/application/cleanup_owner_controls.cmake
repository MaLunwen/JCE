# Combined bootstrap failure and sampler refusal use the actual Engine and
# actual sampler owner, replacing only sampler native operations with typed ports.
set(_cleanup_output "${CMAKE_CURRENT_BINARY_DIR}/engine-sampler-lifetime-output")
file(MAKE_DIRECTORY "${_cleanup_output}")
set(_cleanup_env "${JCE_TEST_ENVIRONMENT};SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;JCE_HEADLESS=1;JCE_BACKEND=noop;JCE_TRACE=1;JCE_TRACE_STREAM=;JCE_TRACE_STREAM_MODE=;JCE_TRACE_EXPORT=;JCE_PERF_LOG=0;JCE_MAX_FRAMES=;JCE_MAX_SECONDS=;JCE_KPI_FRAME_LOG=;JCE_KPI_ASSET_LOG=;JCE_SAMPLER=;JCE_KPI_FRAME_COUNT=0;JCE_LAUNCH_VSYNC=0;JCE_LAUNCH_TARGET_FPS=0")
# Preserve the older ready-session shutdown route even if the parent process
# previously selected one of this fixture's new failed-bootstrap modes.
foreach(_cleanup_existing IN ITEMS test_jce_engine_sampler_lifetime
    test_jce_engine_sampler_lifetime_wait_failed
    test_jce_engine_sampler_lifetime_resume_failed)
    set_property(TEST ${_cleanup_existing} APPEND PROPERTY
        ENVIRONMENT "JCE_ENGINE_CLEANUP_CASE=")
endforeach()

function(_jce_engine_cleanup_case NAME TARGET KIND OWNERSHIP MODE)
    if(NOT NAME STREQUAL TARGET)
        add_test(NAME ${NAME} COMMAND ${TARGET})
        set_property(GLOBAL APPEND PROPERTY JCE_TESTS_WITH_ENV ${NAME})
    endif()
    # Reuse one existing integration case for actual producer-to-analyzer
    # evidence. Other ownership/mutant cases remain stream-independent.
    set(_capture "")
    if(NAME STREQUAL "test_jce_engine_cleanup_owned_timeout")
        set(_capture "1")
    endif()
    set_tests_properties(${NAME} PROPERTIES
        TIMEOUT 60 LABELS "unit" JCE_TEST_KIND ${KIND}
        RUN_SERIAL TRUE RESOURCE_LOCK jce_renderer_context
        ENVIRONMENT "${_cleanup_env};JCE_ENGINE_CLEANUP_TRACE=${_capture};JCE_ENGINE_CLEANUP_CASE=${OWNERSHIP};JCE_ENGINE_SAMPLER_CASE=${MODE};JCE_LOG_FILE=${_cleanup_output}/${NAME}.log")
endfunction()

foreach(_cleanup_ownership IN ITEMS owned legacy)
    foreach(_cleanup_mode IN ITEMS timeout wait_failed resume_failed)
        set(_cleanup_test "test_jce_engine_cleanup_${_cleanup_ownership}_${_cleanup_mode}")
        _jce_engine_cleanup_case(${_cleanup_test}
            test_jce_engine_sampler_lifetime integration
            ${_cleanup_ownership} ${_cleanup_mode})
    endforeach()
endforeach()
_jce_engine_cleanup_case(test_jce_engine_cleanup_constructing
    test_jce_engine_sampler_lifetime integration constructing timeout)
_jce_engine_cleanup_case(test_jce_engine_cleanup_callback
    test_jce_engine_sampler_lifetime integration callback timeout)

# The former constructor discarded its output after deferred teardown. Compile
# the complete actual Engine TU with just that output-retention wire removed;
# the same early nonnull assertion must fail without reading freed services.
set(_cleanup_engine "${CMAKE_SOURCE_DIR}/engine/src/application/jce_engine.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_cleanup_engine}")
file(READ "${_cleanup_engine}" _cleanup_source)
function(_jce_engine_cleanup_mutant NAME WIRE REPLACEMENT OWNERSHIP)
    string(FIND "${_cleanup_source}" "${WIRE}" _first)
    string(FIND "${_cleanup_source}" "${WIRE}" _last REVERSE)
    if(_first LESS 0 OR NOT _first EQUAL _last)
        message(FATAL_ERROR "${NAME} requires one actual cleanup owner wire")
    endif()
    string(REPLACE "${WIRE}" "${REPLACEMENT}" _old "${_cleanup_source}")
    set(_path "${CMAKE_CURRENT_BINARY_DIR}/cleanup-owner/${NAME}.c")
    jce_test_write_if_changed("${_path}" "${_old}")
    set_property(SOURCE "${_path}" APPEND PROPERTY
        COMPILE_DEFINITIONS JCE_BUILDING_ENGINE=1)
    jce_add_unit_test(${NAME} KIND contract
        SOURCES test_jce_engine_sampler_lifetime.c "${_path}"
        LINK JCE sdl::sdl bgfx::bgfx
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src"
            "${CMAKE_SOURCE_DIR}/engine/src/application"
            "${CMAKE_SOURCE_DIR}/tests/os/core/stubs")
    target_compile_definitions(${NAME} PRIVATE
        JCE_ENGINE_SAMPLER_TEST_OUTPUT="${_cleanup_output}")
    if(MSVC)
        target_compile_options(${NAME} PRIVATE /Zc:preprocessor)
    endif()
    _jce_engine_cleanup_case(${NAME} ${NAME} contract ${OWNERSHIP} timeout)
    set_tests_properties(${NAME} PROPERTIES WILL_FAIL TRUE)
endfunction()

set(_cleanup_wire "    (void)jce_engine_try_destroy(owner);")
_jce_engine_cleanup_mutant(test_jce_engine_cleanup_old_lost_owner
    "${_cleanup_wire}"
    "${_cleanup_wire}\n    *owner = NULL; /* old: deferred bootstrap cleanup loses its host owner */"
    owned)

# Restore the state-only borrow check. The first close changes state to
# DRAINING, so a second close incorrectly reaches the sampler while init still
# borrows the Engine. WAIT_TIMEOUT retains storage for an ordinary assertion.
set(_cleanup_wire "    e->owner_state = JCE_ENGINE_DRAINING;\n    if (e->constructor_active || e->perf_hitch.iterating || e->event_dispatching) {")
_jce_engine_cleanup_mutant(test_jce_engine_cleanup_old_state_borrow
    "${_cleanup_wire}"
    "    const bool constructing = e->owner_state == JCE_ENGINE_CONSTRUCTING;\n    e->owner_state = JCE_ENGINE_DRAINING;\n    if (constructing || e->perf_hitch.iterating || e->event_dispatching) {"
    constructing)

unset(_cleanup_ownership)
unset(_cleanup_mode)
unset(_cleanup_test)
unset(_cleanup_existing)
unset(_cleanup_engine)
unset(_cleanup_source)
unset(_cleanup_wire)
unset(_cleanup_output)
unset(_cleanup_env)
