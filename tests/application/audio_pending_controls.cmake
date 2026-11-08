# Actual complete async spawn/poll/control owners; real Core worker/barriers.
function(_jce_audio_pending_extract SOURCE BEGIN END OUT)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SOURCE}")
    file(READ "${SOURCE}" _text)
    string(FIND "${_text}" "${BEGIN}" _begin)
    string(FIND "${_text}" "${BEGIN}" _begin_last REVERSE)
    if("${END}" STREQUAL "EOF")
        string(LENGTH "${_text}" _end)
        set(_end_last ${_end})
    else()
        string(FIND "${_text}" "${END}" _end)
        string(FIND "${_text}" "${END}" _end_last REVERSE)
    endif()
    if(_begin LESS 0 OR NOT _begin EQUAL _begin_last OR
       _end LESS_EQUAL _begin OR NOT _end EQUAL _end_last)
        message(FATAL_ERROR "Audio pending controls require unique ordered complete-owner anchors")
    endif()
    math(EXPR _length "${_end} - ${_begin}")
    string(SUBSTRING "${_text}" ${_begin} ${_length} _part)
    set(${OUT} "${_part}" PARENT_SCOPE)
endfunction()
set(_audio_pending_source "${CMAKE_SOURCE_DIR}/engine/src/application/jce_rt_audio.c")
_jce_audio_pending_extract("${_audio_pending_source}"
    "static JceAsyncRunResult rt_audio_decode_run(" "/* ── Adaptive music director" _audio_pending_spawn)
_jce_audio_pending_extract("${_audio_pending_source}"
    "void rt_audio_poll(" "EOF" _audio_pending_controls)
_jce_audio_pending_extract("${CMAKE_SOURCE_DIR}/engine/src/application/jce_runtime.c"
    "\t/* Cancel in-flight structured audio decodes and drop their results. */"
    "\tif (rt->occ_tracker) {" _audio_pending_close)
set(_audio_pending_path "${CMAKE_CURRENT_BINARY_DIR}/audio-pending/current.inc.h")
set(_audio_pending_close_path "${CMAKE_CURRENT_BINARY_DIR}/audio-pending/close.inc.h")
set(_audio_pending_current "${_audio_pending_spawn}\n${_audio_pending_controls}")
jce_test_write_if_changed("${_audio_pending_path}" "${_audio_pending_current}")
jce_test_write_if_changed("${_audio_pending_close_path}" "${_audio_pending_close}")
function(_jce_audio_pending_target NAME OWNER KIND)
    jce_add_unit_test(${NAME} KIND ${KIND}
        SOURCES test_jce_runtime_audio_pending.c LINK jce_core
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")
    target_compile_definitions(${NAME} PRIVATE
        JCE_TEST_AUDIO_PENDING_OWNER="${OWNER}"
        JCE_TEST_AUDIO_PENDING_CLOSE="${_audio_pending_close_path}")
    set_tests_properties(${NAME} PROPERTIES TIMEOUT 30)
    if(KIND STREQUAL "contract")
        set_tests_properties(${NAME} PROPERTIES WILL_FAIL TRUE)
    endif()
endfunction()
_jce_audio_pending_target(test_jce_runtime_audio_pending "${_audio_pending_path}" unit)
function(_jce_audio_pending_mutant SUFFIX WIRE REPLACEMENT)
    string(FIND "${_audio_pending_current}" "${WIRE}" _first)
    string(FIND "${_audio_pending_current}" "${WIRE}" _last REVERSE)
    if(_first LESS 0 OR NOT _first EQUAL _last)
        message(FATAL_ERROR "Audio pending ${SUFFIX} requires one actual owner wire")
    endif()
    string(REPLACE "${WIRE}" "${REPLACEMENT}" _mutant "${_audio_pending_current}")
    set(_path "${CMAKE_CURRENT_BINARY_DIR}/audio-pending/${SUFFIX}.inc.h")
    jce_test_write_if_changed("${_path}" "${_mutant}")
    _jce_audio_pending_target(test_jce_runtime_audio_pending_${SUFFIX} "${_path}" contract)
endfunction()
_jce_audio_pending_mutant(old_stop "        if (p->entity == e) {"
    "        if (false && p->entity == e) {")
_jce_audio_pending_mutant(old_clip "as && strcmp(as->clip_path, p->args->path) == 0 &&"
    "as &&")
_jce_audio_pending_mutant(missing_disarm "            p->entity = JCE_ENTITY_INVALID;"
    "            /* cancelled terminal results retain the stale play intent */")
foreach(_audio_pending_cleanup IN ITEMS source spawn controls close path close_path current)
    unset(_audio_pending_${_audio_pending_cleanup})
endforeach()
unset(_audio_pending_cleanup)
