# Installed-SDK consumers and evidence controls. No engine target is linked here.
jce_register_test_directory()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(JCE_TEST_SDK_DIR "" CACHE PATH "Explicit installed SDK for consumer integration gates (empty: SKIP)")
set(JCE_TEST_SDK_ARCH "${CMAKE_SYSTEM_PROCESSOR}" CACHE STRING "Installed SDK target arch for consumer gates")
set(JCE_TEST_SDK_VARIANT "release" CACHE STRING "Installed SDK variant: release or dist (Release config)")
set_property(CACHE JCE_TEST_SDK_VARIANT PROPERTY STRINGS release dist)
set(JCE_TEST_SDK_DELIVERY "authoring" CACHE STRING "Installed SDK delivery: authoring or embedded")
set_property(CACHE JCE_TEST_SDK_DELIVERY PROPERTY STRINGS authoring embedded)

if(NOT JCE_TEST_SDK_DIR)
    jce_test_suite_skipped("sdk-consumer-c99" "JCE_TEST_SDK_DIR is not explicitly configured, CTest reports SKIP")
    jce_test_suite_skipped("sdk-consumer-scripting" "JCE_TEST_SDK_DIR is not explicitly configured, CTest reports SKIP")
endif()

foreach(_consumer c99 scripting)
    set(_test "test_jce_sdk_consumer_${_consumer}")
    add_test(NAME "${_test}"
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/sdk_smoke_gate.py"
            "--consumer=${_consumer}" "--sdk=${JCE_TEST_SDK_DIR}"
            "--arch=${JCE_TEST_SDK_ARCH}" "--variant=${JCE_TEST_SDK_VARIANT}"
            "--delivery-profile=${JCE_TEST_SDK_DELIVERY}"
            "--build-dir=${CMAKE_CURRENT_BINARY_DIR}/sdk/${_consumer}")
    set_tests_properties("${_test}" PROPERTIES
        TIMEOUT 600 SKIP_RETURN_CODE 77
        RESOURCE_LOCK jce_installed_sdk_consumer
        ENVIRONMENT "${JCE_TEST_ENVIRONMENT}"
        LABELS "integration;installed-sdk;noop")
    jce_set_test_kind("${_test}" integration)
    set_property(GLOBAL APPEND PROPERTY JCE_TESTS_WITH_ENV "${_test}")
endforeach()

# CPU evidence admission is distinct from native/GPU consumer execution.
foreach(_lab_control validate prepare_inputs)
    set(_lab_test "test_jce_render_liveness_${_lab_control}")
    add_test(NAME "${_lab_test}"
        COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/render_liveness_lab/tools/test_${_lab_control}.py")
    set_tests_properties("${_lab_test}" PROPERTIES
        TIMEOUT 60 ENVIRONMENT "${JCE_TEST_ENVIRONMENT}"
        LABELS "consumer-evidence;cpu")
    jce_set_test_kind("${_lab_test}" unit)
    set_property(GLOBAL APPEND PROPERTY JCE_TESTS_WITH_ENV "${_lab_test}")
endforeach()
