# Engine document UI controls: real Noop evidence stays integration, not graphical.
jce_add_unit_test(test_jce_rml_font_bytes
    SOURCES test_jce_rml_font_bytes.c
    LINK jce_core
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src/middleware/ui")

function(_jce_rml_font_fixture NAME KIND)
    jce_add_cxx_test(${NAME}
        KIND ${KIND}
        SOURCES test_jce_rml_default_fonts.cpp
            "${CMAKE_SOURCE_DIR}/engine/src/middleware/ui/jce_rml_font_bytes.c"
        LINK jce_core jce_renderer jce_resource jce_platform RmlUi::RmlUi bgfx::bgfx
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src"
            "${CMAKE_SOURCE_DIR}/engine/src/middleware/ui")
    target_compile_features(${NAME} PRIVATE cxx_std_20)
    target_compile_definitions(${NAME} PRIVATE
        jce_host_default_font=jce_test_rml_host_default_font
        JCE_RML_TEST_STOCK_FONT="${CMAKE_SOURCE_DIR}/engine/ui/fonts/Aileron-Regular.ttf"
        JCE_RML_TEST_STOCK_UI="${CMAKE_SOURCE_DIR}/engine/ui"
        JCE_RML_TEST_OUTPUT_BASE="${CMAKE_CURRENT_BINARY_DIR}/${NAME}-fixtures")
    set_tests_properties(${NAME} PROPERTIES RUN_SERIAL TRUE RESOURCE_LOCK jce_renderer_context)
endfunction()
_jce_rml_font_fixture(test_jce_rml_default_fonts integration)

# A compiled runtime negative control removes only the actual default preload.
file(READ "${CMAKE_SOURCE_DIR}/engine/src/middleware/ui/jce_ui_rmlui.cpp" _rml_font_owner)
string(REGEX MATCHALL "b->default_font_loaded = rml_preload_default_fonts[(]b[)]"
    _rml_font_wires "${_rml_font_owner}")
list(LENGTH _rml_font_wires _rml_font_wire_count)
if(NOT _rml_font_wire_count EQUAL 1)
    message(FATAL_ERROR "Rml default-font control requires one real preload wire")
endif()
string(REPLACE "b->default_font_loaded = rml_preload_default_fonts(b);"
    "(void)&rml_preload_default_fonts;" _rml_font_mutant "${_rml_font_owner}")
if(_rml_font_mutant STREQUAL _rml_font_owner)
    message(FATAL_ERROR "Rml default-font control did not remove the real preload wire")
endif()
set(_rml_font_mutant_path "${CMAKE_CURRENT_BINARY_DIR}/jce_ui_rmlui_old_defaults.cpp")
jce_test_write_if_changed("${_rml_font_mutant_path}" "${_rml_font_mutant}")
_jce_rml_font_fixture(test_jce_rml_default_fonts_old_wire contract)
target_compile_definitions(test_jce_rml_default_fonts_old_wire PRIVATE
    JCE_RML_OWNER_SOURCE="${_rml_font_mutant_path}")
set_tests_properties(test_jce_rml_default_fonts_old_wire PROPERTIES WILL_FAIL TRUE)
unset(_rml_font_owner)
unset(_rml_font_wires)
unset(_rml_font_wire_count)
unset(_rml_font_mutant)
unset(_rml_font_mutant_path)

# Exact first-party atlas upload transaction and real bounded trace; counted
# backend ports do not certify a physical GPU upload or visible text.
jce_add_cxx_test(test_jce_imgui_texture_upload
    SOURCES test_jce_imgui_texture_upload.cpp
    LINK jce_core
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")
file(READ "${CMAKE_SOURCE_DIR}/engine/src/middleware/ui/jce_imgui_texture_upload_internal.h" _imgui_upload_owner)
string(REGEX MATCHALL "if [(]created == UINT16_MAX[)]"
    _imgui_upload_wires "${_imgui_upload_owner}")
list(LENGTH _imgui_upload_wires _imgui_upload_wire_count)
if(NOT _imgui_upload_wire_count EQUAL 1)
    message(FATAL_ERROR "ImGui upload control requires one actual invalid-create guard")
endif()
string(REPLACE "if (created == UINT16_MAX)" "if (false)"
    _imgui_upload_mutant "${_imgui_upload_owner}")
if(_imgui_upload_mutant STREQUAL _imgui_upload_owner)
    message(FATAL_ERROR "ImGui upload control did not remove the invalid-create guard")
endif()
set(_imgui_upload_mutant_path "${CMAKE_CURRENT_BINARY_DIR}/jce_imgui_texture_upload_old_wire.h")
jce_test_write_if_changed("${_imgui_upload_mutant_path}" "${_imgui_upload_mutant}")
jce_add_cxx_test(test_jce_imgui_texture_upload_old_wire
    KIND contract
    SOURCES test_jce_imgui_texture_upload.cpp
    LINK jce_core
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")
target_compile_definitions(test_jce_imgui_texture_upload_old_wire PRIVATE
    JCE_TEST_IMGUI_TEXTURE_UPLOAD_HEADER="${_imgui_upload_mutant_path}")
set_tests_properties(test_jce_imgui_texture_upload_old_wire PROPERTIES WILL_FAIL TRUE)
unset(_imgui_upload_owner)
unset(_imgui_upload_wires)
unset(_imgui_upload_wire_count)
unset(_imgui_upload_mutant)
unset(_imgui_upload_mutant_path)

# Actual first-party ImGui command owner with CPU-only backend ports.
set(_imgui_clip_owner_path "${CMAKE_SOURCE_DIR}/engine/src/middleware/ui/jce_imgui_renderer.cpp")
set(_imgui_clip_helper_path "${CMAKE_SOURCE_DIR}/engine/src/middleware/ui/jce_imgui_clip_internal.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_imgui_clip_owner_path}" "${_imgui_clip_helper_path}")
file(READ "${_imgui_clip_owner_path}" _imgui_clip_owner)
file(READ "${_imgui_clip_helper_path}" _imgui_clip_helper)
function(_jce_imgui_clip_extract OUTPUT START END)
    string(FIND "${_imgui_clip_owner}" "${START}" _first)
    string(FIND "${_imgui_clip_owner}" "${START}" _last REVERSE)
    string(FIND "${_imgui_clip_owner}" "${END}" _end)
    string(FIND "${_imgui_clip_owner}" "${END}" _end_last REVERSE)
    if(_first LESS 0 OR _end LESS 0 OR NOT _first EQUAL _last OR
       NOT _end EQUAL _end_last OR NOT _end GREATER _first)
        message(FATAL_ERROR "ImGui clip fixture requires one ordered actual ${OUTPUT} slice")
    endif()
    math(EXPR _length "${_end} - ${_first}")
    string(SUBSTRING "${_imgui_clip_owner}" ${_first} ${_length} _slice)
    set(${OUTPUT} "${_slice}" PARENT_SCOPE)
endfunction()
_jce_imgui_clip_extract(_imgui_clip_ctx
    "static struct {\n    bgfx_program_handle_t"
    "/* Ortho projection now delegates")
_jce_imgui_clip_extract(_imgui_clip_rec
    "static struct {\n    bool                       active;"
    "extern \"C\" void jce_imgui_renderer_set_recording")
_jce_imgui_clip_extract(_imgui_clip_codes
    "static const JceImGuiTextureCodes s_texture_codes = {"
    "struct ImGuiTextureGpu {")
_jce_imgui_clip_extract(_imgui_clip_draw_base
    "struct ImGuiDrawObservationState {"
    "void jce_imgui_renderer_rebuild_fonts(void)")
set(_imgui_clip_state "${_imgui_clip_ctx}\n${_imgui_clip_rec}\n${_imgui_clip_codes}")
set(_imgui_clip_extent
    "    if (!s_ctx.initialized) return;\n    s_ctx.framebuffer_width = width;\n    s_ctx.framebuffer_height = height;\n")
string(FIND "${_imgui_clip_owner}" "${_imgui_clip_extent}" _imgui_clip_first)
string(FIND "${_imgui_clip_owner}" "${_imgui_clip_extent}" _imgui_clip_last REVERSE)
if(_imgui_clip_first LESS 0 OR NOT _imgui_clip_first EQUAL _imgui_clip_last)
    message(FATAL_ERROR "ImGui clip fixture requires the one complete actual setup extent wire")
endif()
set(_imgui_clip_dir "${CMAKE_CURRENT_BINARY_DIR}/imgui-clip")
file(MAKE_DIRECTORY "${_imgui_clip_dir}")
foreach(_imgui_clip_variant IN ITEMS current old_clip_limits missing_extent
        missing_queue_observation missing_submission_counts)
    set(_imgui_clip_draw "${_imgui_clip_draw_base}")
    set(_imgui_clip_header "${_imgui_clip_helper}")
    set(_imgui_clip_extent_content "${_imgui_clip_extent}")
    if(_imgui_clip_variant STREQUAL "old_clip_limits")
        # Restore the old 65535 admission limit, retaining finite guards so the
        # behavioral negative is an ordinary assertion rather than float UB.
        foreach(_imgui_clip_axis IN ITEMS x y)
            if(_imgui_clip_axis STREQUAL "x")
                set(_imgui_clip_dimension width)
            else()
                set(_imgui_clip_dimension height)
            endif()
            set(_imgui_clip_needle
                "const double limit_${_imgui_clip_axis} = double(framebuffer_${_imgui_clip_dimension});")
            string(FIND "${_imgui_clip_header}" "${_imgui_clip_needle}" _imgui_clip_first)
            string(FIND "${_imgui_clip_header}" "${_imgui_clip_needle}" _imgui_clip_last REVERSE)
            if(_imgui_clip_first LESS 0 OR NOT _imgui_clip_first EQUAL _imgui_clip_last)
                message(FATAL_ERROR "ImGui old clip limits control requires one actual ${_imgui_clip_axis} limit")
            endif()
            string(REPLACE "${_imgui_clip_needle}"
                "const double limit_${_imgui_clip_axis} = 65535.0;"
                _imgui_clip_header "${_imgui_clip_header}")
        endforeach()
    elseif(_imgui_clip_variant STREQUAL "missing_extent")
        string(REPLACE
            "    s_ctx.framebuffer_width = width;\n    s_ctx.framebuffer_height = height;"
            "    (void)width;\n    (void)height;"
            _imgui_clip_extent_content "${_imgui_clip_extent_content}")
        if(_imgui_clip_extent_content STREQUAL _imgui_clip_extent)
            message(FATAL_ERROR "ImGui missing extent control did not remove the actual setup assignments")
        endif()
    elseif(_imgui_clip_variant STREQUAL "missing_queue_observation")
        set(_imgui_clip_needle "        imgui_observe_queue(draw_data, observation);")
        string(REPLACE "${_imgui_clip_needle}" "        (void)&imgui_observe_queue;"
            _imgui_clip_draw "${_imgui_clip_draw}")
        if(_imgui_clip_draw STREQUAL _imgui_clip_draw_base)
            message(FATAL_ERROR "ImGui queue control did not remove actual observation wire")
        endif()
    elseif(_imgui_clip_variant STREQUAL "missing_submission_counts")
        foreach(_imgui_clip_counter IN ITEMS main_submits recording_submits)
            set(_imgui_clip_needle
                "imgui_observation_increment(observation, &observation->value.${_imgui_clip_counter});")
            string(REPLACE "${_imgui_clip_needle}" "(void)observation;"
                _imgui_clip_draw "${_imgui_clip_draw}")
        endforeach()
        if(_imgui_clip_draw STREQUAL _imgui_clip_draw_base)
            message(FATAL_ERROR "ImGui submit control did not remove actual count wires")
        endif()
    endif()
    foreach(_imgui_clip_part IN ITEMS header state draw extent_content)
        set(_imgui_clip_path "${_imgui_clip_dir}/${_imgui_clip_variant}_${_imgui_clip_part}.inc.h")
        unset(_imgui_clip_previous)
        if(EXISTS "${_imgui_clip_path}")
            file(READ "${_imgui_clip_path}" _imgui_clip_previous)
        endif()
        if(NOT _imgui_clip_previous STREQUAL _imgui_clip_${_imgui_clip_part})
            file(WRITE "${_imgui_clip_path}" "${_imgui_clip_${_imgui_clip_part}}")
        endif()
    endforeach()
    if(_imgui_clip_variant STREQUAL "current")
        set(_imgui_clip_target test_jce_imgui_clip_draw)
        set(_imgui_clip_kind unit)
    else()
        set(_imgui_clip_target "test_jce_imgui_clip_draw_${_imgui_clip_variant}")
        set(_imgui_clip_kind contract)
    endif()
    jce_add_cxx_test(${_imgui_clip_target} KIND ${_imgui_clip_kind}
        SOURCES test_jce_imgui_clip_draw.cpp LINK jce_core
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")
    target_compile_features(${_imgui_clip_target} PRIVATE cxx_std_20)
    target_compile_definitions(${_imgui_clip_target} PRIVATE
        JCE_IMGUI_CLIP_HEADER="${_imgui_clip_dir}/${_imgui_clip_variant}_header.inc.h"
        JCE_IMGUI_CLIP_STATE_OWNER="${_imgui_clip_dir}/${_imgui_clip_variant}_state.inc.h"
        JCE_IMGUI_CLIP_DRAW_OWNER="${_imgui_clip_dir}/${_imgui_clip_variant}_draw.inc.h"
        JCE_IMGUI_CLIP_EXTENT_OWNER="${_imgui_clip_dir}/${_imgui_clip_variant}_extent_content.inc.h")
    if(MSVC)
        target_compile_options(${_imgui_clip_target} PRIVATE /Zc:preprocessor)
    endif()
    if(_imgui_clip_kind STREQUAL "contract")
        set_tests_properties(${_imgui_clip_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
foreach(_imgui_clip_var IN ITEMS owner_path helper_path owner helper ctx rec codes draw draw_base counter state extent
        first last dir variant header extent_content axis dimension needle part path previous target kind)
    unset(_imgui_clip_${_imgui_clip_var})
endforeach()
unset(_imgui_clip_var)

include("${CMAKE_CURRENT_LIST_DIR}/imgui_capture_ownership_controls.cmake")
