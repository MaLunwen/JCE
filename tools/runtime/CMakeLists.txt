cmake_minimum_required(VERSION 3.20)
project(JCEStockRuntime LANGUAGES C CXX)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Stock runtime currently supports Windows x64 only")
endif()
if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    message(FATAL_ERROR "Stock runtime publication requires Release")
endif()
set(JCE_BUILD_VARIANT dist)
set(JCE_DELIVERY_PROFILE embedded)
find_package(JCE CONFIG REQUIRED)
# The producer verifies these actual bytes before configuring. Changing SDK or
# first-party source identity changes every real compile command in this cache.
string(LENGTH "${JCE_STOCK_INPUT_SHA256}" _stock_identity_length)
if(NOT _stock_identity_length EQUAL 64 OR NOT JCE_STOCK_INPUT_SHA256 MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "Use stock-app build-runtime with a verified SDK/source identity")
endif()
add_compile_definitions(JCE_STOCK_INPUT_SHA256="${JCE_STOCK_INPUT_SHA256}")
add_executable(jce_stock_runtime WIN32 main.c stock_payload.c stock_bundle.c)
target_link_libraries(jce_stock_runtime PRIVATE JCE::JCE)
if(NOT COMMAND jce_script_enable)
    message(FATAL_ERROR "Selected SDK lacks the public language registration helper")
endif()
jce_script_enable(jce_stock_runtime LANGUAGES lua js REQUIRED NO_STAGE)
target_compile_definitions(jce_stock_runtime PRIVATE SDL_MAIN_USE_CALLBACKS=1)
# Use the SDK's existing resource and pack owner, including fallback font/UI.
# Notices are real embedded assets, not a sidecar required by the application.
if(NOT JCE_THIRD_PARTY_LICENSES_FILE OR NOT EXISTS "${JCE_THIRD_PARTY_LICENSES_FILE}")
    message(FATAL_ERROR "Selected SDK lacks third-party notices")
endif()
set(_stock_assets "${CMAKE_CURRENT_BINARY_DIR}/stock_resources")
file(MAKE_DIRECTORY "${_stock_assets}/licenses")
configure_file("${JCE_THIRD_PARTY_LICENSES_FILE}"
    "${_stock_assets}/licenses/THIRD_PARTY_LICENSES.md" COPYONLY)
# First-party producer input, never a runtime filesystem fallback. An explicit
# override remains available until the SDK installs its own LICENSE roster.
if(NOT DEFINED JCE_STOCK_LICENSE_FILE)
    get_filename_component(JCE_STOCK_LICENSE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../../LICENSE" ABSOLUTE)
endif()
if(NOT JCE_STOCK_LICENSE_FILE OR NOT EXISTS "${JCE_STOCK_LICENSE_FILE}")
    message(FATAL_ERROR "JCE_STOCK_LICENSE_FILE must name JCE's actual LICENSE")
endif()
configure_file("${JCE_STOCK_LICENSE_FILE}"
    "${_stock_assets}/licenses/LICENSE" COPYONLY)
jce_target_embed_pak(jce_stock_runtime RESOURCE_DIRS "${_stock_assets}")

# Reuse the standalone notice verifier's exact resource identifiers. Loading
# these resources for publication never executes the application.
enable_language(RC)
file(TO_CMAKE_PATH "${JCE_STOCK_LICENSE_FILE}" _stock_license_rc)
file(TO_CMAKE_PATH "${JCE_THIRD_PARTY_LICENSES_FILE}" _stock_third_rc)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/stock_notices.rc"
    CONTENT "JCE_LICENSE RCDATA \"${_stock_license_rc}\"\nJCE_THIRD_PARTY_LICENSES RCDATA \"${_stock_third_rc}\"\n")
target_sources(jce_stock_runtime PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/stock_notices.rc")

# The explicit stock-app build-runtime verification target compiles and runs
# these CPU controls in the same consumer cache. Ordinary stock builds omit them.
enable_testing()
# Public SDK CPU-main lane: the same archive and dependencies, without pulling
# the SDL application entry into conventional CPU test executables.
if(NOT TARGET JCE::Core)
    message(FATAL_ERROR "Selected SDK lacks the public JCE::Core CPU consumer target")
endif()
add_executable(jce_stock_payload_controls EXCLUDE_FROM_ALL tests/test_stock_payload.c)
target_link_libraries(jce_stock_payload_controls PRIVATE JCE::Core)
add_executable(jce_stock_bundle_controls EXCLUDE_FROM_ALL tests/test_stock_bundle.c stock_bundle.c)
target_link_libraries(jce_stock_bundle_controls PRIVATE JCE::Core)
add_executable(jce_stock_policy_controls EXCLUDE_FROM_ALL tests/test_stock_policy.c stock_bundle.c)
target_link_libraries(jce_stock_policy_controls PRIVATE JCE::Core)
add_test(NAME jce_stock_policy_controls COMMAND jce_stock_policy_controls)
add_test(NAME jce_stock_payload_controls COMMAND jce_stock_payload_controls)
add_test(NAME jce_stock_bundle_controls COMMAND jce_stock_bundle_controls)
include(tests/stock_runtime_controls.cmake)
add_custom_target(jce_stock_runtime_verify
    COMMAND "${CMAKE_CTEST_COMMAND}" --output-on-failure --no-tests=error
        -R "^jce_stock_(payload_controls|bundle_controls|policy_controls|payload_old|payload_old_secure|payload_old_unique|policy_old_overlay|policy_old_scene)$"
    DEPENDS jce_stock_runtime jce_stock_payload_controls jce_stock_bundle_controls
        jce_stock_policy_controls jce_stock_payload_old
        jce_stock_payload_old_secure jce_stock_payload_old_unique
        jce_stock_policy_old_overlay jce_stock_policy_old_scene
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    USES_TERMINAL
    VERBATIM)
