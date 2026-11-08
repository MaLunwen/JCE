# Actual shared-master owner with typed native device/clock ports, no sound device.
set(_audio_master_source "${CMAKE_SOURCE_DIR}/engine/src/middleware/audio/jce_audio_master.inc.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_audio_master_source}")
file(READ "${_audio_master_source}" _audio_master_text)
function(_jce_audio_master_control NAME SOURCE KIND)
    jce_add_unit_test(${NAME} KIND ${KIND}
        SOURCES test_jce_audio_master_lifecycle.c
        LINK jce_core sdl::sdl miniaudio::miniaudio
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src"
            "${CMAKE_SOURCE_DIR}/engine/src/middleware/audio")
    target_compile_definitions(${NAME} PRIVATE JCE_BUILDING_ENGINE=1
        JCE_TEST_AUDIO_MASTER_SOURCE="${SOURCE}")
    if(KIND STREQUAL "contract")
        set_tests_properties(${NAME} PROPERTIES WILL_FAIL TRUE)
    endif()
endfunction()
_jce_audio_master_control(test_jce_audio_master_lifecycle "${_audio_master_source}" unit)
_jce_audio_master_control(test_jce_audio_master_unmarked_owner "${_audio_master_source}" unit)
target_compile_definitions(test_jce_audio_master_unmarked_owner PRIVATE JCE_TEST_AUDIO_UNMARKED_OWNER=1)
function(_jce_audio_master_mutant NAME WIRE REPLACEMENT)
    string(FIND "${_audio_master_text}" "${WIRE}" _first)
    string(FIND "${_audio_master_text}" "${WIRE}" _last REVERSE)
    if(_first LESS 0 OR NOT _first EQUAL _last)
        message(FATAL_ERROR "${NAME} requires one actual master owner wire")
    endif()
    string(REPLACE "${WIRE}" "${REPLACEMENT}" _mutant "${_audio_master_text}")
    set(_path "${CMAKE_CURRENT_BINARY_DIR}/audio-master/${NAME}.inc.h")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/audio-master")
    jce_test_write_if_changed("${_path}" "${_mutant}")
    _jce_audio_master_control(${NAME} "${_path}" contract)
endfunction()
_jce_audio_master_mutant(test_jce_audio_master_old_timeout_release
    "    return jce_audio_master_gate_wait(&s_master.gate, UINT32_MAX);"
    "    (void)jce_audio_master_gate_wait(&s_master.gate, 2000); return true; /* old unsafe timeout */")
_jce_audio_master_mutant(test_jce_audio_master_old_tap_replace
    "    if (!master_wait_quiescent()) return false;\n    s_master.tap_fn = fn;"
    "    s_master.tap_fn = fn; /* old: overwrite borrowed callback fields */")
_jce_audio_master_mutant(test_jce_audio_master_old_reopen
    "    if (!master_control_allowed() || s_master.session_closed || s_master.retiring)\n        return false;\n    if (s_master.device_inited) return true;"
    "    if (!master_control_allowed()) return false;\n    if (s_master.device_inited) return true; /* old lazy reopen */")

_jce_audio_master_mutant(test_jce_audio_master_old_silent_gate_oom
    "    if (!jce_audio_master_gate_create(&s_master.gate)) {\n        s_master.device_failed = true;\n        LOG_ERROR(\"jce_audio\", \"master callback admission allocation failed; engines remain silent until the next session\");\n        return false;\n    }"
    "    if (!jce_audio_master_gate_create(&s_master.gate)) return false; /* old unreported retrying allocation failure */")

# These are the actual new disabled definitions only. The historical complete
# JCE_NO_AUDIO facade still has unrelated defects and is not certified by this.
jce_add_unit_test(test_jce_audio_master_noaudio_hooks
    SOURCES test_jce_audio_master_noaudio_hooks.c
    LINK jce_core INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")
target_compile_definitions(test_jce_audio_master_noaudio_hooks PRIVATE
    JCE_BUILDING_ENGINE=1 JCE_NO_AUDIO=1)
unset(_audio_master_source)
unset(_audio_master_text)

# Complete actual public offline lifecycle, real native worker and real graph.
if(EMSCRIPTEN)
    message(STATUS "Post-mark offline native worker control unavailable on Web")
else()
    set(_audio_offline_source "${CMAKE_SOURCE_DIR}/engine/src/middleware/audio/jce_audio.c")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_audio_offline_source}")
    file(READ "${_audio_offline_source}" _audio_offline_text)
    function(_jce_audio_offline_control NAME SOURCE KIND)
        jce_add_unit_test(${NAME} KIND ${KIND}
            SOURCES test_jce_audio_offline_owner_thread.c
            LINK jce_audio jce_resource sdl::sdl miniaudio::miniaudio
            INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/middleware/audio")
        target_compile_definitions(${NAME} PRIVATE JCE_BUILDING_ENGINE=1
            JCE_TEST_AUDIO_OFFLINE_SOURCE="${SOURCE}")
        if(KIND STREQUAL "contract")
            set_tests_properties(${NAME} PROPERTIES WILL_FAIL TRUE)
        endif()
    endfunction()
    _jce_audio_offline_control(test_jce_audio_offline_owner_thread "${_audio_offline_source}" unit)
    function(_jce_audio_offline_mutant NAME WIRE REPLACEMENT)
        string(FIND "${_audio_offline_text}" "${WIRE}" _first)
        string(FIND "${_audio_offline_text}" "${WIRE}" _last REVERSE)
        if(_first LESS 0 OR NOT _first EQUAL _last)
            message(FATAL_ERROR "${NAME} requires one actual offline owner wire")
        endif()
        string(REPLACE "${WIRE}" "${REPLACEMENT}" _mutant "${_audio_offline_text}")
        set(_path "${CMAKE_CURRENT_BINARY_DIR}/audio-master/${NAME}.c")
        jce_test_write_if_changed("${_path}" "${_mutant}")
        _jce_audio_offline_control(${NAME} "${_path}" contract)
    endfunction()
    _jce_audio_offline_mutant(test_jce_audio_offline_old_main_create
        "    if (!offline && (!master_control_allowed() || s_master.session_closed)) return NULL;"
        "    if (!jce_thread_is_main() || !master_control_allowed() || (!offline && s_master.session_closed)) return NULL;")
    _jce_audio_offline_mutant(test_jce_audio_offline_old_main_destroy
        "    if (!audio->offline && !master_unregister(audio)) return;"
        "    if (!jce_thread_is_main() || !master_unregister(audio)) return;")
    unset(_audio_offline_source)
    unset(_audio_offline_text)
endif()
