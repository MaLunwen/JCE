# Complete header controls with counted public dependency ports. No GPU/window.
if(NOT Python3_EXECUTABLE)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
endif()
set(_rml_batch_build "${CMAKE_CURRENT_BINARY_DIR}/rml-scene-batch-controls")
set(_rml_batch_runner "${CMAKE_SOURCE_DIR}/tools/tests/test_rml_scene_batch.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_rml_batch_runner}"
    "${CMAKE_SOURCE_DIR}/engine/include/jce/application/jce_rml_scene_overlay.h"
    "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_scene_tags_layers.c")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${_rml_batch_runner}"
    --source-root "${CMAKE_SOURCE_DIR}" --build-dir "${_rml_batch_build}" --prepare-only
    RESULT_VARIABLE _rml_batch_result OUTPUT_QUIET ERROR_VARIABLE _rml_batch_error)
if(NOT _rml_batch_result EQUAL 0)
    message(FATAL_ERROR "Rml header controls preparation failed: ${_rml_batch_error}")
endif()
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/test_jce_rml_scene_batch.c"
    "${_rml_batch_build}/test_jce_rml_scene_batch_cpp.cpp" COPYONLY)
foreach(_rml_batch_variant IN ITEMS current per_binding)
    foreach(_rml_batch_language IN ITEMS c cpp)
        set(_rml_batch_target "test_jce_rml_scene_batch_${_rml_batch_language}_${_rml_batch_variant}")
        if(_rml_batch_language STREQUAL "c")
            add_executable(${_rml_batch_target} "${CMAKE_CURRENT_SOURCE_DIR}/test_jce_rml_scene_batch.c")
            target_compile_features(${_rml_batch_target} PRIVATE c_std_99)
        else()
            add_executable(${_rml_batch_target} "${_rml_batch_build}/test_jce_rml_scene_batch_cpp.cpp")
            target_compile_features(${_rml_batch_target} PRIVATE cxx_std_20)
        endif()
        set_target_properties(${_rml_batch_target} PROPERTIES C_EXTENSIONS OFF CXX_EXTENSIONS OFF)
        target_include_directories(${_rml_batch_target} BEFORE PRIVATE
            "${_rml_batch_build}/${_rml_batch_variant}/ports" "${CMAKE_CURRENT_SOURCE_DIR}")
        if(_rml_batch_variant STREQUAL "current")
            add_test(NAME ${_rml_batch_target} COMMAND ${_rml_batch_target})
            set_tests_properties(${_rml_batch_target} PROPERTIES LABELS "unit;application;cpu")
        else()
            add_test(NAME ${_rml_batch_target} COMMAND ${_rml_batch_target} --budget-only)
            set_tests_properties(${_rml_batch_target} PROPERTIES WILL_FAIL TRUE LABELS "contract;application;cpu")
        endif()
    endforeach()
endforeach()
foreach(_rml_batch_variable IN ITEMS build runner result error variant language target)
    unset(_rml_batch_${_rml_batch_variable})
endforeach()
unset(_rml_batch_variable)
