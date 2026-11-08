# Complete Engine owner with real default workers and real teardown callbacks.
set(_async_retire_engine "${CMAKE_SOURCE_DIR}/engine/src/application/jce_engine.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_async_retire_engine}")
file(READ "${_async_retire_engine}" _async_retire_source)
set(_async_retire_wire "    jce_async_default_session_close_internal();")
string(FIND "${_async_retire_source}" "${_async_retire_wire}" _async_retire_first)
string(FIND "${_async_retire_source}" "${_async_retire_wire}" _async_retire_last REVERSE)
if(_async_retire_first LESS 0 OR NOT _async_retire_first EQUAL _async_retire_last)
    message(FATAL_ERROR "Async Engine admission control requires one actual close wire")
endif()
string(REPLACE "${_async_retire_wire}"
    "    /* old: teardown callbacks can lazily recreate the default executor */"
    _async_retire_old "${_async_retire_source}")
set(_async_retire_mutant "${CMAKE_CURRENT_BINARY_DIR}/async-retirement/old_default_reopen.c")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/async-retirement")
jce_test_write_if_changed("${_async_retire_mutant}" "${_async_retire_old}")
set(_async_retire_shared_wire "    jce_thread_pool_session_close_internal();")
string(FIND "${_async_retire_source}" "${_async_retire_shared_wire}" _async_retire_first)
string(FIND "${_async_retire_source}" "${_async_retire_shared_wire}" _async_retire_last REVERSE)
if(_async_retire_first LESS 0 OR NOT _async_retire_first EQUAL _async_retire_last)
    message(FATAL_ERROR "Shared-pool Engine admission control requires one actual close wire")
endif()
string(REPLACE "${_async_retire_shared_wire}"
    "    /* old: teardown callbacks can recreate the joined shared frame pool */"
    _async_retire_shared_old "${_async_retire_source}")
set(_async_retire_shared_mutant "${CMAKE_CURRENT_BINARY_DIR}/async-retirement/old_shared_reopen.c")
jce_test_write_if_changed("${_async_retire_shared_mutant}" "${_async_retire_shared_old}")
foreach(_async_retire_variant IN ITEMS current old_default_reopen old_shared_reopen)
    set(_async_retire_name test_jce_engine_async_retirement)
    set(_async_retire_kind integration)
    set(_async_retire_selected "${_async_retire_engine}")
    if(_async_retire_variant STREQUAL "old_default_reopen")
        string(APPEND _async_retire_name "_old_default_reopen")
        set(_async_retire_kind contract)
        set(_async_retire_selected "${_async_retire_mutant}")
    elseif(_async_retire_variant STREQUAL "old_shared_reopen")
        string(APPEND _async_retire_name "_old_shared_reopen")
        set(_async_retire_kind contract)
        set(_async_retire_selected "${_async_retire_shared_mutant}")
    endif()
    jce_add_unit_test(${_async_retire_name} KIND ${_async_retire_kind}
        SOURCES test_jce_engine_async_retirement.c
        LINK JCE sdl::sdl bgfx::bgfx
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src"
            "${CMAKE_SOURCE_DIR}/engine/src/application"
            "${CMAKE_SOURCE_DIR}/tests/os/core/stubs")
    target_compile_definitions(${_async_retire_name} PRIVATE JCE_BUILDING_ENGINE=1
        JCE_TEST_ASYNC_RETIREMENT_ENGINE="${_async_retire_selected}")
    if(MSVC)
        target_compile_options(${_async_retire_name} PRIVATE /Zc:preprocessor)
    endif()
    set_tests_properties(${_async_retire_name} PROPERTIES
        ENVIRONMENT "${JCE_TEST_ENVIRONMENT};SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;JCE_HEADLESS=1;JCE_BACKEND=noop;JCE_TRACE=1;JCE_TRACE_STREAM=;JCE_TRACE_EXPORT=;JCE_PERF_LOG=0;JCE_MAX_FRAMES=;JCE_MAX_SECONDS=;JCE_KPI_FRAME_LOG=;JCE_KPI_ASSET_LOG=;JCE_SAMPLER=;JCE_KPI_FRAME_COUNT=0;JCE_LAUNCH_VSYNC=0;JCE_LAUNCH_TARGET_FPS=0")
    if(_async_retire_kind STREQUAL "contract")
        set_tests_properties(${_async_retire_name} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
foreach(_async_retire_key IN ITEMS engine source wire first last old mutant
        variant name kind selected shared_wire shared_old shared_mutant)
    unset(_async_retire_${_async_retire_key})
endforeach()
unset(_async_retire_key)
