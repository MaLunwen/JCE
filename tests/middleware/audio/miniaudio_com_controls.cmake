# Actual selected original miniaudio WASAPI function, typed COM ports, no device.
if(WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(_miniaudio_com_headers)
    foreach(_directory IN LISTS miniaudio_INCLUDE_DIRS)
        if(EXISTS "${_directory}/miniaudio.h")
            list(APPEND _miniaudio_com_headers "${_directory}/miniaudio.h")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES _miniaudio_com_headers)
    list(LENGTH _miniaudio_com_headers _miniaudio_com_count)
    if(NOT _miniaudio_com_count EQUAL 1)
        message(FATAL_ERROR "COM regression requires exactly one selected miniaudio header; no missing-header PASS")
    endif()
    list(GET _miniaudio_com_headers 0 _miniaudio_com_header)
    set(_miniaudio_com_extractor "${CMAKE_CURRENT_SOURCE_DIR}/extract_miniaudio_com.py")
    set(_miniaudio_com_output "${CMAKE_CURRENT_BINARY_DIR}/miniaudio-com/selected_miniaudio_com.inc.h")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${_miniaudio_com_header}" "${_miniaudio_com_extractor}")
    execute_process(COMMAND "${Python3_EXECUTABLE}" -B "${_miniaudio_com_extractor}"
        --header "${_miniaudio_com_header}" --output "${_miniaudio_com_output}"
        --manifest "${CMAKE_CURRENT_BINARY_DIR}/miniaudio-com/selected-source.json"
        RESULT_VARIABLE _miniaudio_com_result
        OUTPUT_VARIABLE _miniaudio_com_selection ERROR_VARIABLE _miniaudio_com_error)
    if(NOT _miniaudio_com_result EQUAL 0)
        message(FATAL_ERROR "Selected miniaudio COM source extraction failed: ${_miniaudio_com_error}")
    endif()
    message(STATUS "miniaudio COM regression selected: ${_miniaudio_com_selection}")
    jce_add_unit_test(test_jce_miniaudio_com KIND unit
        SOURCES test_jce_miniaudio_com.c LINK miniaudio::miniaudio
        INCLUDES "${CMAKE_CURRENT_BINARY_DIR}/miniaudio-com")
    unset(_miniaudio_com_headers)
    unset(_miniaudio_com_count)
    unset(_miniaudio_com_header)
    unset(_miniaudio_com_extractor)
    unset(_miniaudio_com_output)
    unset(_miniaudio_com_result)
    unset(_miniaudio_com_selection)
    unset(_miniaudio_com_error)
else()
    message(STATUS "miniaudio WASAPI COM native CPU regression unavailable for this target")
endif()
