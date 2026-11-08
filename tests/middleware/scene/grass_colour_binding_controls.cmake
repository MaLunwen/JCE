# Exact draw owner + actual full/directional light and PBR view-mode bodies.
# Backend ports count uniform-name aliases, sampler writes and ALL discards.
function(jce_grass_colour_binding_fixture target kind)
    jce_add_unit_test(${target}
        KIND ${kind}
        SOURCES "${CMAKE_SOURCE_DIR}/tests/middleware/scene/test_jce_grass_colour_binding.c"
        LINK jce_core
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src"
            "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene")
    target_link_libraries(${target} PRIVATE bgfx::bgfx)
    # The included PBR TU needs SDL declarations; its unused IO functions are
    # discarded below, so this fixture borrows headers without an SDL link.
    target_include_directories(${target} SYSTEM PRIVATE
        "$<TARGET_PROPERTY:sdl::sdl,INTERFACE_INCLUDE_DIRECTORIES>")
    # Discard unrelated functions from the exact included TUs where supported.
    # The fixture also fails explicitly if retained custom-program ports run.
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
            # PE unwind tables must remain for Unity's ordinary assertion
            # longjmp. LTO removes unrelated functions before PE section GC.
            target_compile_options(${target} PRIVATE -flto)
            target_link_options(${target} PRIVATE -flto)
        endif()
    endif()
endfunction()

jce_grass_colour_binding_fixture(test_jce_grass_colour_binding unit)
set(_grass_binding_owner_path
    "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_sr_grass_colour.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_grass_binding_owner_path}")
file(READ "${_grass_binding_owner_path}" _grass_binding_owner)

function(jce_grass_colour_binding_mutant suffix needle replacement)
    string(REPLACE "${needle}" "" _without "${_grass_binding_owner}")
    string(LENGTH "${_grass_binding_owner}" _original_length)
    string(LENGTH "${_without}" _without_length)
    string(LENGTH "${needle}" _needle_length)
    math(EXPR _removed "${_original_length} - ${_without_length}")
    if(NOT _removed EQUAL _needle_length)
        message(FATAL_ERROR "Grass binding ${suffix} requires one exact owner wire")
    endif()
    string(REPLACE "${needle}" "${replacement}" _mutant "${_grass_binding_owner}")
    set(_path "${CMAKE_CURRENT_BINARY_DIR}/jce_sr_grass_colour_${suffix}.c")
    jce_test_write_if_changed("${_path}" "${_mutant}")
    set(_target "test_jce_grass_colour_binding_${suffix}")
    jce_grass_colour_binding_fixture(${_target} contract)
    target_compile_definitions(${_target} PRIVATE
        "JCE_GRASS_COLOUR_BINDING_OWNER=\"${_path}\"")
    set_tests_properties(${_target} PROPERTIES WILL_FAIL TRUE)
endfunction()

jce_grass_colour_binding_mutant(duplicate_shadow
    "    sr_bind_frame_shadow_state(sr);"
    "    sr_bind_frame_shadow_state(sr);\n    sr_bind_frame_shadow_state(sr);")
jce_grass_colour_binding_mutant(camera_alias
    "    jce_light_env_apply_directional_for_layer(sr->light_env, sr->renderer,"
    "    jce_light_env_apply_for_layer(sr->light_env, sr->renderer,")
jce_grass_colour_binding_mutant(first_batch_only
    "    sr_grass_bind_colour_uniforms(sr, range->field, range->camera,"
    "    if (offset == 0u) sr_grass_bind_colour_uniforms(sr, range->field, range->camera,")
unset(_grass_binding_owner_path)
unset(_grass_binding_owner)
