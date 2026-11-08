# Real callback owner with only its canonical capability gate disconnected.
file(READ "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_renderer_bgfx_callback.c" _legacy_rb)
set(_legacy_gate "    if (!jce_render_readback_format_supported(JCE_RENDER_FORMAT_RGBA16F)) {")
string(FIND "${_legacy_rb}" "${_legacy_gate}" _legacy_first)
string(FIND "${_legacy_rb}" "${_legacy_gate}" _legacy_last REVERSE)
if(_legacy_first LESS 0 OR NOT _legacy_first EQUAL _legacy_last)
    message(FATAL_ERROR "Legacy readback control requires one actual canonical admission gate")
endif()
string(REPLACE "${_legacy_gate}" "    if (false) {" _legacy_mutant "${_legacy_rb}")
set(_legacy_path "${CMAKE_CURRENT_BINARY_DIR}/readback-lifecycle/legacy_missing_admission.c")
unset(_legacy_previous)
if(EXISTS "${_legacy_path}")
    file(READ "${_legacy_path}" _legacy_previous)
endif()
if(NOT _legacy_previous STREQUAL _legacy_mutant)
    file(WRITE "${_legacy_path}" "${_legacy_mutant}")
endif()
jce_add_unit_test(test_jce_screenshot_missing_readback_admission KIND contract
    SOURCES test_jce_screenshot_completion.c
    LINK jce_core SDL3_image::SDL3_image bgfx::bgfx)
target_include_directories(test_jce_screenshot_missing_readback_admission PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/renderer")
target_compile_definitions(test_jce_screenshot_missing_readback_admission PRIVATE
    "JCE_SCREENSHOT_CALLBACK_OWNER=\"${_legacy_path}\"")
if(MSVC)
    target_compile_options(test_jce_screenshot_missing_readback_admission PRIVATE /Zc:preprocessor)
endif()
set_tests_properties(test_jce_screenshot_missing_readback_admission PROPERTIES WILL_FAIL TRUE)
