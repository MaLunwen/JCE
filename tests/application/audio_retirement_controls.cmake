# Complete Engine TU, actual callback gate, no window or physical audio device.
set(_engine_audio_source "${CMAKE_SOURCE_DIR}/engine/src/application/jce_engine.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_engine_audio_source}")
file(READ "${_engine_audio_source}" _engine_audio_text)
function(_jce_engine_audio_control NAME SOURCE KIND)
    jce_add_unit_test(${NAME} KIND ${KIND}
        SOURCES test_jce_engine_audio_retirement.c
        LINK JCE sdl::sdl bgfx::bgfx
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/application")
    target_compile_definitions(${NAME} PRIVATE JCE_BUILDING_ENGINE=1
        JCE_TEST_AUDIO_RETIREMENT_ENGINE="${SOURCE}")
    if(MSVC)
        target_compile_options(${NAME} PRIVATE /Zc:preprocessor)
    endif()
    set_tests_properties(${NAME} PROPERTIES RUN_SERIAL TRUE
        RESOURCE_LOCK jce_renderer_context TIMEOUT 60
        ENVIRONMENT "${JCE_TEST_ENVIRONMENT};SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;JCE_HEADLESS=1;JCE_BACKEND=noop;JCE_TRACE=1;JCE_TRACE_STREAM=;JCE_TRACE_STREAM_MODE=;JCE_TRACE_EXPORT=;JCE_PERF_LOG=0;JCE_MAX_FRAMES=;JCE_MAX_SECONDS=;JCE_KPI_FRAME_LOG=;JCE_KPI_ASSET_LOG=;JCE_SAMPLER=;JCE_KPI_FRAME_COUNT=0;JCE_LAUNCH_VSYNC=0;JCE_LAUNCH_TARGET_FPS=0")
    if(KIND STREQUAL "contract")
        set_tests_properties(${NAME} PROPERTIES WILL_FAIL TRUE)
    endif()
endfunction()
_jce_engine_audio_control(test_jce_engine_audio_retirement "${_engine_audio_source}" integration)
function(_jce_engine_audio_mutant NAME WIRE REPLACEMENT)
    string(FIND "${_engine_audio_text}" "${WIRE}" _first)
    string(FIND "${_engine_audio_text}" "${WIRE}" _last REVERSE)
    if(_first LESS 0 OR NOT _first EQUAL _last)
        message(FATAL_ERROR "${NAME} requires one actual Engine audio wire")
    endif()
    string(REPLACE "${WIRE}" "${REPLACEMENT}" _mutant "${_engine_audio_text}")
    set(_path "${CMAKE_CURRENT_BINARY_DIR}/engine-audio/${NAME}.c")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/engine-audio")
    jce_test_write_if_changed("${_path}" "${_mutant}")
    _jce_engine_audio_control(${NAME} "${_path}" contract)
endfunction()
_jce_engine_audio_mutant(test_jce_engine_audio_missing_retirement
    "    if (!engine_audio_master_retire())\n        return engine_cleanup_defer(e, cleanup_trace, 6,\n            \"engine destroy deferred while audio master callbacks remain; retry after callback\");"
    "    /* old: irreversible app exit before shared audio borrowers retire */")
_jce_engine_audio_mutant(test_jce_engine_audio_missing_session_close
    "    jce_audio_master_session_close_internal();"
    "    /* old: draining callbacks can reopen the shared audio master */")
# Mutate only the actual private adapter, then include it from the complete
# Engine owner. Missing/hardcoded device observations must fail ordinary checks.
set(_engine_audio_adapter "${CMAKE_SOURCE_DIR}/engine/src/application/jce_engine_audio_retirement.inc.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_engine_audio_adapter}")
file(READ "${_engine_audio_adapter}" _engine_audio_adapter_text)
function(_jce_engine_audio_observation_mutant NAME WIRE REPLACEMENT)
    string(FIND "${_engine_audio_adapter_text}" "${WIRE}" _first)
    string(FIND "${_engine_audio_adapter_text}" "${WIRE}" _last REVERSE)
    if(_first LESS 0 OR NOT _first EQUAL _last)
        message(FATAL_ERROR "${NAME} requires one actual Engine audio observation wire")
    endif()
    string(REPLACE "${WIRE}" "${REPLACEMENT}" _mutant "${_engine_audio_adapter_text}")
    set(_adapter_path "${CMAKE_CURRENT_BINARY_DIR}/engine-audio/${NAME}.inc.h")
    jce_test_write_if_changed("${_adapter_path}" "${_mutant}")
    _jce_engine_audio_mutant(${NAME}
        "#include \"jce_engine_audio_retirement.inc.h\""
        "#include \"${_adapter_path}\"")
endfunction()
_jce_engine_audio_observation_mutant(test_jce_engine_audio_missing_device_observation
    "        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_CORE,\n            \"audio_master.had_device\", jce_audio_master_device_live_internal(), scope.id, scope.id);"
    "        (void)jce_audio_master_device_live_internal(); /* missing actual counter */")
_jce_engine_audio_observation_mutant(test_jce_engine_audio_false_no_device
    "\"audio_master.had_device\", jce_audio_master_device_live_internal(), scope.id, scope.id"
    "\"audio_master.had_device\", false, scope.id, scope.id")
unset(_engine_audio_adapter)
unset(_engine_audio_adapter_text)
unset(_engine_audio_source)
unset(_engine_audio_text)
