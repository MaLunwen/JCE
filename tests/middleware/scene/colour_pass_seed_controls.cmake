# Execute the actual light-preparation tail and GI-preparation slice with the
# complete lighting owner and the actual complete GI upload helper functions.
set(_colour_seed_draw "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_sr_draw.c")
set(_colour_seed_gi "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_sr_cull.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_colour_seed_draw}" "${_colour_seed_gi}")
file(READ "${_colour_seed_draw}" _colour_seed_draw_text)
file(READ "${_colour_seed_gi}" _colour_seed_gi_text)

function(jce_colour_seed_slice output text begin end)
    string(FIND "${text}" "${begin}" _first)
    string(FIND "${text}" "${begin}" _last REVERSE)
    string(FIND "${text}" "${end}" _end)
    string(FIND "${text}" "${end}" _end_last REVERSE)
    if(_first LESS 0 OR NOT _first EQUAL _last OR _end LESS_EQUAL _first OR
        NOT _end EQUAL _end_last)
        message(FATAL_ERROR "Colour pass fixture needs one actual ${output} owner slice")
    endif()
    math(EXPR _length "${_end} - ${_first}")
    string(SUBSTRING "${text}" ${_first} ${_length} _slice)
    set(${output} "${_slice}" PARENT_SCOPE)
endfunction()
jce_colour_seed_slice(_colour_seed_light_prepare "${_colour_seed_draw_text}"
    "        /* Colour-pass light preparation changes CPU inputs only."
    "        /* Forward+ clustered lighting: use the admitted color-view request")
jce_colour_seed_slice(_colour_seed_gi_prepare "${_colour_seed_draw_text}"
    "    /* Colour-pass GI preparation records no draw commands."
    "    /* Parallel color gather (concurrent-ECS, charter opt-in JCE_PARALLEL_GATHER).")
jce_colour_seed_slice(_colour_seed_gi_owner "${_colour_seed_gi_text}"
    "void sr_upload_gi_uniforms_at(" "/* Apply baked-GI uniforms + (optional) reflection-probe cubemap override.")

set(_colour_seed_dir "${CMAKE_CURRENT_BINARY_DIR}/colour-pass-seed")
file(MAKE_DIRECTORY "${_colour_seed_dir}")
jce_test_write_if_changed("${_colour_seed_dir}/gi_owner.inc.h" "${_colour_seed_gi_owner}")
foreach(_colour_seed_variant IN ITEMS current old_light_seed old_gi_seed)
    set(_colour_seed_target test_jce_colour_pass_seed)
    set(_colour_seed_kind unit)
    set(_colour_seed_light "${_colour_seed_light_prepare}")
    set(_colour_seed_gi_stage "${_colour_seed_gi_prepare}")
    if(NOT _colour_seed_variant STREQUAL "current")
        string(APPEND _colour_seed_target "_${_colour_seed_variant}")
        set(_colour_seed_kind contract)
        if(_colour_seed_variant STREQUAL "old_light_seed")
            string(APPEND _colour_seed_light "        jce_light_env_apply(sr->light_env, sr->renderer);\n")
        else()
            string(APPEND _colour_seed_gi_stage "    sr_upload_gi_uniforms(sr);\n")
        endif()
    endif()
    set(_colour_seed_light_path "${_colour_seed_dir}/${_colour_seed_variant}_light.inc.h")
    set(_colour_seed_gi_path "${_colour_seed_dir}/${_colour_seed_variant}_gi.inc.h")
    jce_test_write_if_changed("${_colour_seed_light_path}" "${_colour_seed_light}")
    jce_test_write_if_changed("${_colour_seed_gi_path}" "${_colour_seed_gi_stage}")
    jce_add_unit_test(${_colour_seed_target} KIND ${_colour_seed_kind}
        SOURCES test_jce_colour_pass_seed.c LINK jce_core
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src"
            "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene")
    target_link_libraries(${_colour_seed_target} PRIVATE bgfx::bgfx)
    # The reused PBR owner needs SDL declarations; unrelated IO is discarded.
    target_include_directories(${_colour_seed_target} SYSTEM PRIVATE
        "$<TARGET_PROPERTY:sdl::sdl,INTERFACE_INCLUDE_DIRECTORIES>")
    target_compile_definitions(${_colour_seed_target} PRIVATE
        "JCE_TEST_COLOUR_PASS_LIGHT_PREPARE=\"${_colour_seed_light_path}\""
        "JCE_TEST_COLOUR_PASS_GI_PREPARE=\"${_colour_seed_gi_path}\""
        "JCE_TEST_COLOUR_PASS_GI_OWNER=\"${_colour_seed_dir}/gi_owner.inc.h\"")
    # Reused complete light/grass TUs have unrelated public functions; retain
    # exactly the same section-retirement policy as the existing binding probe.
    if(MSVC)
        target_compile_options(${_colour_seed_target} PRIVATE /Gy /Gw /Zc:preprocessor)
        target_link_options(${_colour_seed_target} PRIVATE /OPT:REF)
    else()
        target_compile_options(${_colour_seed_target} PRIVATE -ffunction-sections -fdata-sections)
        if(APPLE)
            target_link_options(${_colour_seed_target} PRIVATE -Wl,-dead_strip)
        else()
            target_link_options(${_colour_seed_target} PRIVATE -Wl,--gc-sections)
        endif()
        if(WIN32)
            target_compile_options(${_colour_seed_target} PRIVATE -flto)
            target_link_options(${_colour_seed_target} PRIVATE -flto)
        endif()
    endif()
    if(NOT _colour_seed_variant STREQUAL "current")
        set_tests_properties(${_colour_seed_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
foreach(_colour_seed_variable IN ITEMS draw gi draw_text gi_text light_prepare gi_prepare
    gi_owner dir variant target kind light gi_stage light_path gi_path)
    unset(_colour_seed_${_colour_seed_variable})
endforeach()
unset(_colour_seed_variable)
