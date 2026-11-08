# Actual cluster emission + canonical camera and shadow split owners, CPU only.
function(jce_fcluster_colour_fixture target kind)
    jce_add_unit_test(${target} KIND ${kind}
        SOURCES "${CMAKE_SOURCE_DIR}/tests/middleware/scene/test_jce_fcluster_colour_binding.c"
        LINK jce_core
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src"
            "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene")
    target_link_libraries(${target} PRIVATE bgfx::bgfx)
    if(MSVC)
        target_compile_options(${target} PRIVATE /Gy /Gw /Zc:preprocessor)
        target_link_options(${target} PRIVATE /OPT:REF)
    else()
        target_compile_options(${target} PRIVATE -ffunction-sections -fdata-sections)
        if(APPLE)
            target_link_options(${target} PRIVATE -Wl,-dead_strip)
        else()
            target_link_options(${target} PRIVATE -Wl,--gc-sections)
        endif()
        if(WIN32)
            target_compile_options(${target} PRIVATE -flto)
            target_link_options(${target} PRIVATE -flto)
        endif()
    endif()
endfunction()
jce_fcluster_colour_fixture(test_jce_fcluster_colour_binding unit)
set(_fcluster_colour_path "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_sr_fcluster_colour.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_fcluster_colour_path}"
    "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_sr_shadow_frame.c"
    "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_lighting_system.c")
file(READ "${_fcluster_colour_path}" _fcluster_colour_source)
function(jce_fcluster_colour_mutant suffix needle replacement)
    string(FIND "${_fcluster_colour_source}" "${needle}" _first)
    string(FIND "${_fcluster_colour_source}" "${needle}" _last REVERSE)
    if(_first LESS 0 OR NOT _first EQUAL _last)
        message(FATAL_ERROR "Cluster ${suffix} requires one exact emission wire")
    endif()
    string(REPLACE "${needle}" "${replacement}" _source "${_fcluster_colour_source}")
    if(suffix STREQUAL "late_geometry")
        set(_boundary "    const float identity[16] =")
        string(FIND "${_source}" "${_boundary}" _position)
        if(_position LESS 0)
            message(FATAL_ERROR "Cluster late-geometry control lost its actual bind boundary")
        endif()
        string(REPLACE "${_boundary}" "${needle}\n\n${_boundary}" _source "${_source}")
    endif()
    set(_path "${CMAKE_CURRENT_BINARY_DIR}/jce_sr_fcluster_colour_${suffix}.c")
    jce_test_write_if_changed("${_path}" "${_source}")
    set(_target "test_jce_fcluster_colour_binding_${suffix}")
    jce_fcluster_colour_fixture(${_target} contract)
    target_compile_definitions(${_target} PRIVATE "JCE_FCLUSTER_COLOUR_OWNER=\"${_path}\"")
    set_tests_properties(${_target} PROPERTIES WILL_FAIL TRUE)
endfunction()
jce_fcluster_colour_mutant(missing_camera
    "    jce_lighting_system_bind_camera(position);" "    (void)position;")
jce_fcluster_colour_mutant(missing_frame
    "    sr_bind_frame_shadow_splits(sr);\n    sr_apply_fog_uniforms(sr);"
    "    /* Old inherited split/fog values. */")
jce_fcluster_colour_mutant(late_geometry
    "    if (!BGFX_HANDLE_IS_VALID(vertex) || !BGFX_HANDLE_IS_VALID(index)) {\n        receipt.reason = JCE_FCLUSTER_GEOMETRY_UNAVAILABLE;\n        return receipt;\n    }"
    "")
unset(_fcluster_colour_path)
unset(_fcluster_colour_source)

# Execute the existing full model fixture against an ordinary missing fallback.
set(_foliage_model_path "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_model.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_foliage_model_path}")
file(READ "${_foliage_model_path}" _foliage_model_source)
set(_foliage_default_wire "    JcePbrMaterial fallback;\n    if (!mat) {\n        fallback = jce_pbr_material_default();\n        mat = &fallback;\n    }")
string(FIND "${_foliage_model_source}" "${_foliage_default_wire}" _first)
string(FIND "${_foliage_model_source}" "${_foliage_default_wire}" _last REVERSE)
if(_first LESS 0 OR NOT _first EQUAL _last)
    message(FATAL_ERROR "Foliage default control requires one effective-material fallback")
endif()
string(REPLACE "${_foliage_default_wire}" "    /* Old absent-material behavior. */"
    _foliage_model_old "${_foliage_model_source}")
set(_foliage_model_old_path "${CMAKE_CURRENT_BINARY_DIR}/jce_model_missing_foliage_default.c")
jce_test_write_if_changed("${_foliage_model_old_path}" "${_foliage_model_old}")
jce_add_unit_test(test_jce_model_missing_foliage_default KIND contract
    SOURCES "${CMAKE_SOURCE_DIR}/tests/renderer/test_jce_model_colour_submit.c"
        "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_instance_upload.c"
    LINK jce_core
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/renderer"
        "$<TARGET_PROPERTY:bgfx::bgfx,INTERFACE_INCLUDE_DIRECTORIES>")
target_compile_definitions(test_jce_model_missing_foliage_default PRIVATE
    "JCE_TEST_MODEL_COLOUR_OWNER=\"${_foliage_model_old_path}\"")
if(MSVC)
    target_compile_options(test_jce_model_missing_foliage_default PRIVATE /Zc:preprocessor)
endif()
set_tests_properties(test_jce_model_missing_foliage_default PROPERTIES WILL_FAIL TRUE)
foreach(_foliage_var IN ITEMS model_path model_source default_wire model_old model_old_path)
    unset(_foliage_${_foliage_var})
endforeach()
unset(_foliage_var)
unset(_first)
unset(_last)
