# Exact collector and existing model-publication/ecull owners; no GPU/window.
if(NOT Python3_EXECUTABLE)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
endif()
set(_collect_layout_build "${CMAKE_CURRENT_BINARY_DIR}/collect-layout-controls")
set(_collect_layout_runner "${CMAKE_SOURCE_DIR}/tools/validation/collect_layout_controls.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_collect_layout_runner}"
    "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_scene_renderer.c"
    "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_sr_model_bounds.c"
    "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_sr_ecull.c"
    "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_sr_metadata_key.h")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${_collect_layout_runner}"
    --root "${CMAKE_SOURCE_DIR}" --build-dir "${_collect_layout_build}" --prepare-only
    RESULT_VARIABLE _collect_layout_result OUTPUT_QUIET ERROR_VARIABLE _collect_layout_error)
if(NOT _collect_layout_result EQUAL 0)
    message(FATAL_ERROR "Collector owner controls preparation failed: ${_collect_layout_error}")
endif()
foreach(_collect_layout_variant IN ITEMS current old_unconditional_generation
        missing_bounds_guard missing_count_guard missing_order_guard cache_partial_collection
        old_default_note old_default_entry old_default_reset missing_default_drain
        old_default_focus_hit old_invalidation_gate
        missing_cold_bounds_guard missing_all_proof_guard)
    set(_collect_layout_target "test_jce_collect_layout_${_collect_layout_variant}")
    if(_collect_layout_variant STREQUAL "current")
        set(_collect_layout_kind unit)
    else()
        set(_collect_layout_kind contract)
    endif()
    jce_add_unit_test(${_collect_layout_target} KIND ${_collect_layout_kind}
        SOURCES test_jce_collect_layout.c
        INCLUDES "${_collect_layout_build}/native/${_collect_layout_variant}")
    if(MSVC)
        target_compile_options(${_collect_layout_target} PRIVATE /Zc:preprocessor)
    endif()
    if(NOT _collect_layout_variant STREQUAL "current")
        set_tests_properties(${_collect_layout_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
foreach(_collect_layout_bypass IN ITEMS collect-off metadata-off)
    set(_collect_layout_test "test_jce_collect_layout_${_collect_layout_bypass}")
    add_test(NAME ${_collect_layout_test}
        COMMAND test_jce_collect_layout_current "${_collect_layout_bypass}")
    set_tests_properties(${_collect_layout_test} PROPERTIES
        LABELS "unit;scene;cpu" TIMEOUT 60 ENVIRONMENT "${JCE_TEST_ENVIRONMENT}")
endforeach()
foreach(_collect_layout_variable IN ITEMS build runner result error variant target kind bypass test)
    unset(_collect_layout_${_collect_layout_variable})
endforeach()
unset(_collect_layout_variable)
