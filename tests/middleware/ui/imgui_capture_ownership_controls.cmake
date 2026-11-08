# Include after the existing ImGui clip owner extraction helper.
function(_jce_imgui_capture_ownership_controls)
    set(_owner_path "${CMAKE_SOURCE_DIR}/engine/src/middleware/ui/jce_imgui_renderer.cpp")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_owner_path}")
    file(READ "${_owner_path}" _imgui_clip_owner)
    _jce_imgui_clip_extract(_state
        "static struct {\n    bgfx_program_handle_t"
        "/* ImGui 1.92 stores a bgfx texture handle idx")
    _jce_imgui_clip_extract(_lifecycle
        "void jce_imgui_renderer_shutdown(void)"
        "struct ImGuiDrawObservationState {")

    # The negative removes only the two real shutdown release calls.
    set(_wires "    release_capture_target(s_cap, s_ctx.view_id);\n    release_capture_target(s_rec, (uint16_t)(s_ctx.view_id + 1));")
    string(FIND "${_lifecycle}" "${_wires}" _first)
    string(FIND "${_lifecycle}" "${_wires}" _last REVERSE)
    if(_first LESS 0 OR NOT _first EQUAL _last)
        message(FATAL_ERROR "ImGui capture control requires one exact shutdown release pair")
    endif()
    string(REPLACE "${_wires}" "    (void)0;" _missing_release "${_lifecycle}")

    foreach(_variant IN ITEMS current missing_release)
        if(_variant STREQUAL "current")
            set(_body "${_lifecycle}")
            set(_name test_jce_imgui_capture_ownership)
            set(_kind unit)
        else()
            set(_body "${_missing_release}")
            set(_name test_jce_imgui_capture_ownership_missing_release)
            set(_kind contract)
        endif()
        set(_generated "${CMAKE_CURRENT_BINARY_DIR}/${_name}_owner.inc.h")
        file(CONFIGURE OUTPUT "${_generated}" CONTENT "${_state}\n${_body}" @ONLY NEWLINE_STYLE UNIX)
        jce_add_cxx_test(${_name} KIND ${_kind}
            SOURCES test_jce_imgui_capture_ownership.cpp LINK jce_core)
        target_compile_features(${_name} PRIVATE cxx_std_20)
        target_compile_definitions(${_name} PRIVATE JCE_IMGUI_CAPTURE_OWNER="${_generated}")
        if(MSVC)
            target_compile_options(${_name} PRIVATE /Zc:preprocessor)
        endif()
        if(_kind STREQUAL "contract")
            set_tests_properties(${_name} PROPERTIES WILL_FAIL TRUE)
        endif()
    endforeach()
endfunction()

_jce_imgui_capture_ownership_controls()
