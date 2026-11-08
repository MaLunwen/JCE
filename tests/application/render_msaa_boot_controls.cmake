# Actual stock startup ordering, Engine graphics owner and renderer MSAA setter.
# Only unrelated application services and the backend reset boundary are ports.
function(_jce_msaa_extract source begin end output)
    string(FIND "${source}" "${begin}" _first)
    string(FIND "${source}" "${begin}" _first_last REVERSE)
    string(FIND "${source}" "${end}" _last)
    string(FIND "${source}" "${end}" _last_last REVERSE)
    if(_first LESS 0 OR NOT _first EQUAL _first_last OR _last LESS_EQUAL _first OR
       NOT _last EQUAL _last_last)
        message(FATAL_ERROR "MSAA boot fixture requires one complete actual ${begin} block")
    endif()
    math(EXPR _length "${_last} - ${_first}")
    string(SUBSTRING "${source}" ${_first} ${_length} _body)
    set(${output} "${_body}" PARENT_SCOPE)
    set(${output}_position ${_first} PARENT_SCOPE)
endfunction()

set(_msaa_stock "${CMAKE_SOURCE_DIR}/engine/include/jce/application/jce_default_main.inc.h")
set(_msaa_engine "${CMAKE_SOURCE_DIR}/engine/src/application/jce_engine.c")
set(_msaa_renderer "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_renderer.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_msaa_stock}" "${_msaa_engine}" "${_msaa_renderer}")
file(READ "${_msaa_stock}" _msaa_stock_source)
file(READ "${_msaa_engine}" _msaa_engine_source)
file(READ "${_msaa_renderer}" _msaa_renderer_source)
_jce_msaa_extract("${_msaa_stock_source}"
    "static char *dm_pak_read_config(const char *pak_key, size_t *out_len)\n{"
    "static void s_default_load_render_settings(void)"
    _msaa_read_config)
_jce_msaa_extract("${_msaa_stock_source}"
    "static void s_default_load_render_settings(void)"
    "static bool app_init(const JceServices *svc, void *ud)"
    _msaa_load_settings)
_jce_msaa_extract("${_msaa_stock_source}"
    "    /* Settings S2: the engine-init boot ran BEFORE any game PAK was mounted,"
    "    /* Render-pipeline startup resolution is complete. */"
    _msaa_pipeline)
_jce_msaa_extract("${_msaa_stock_source}"
    "    /* Project-wide quality settings (Top 5): apply the legacy settings loaded"
    "    if (s_scene) {\n        JceRuntimeDesc rd = (JceRuntimeDesc){0};"
    _msaa_legacy)
_jce_msaa_extract("${_msaa_engine_source}"
    "void jce_engine_apply_graphics_config(" "static bool jce_path_exists(" _msaa_graphics)
_jce_msaa_extract("${_msaa_renderer_source}"
    "void jce_renderer_set_msaa(" "void jce_renderer_set_backbuffer_capture(" _msaa_setter)
set(_msaa_dir "${CMAKE_CURRENT_BINARY_DIR}/render-msaa-boot")
file(MAKE_DIRECTORY "${_msaa_dir}")
jce_test_write_if_changed("${_msaa_dir}/graphics.inc.h" "${_msaa_graphics}")
jce_test_write_if_changed("${_msaa_dir}/setter.inc.h" "${_msaa_setter}")
jce_test_write_if_changed("${_msaa_dir}/load-settings.inc.h" "${_msaa_load_settings}")
jce_test_write_if_changed("${_msaa_dir}/read-config.inc.h" "${_msaa_read_config}")
if(_msaa_pipeline_position LESS _msaa_legacy_position)
    set(_msaa_current "${_msaa_pipeline}\n${_msaa_legacy}")
else()
    set(_msaa_current "${_msaa_legacy}\n${_msaa_pipeline}")
endif()
# Restore only the old lower-priority late writer. It must fail ordinary
# unchanged assertions after the shipping owner is repaired.
set(_msaa_old_late_legacy "${_msaa_current}\n    if (s_render_settings_loaded && svc && svc->renderer)\n        jce_renderer_set_msaa(svc->renderer, s_render_settings.msaa);\n")
foreach(_msaa_case IN ITEMS current old_late_legacy packed)
    set(_msaa_target test_jce_render_msaa_boot)
    set(_msaa_kind integration)
    if(_msaa_case STREQUAL "old_late_legacy")
        string(APPEND _msaa_target "_${_msaa_case}")
        set(_msaa_kind contract)
    elseif(_msaa_case STREQUAL "packed")
        string(APPEND _msaa_target "_packed")
    endif()
    set(_msaa_path "${_msaa_dir}/${_msaa_case}.inc.h")
    if(_msaa_case STREQUAL "packed")
        jce_test_write_if_changed("${_msaa_path}" "${_msaa_current}")
    else()
        jce_test_write_if_changed("${_msaa_path}" "${_msaa_${_msaa_case}}")
    endif()
    set(_msaa_work "${_msaa_dir}/${_msaa_case}-work")
    file(MAKE_DIRECTORY "${_msaa_work}/Settings" "${_msaa_work}/resources/_cooked")
    jce_add_unit_test(${_msaa_target} KIND ${_msaa_kind}
        SOURCES test_jce_render_msaa_boot.c
            "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_render_pipeline.c"
            "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_render_settings.c"
        LINK jce_core jce_resource bgfx::bgfx
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${_msaa_dir}")
    target_compile_definitions(${_msaa_target} PRIVATE
        JCE_TEST_MSAA_STOCK_OWNER="${_msaa_path}"
        JCE_TEST_MSAA_WORK_DIR="${_msaa_work}/")
    if(_msaa_case STREQUAL "packed")
        target_compile_definitions(${_msaa_target} PRIVATE JCE_TEST_MSAA_PACKAGED=1)
    endif()
    if(MSVC)
        target_compile_options(${_msaa_target} PRIVATE /Zc:preprocessor)
    endif()
    set_tests_properties(${_msaa_target} PROPERTIES
        LABELS "${_msaa_kind};application"
        WORKING_DIRECTORY "${_msaa_work}"
        RESOURCE_LOCK JCE_RENDER_MSAA_BOOT
        ENVIRONMENT "${JCE_TEST_ENVIRONMENT};JCE_RP_FORCE=")
    if(_msaa_case STREQUAL "old_late_legacy")
        set_tests_properties(${_msaa_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
add_test(NAME test_jce_render_msaa_boot_force COMMAND test_jce_render_msaa_boot force)
set_tests_properties(test_jce_render_msaa_boot_force PROPERTIES
    TIMEOUT 60 LABELS "integration;application" JCE_TEST_KIND integration
    RESOURCE_LOCK JCE_RENDER_MSAA_BOOT
    WORKING_DIRECTORY "${_msaa_dir}/current-work"
    ENVIRONMENT "${JCE_TEST_ENVIRONMENT};JCE_RP_FORCE=msaa_samples=4")
set_property(GLOBAL APPEND PROPERTY JCE_TESTS_WITH_ENV test_jce_render_msaa_boot_force)
add_test(NAME test_jce_render_msaa_boot_packed_force COMMAND test_jce_render_msaa_boot_packed force)
set_tests_properties(test_jce_render_msaa_boot_packed_force PROPERTIES
    TIMEOUT 60 LABELS "integration;application" JCE_TEST_KIND integration
    RESOURCE_LOCK JCE_RENDER_MSAA_BOOT
    WORKING_DIRECTORY "${_msaa_dir}/packed-work"
    ENVIRONMENT "${JCE_TEST_ENVIRONMENT};JCE_RP_FORCE=msaa_samples=4")
set_property(GLOBAL APPEND PROPERTY JCE_TESTS_WITH_ENV test_jce_render_msaa_boot_packed_force)
