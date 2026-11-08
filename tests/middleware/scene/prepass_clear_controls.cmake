# Exact prepass clear calls + actual target owner; backend calls are typed ports.
set(_preclear_path "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_sr_cull.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_preclear_path}")
file(READ "${_preclear_path}" _preclear_source)
set(_preclear_begin "    bgfx_set_view_rect(v, 0, 0, sr->ssao_w, sr->ssao_h);")
set(_preclear_end "    bgfx_set_view_transform(v, view.raw[0], proj.raw[0]);")
foreach(_preclear_boundary IN ITEMS begin end)
    string(FIND "${_preclear_source}" "${_preclear_${_preclear_boundary}}" _preclear_first)
    string(FIND "${_preclear_source}" "${_preclear_${_preclear_boundary}}" _preclear_last REVERSE)
    if(_preclear_first LESS 0 OR NOT _preclear_first EQUAL _preclear_last)
        message(FATAL_ERROR "Prepass clear requires unique actual ${_preclear_boundary} boundary")
    endif()
    set(_preclear_${_preclear_boundary}_offset ${_preclear_first})
endforeach()
if(_preclear_end_offset LESS_EQUAL _preclear_begin_offset)
    message(FATAL_ERROR "Prepass clear production boundaries are reversed")
endif()
math(EXPR _preclear_length "${_preclear_end_offset}-${_preclear_begin_offset}")
string(SUBSTRING "${_preclear_source}" ${_preclear_begin_offset} ${_preclear_length} _preclear_current)

# Import the complete existing backend adapter function, including its loop and
# creation call, so the new admission is tested at its real owning boundary.
set(_preclear_adapter_path "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_sr_prepass_target_adapter.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_preclear_adapter_path}")
file(READ "${_preclear_adapter_path}" _preclear_adapter_source)
set(_preclear_adapter_begin "static uint16_t frame_buffer_create(const uint16_t *textures,")
set(_preclear_adapter_end "static void texture_destroy(uint16_t texture, void *user)")
foreach(_preclear_boundary IN ITEMS begin end)
    string(FIND "${_preclear_adapter_source}" "${_preclear_adapter_${_preclear_boundary}}" _preclear_first)
    string(FIND "${_preclear_adapter_source}" "${_preclear_adapter_${_preclear_boundary}}" _preclear_last REVERSE)
    if(_preclear_first LESS 0 OR NOT _preclear_first EQUAL _preclear_last)
        message(FATAL_ERROR "Prepass adapter requires unique actual ${_preclear_boundary} boundary")
    endif()
    set(_preclear_adapter_${_preclear_boundary}_offset ${_preclear_first})
endforeach()
if(_preclear_adapter_end_offset LESS_EQUAL _preclear_adapter_begin_offset)
    message(FATAL_ERROR "Prepass adapter production boundaries are reversed")
endif()
math(EXPR _preclear_adapter_length "${_preclear_adapter_end_offset}-${_preclear_adapter_begin_offset}")
string(SUBSTRING "${_preclear_adapter_source}" ${_preclear_adapter_begin_offset}
    ${_preclear_adapter_length} _preclear_adapter_current)

# Derive output slots from each actual fragment shader, retaining its exact
# neutral encoding contract. Comments or a stale manually copied slot cannot
# satisfy these assignment patterns.
foreach(_preclear_shader IN ITEMS static skinned)
    set(_preclear_shader_name fs_gbuffer_vel.sc)
    if(_preclear_shader STREQUAL "skinned")
        set(_preclear_shader_name fs_gbuffer_vel_skinned.sc)
    endif()
    set(_preclear_shader_path "${CMAKE_SOURCE_DIR}/engine/shaders/pbr/${_preclear_shader_name}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_preclear_shader_path}")
    file(READ "${_preclear_shader_path}" _preclear_shader_source)
    set(_preclear_velocity_pattern [=[gl_FragData\[([0-7])\][ \t]*=[ \t]*vec4\(delta \* 0\.5 \+ 0\.5,]=])
    set(_preclear_albedo_pattern [=[gl_FragData\[([0-7])\][ \t]*=[ \t]*vec4\(u_gbufferAlbedo\.rgb,]=])
    foreach(_preclear_output IN ITEMS velocity albedo)
        string(REGEX MATCHALL "${_preclear_${_preclear_output}_pattern}" _preclear_matches "${_preclear_shader_source}")
        list(LENGTH _preclear_matches _preclear_match_count)
        if(NOT _preclear_match_count EQUAL 1)
            message(FATAL_ERROR "Prepass clear requires one actual ${_preclear_shader} ${_preclear_output} output/encoding")
        endif()
        string(REGEX MATCH "${_preclear_${_preclear_output}_pattern}" _preclear_match "${_preclear_shader_source}")
        set(_preclear_${_preclear_shader}_${_preclear_output}_slot ${CMAKE_MATCH_1})
    endforeach()
endforeach()

set(_preclear_wire "                                2,    /* attachment 1 -> palette 2 (albedo) */\n                                1,    /* attachment 2 -> palette 1 (velocity) */")
string(FIND "${_preclear_current}" "${_preclear_wire}" _preclear_first)
string(FIND "${_preclear_current}" "${_preclear_wire}" _preclear_last REVERSE)
if(_preclear_first LESS 0 OR NOT _preclear_first EQUAL _preclear_last)
    message(FATAL_ERROR "Prepass clear old-index control requires one actual palette routing wire")
endif()
string(REPLACE "${_preclear_wire}"
    "                                1,    /* old attachment 1 -> neutral velocity */\n                                UINT8_MAX, /* old attachment 2 remains uncleared */"
    _preclear_old_index1 "${_preclear_current}")
# Mutate only the actual attachment-driven COLOR admission, never request policy.
set(_preclear_depth_wire "if (BGFX_HANDLE_IS_VALID(sr->ssao_normal_tex) ||\n            BGFX_HANDLE_IS_VALID(sr->ssao_albedo_tex))")
string(FIND "${_preclear_current}" "${_preclear_depth_wire}" _preclear_depth_first)
string(FIND "${_preclear_current}" "${_preclear_depth_wire}" _preclear_depth_last REVERSE)
if(_preclear_depth_first LESS 0 OR NOT _preclear_depth_first EQUAL _preclear_depth_last)
    message(FATAL_ERROR "Prepass depth-only clear requires one actual COLOR admission gate")
endif()
string(REPLACE "${_preclear_depth_wire}" "if (true)"
    _preclear_old_depth_color "${_preclear_current}")
# Removing only the new actual count-one guard must fail the same owner fixture.
set(_preclear_adapter_wire "    if (count == 1 && !bgfx_is_frame_buffer_valid(count, attachments))\n        return UINT16_MAX;\n")
string(FIND "${_preclear_adapter_current}" "${_preclear_adapter_wire}" _preclear_first)
string(FIND "${_preclear_adapter_current}" "${_preclear_adapter_wire}" _preclear_last REVERSE)
if(_preclear_first LESS 0 OR NOT _preclear_first EQUAL _preclear_last)
    message(FATAL_ERROR "Prepass adapter validation control requires one actual count-one guard")
endif()
string(REPLACE "${_preclear_adapter_wire}" ""
    _preclear_adapter_old_depth_validation "${_preclear_adapter_current}")
set(_preclear_old_depth_validation "${_preclear_current}")
set(_preclear_dir "${CMAKE_CURRENT_BINARY_DIR}/prepass-clear")
file(MAKE_DIRECTORY "${_preclear_dir}")
foreach(_preclear_variant IN ITEMS current old_index1 old_depth_color old_depth_validation)
    set(_preclear_generated "${_preclear_dir}/${_preclear_variant}.inc.h")
    jce_test_write_if_changed("${_preclear_generated}" "${_preclear_${_preclear_variant}}")
    set(_preclear_adapter_generated "${_preclear_dir}/${_preclear_variant}_adapter.inc.h")
    set(_preclear_adapter_body "${_preclear_adapter_current}")
    if(_preclear_variant STREQUAL "old_depth_validation")
        set(_preclear_adapter_body "${_preclear_adapter_old_depth_validation}")
    endif()
    jce_test_write_if_changed("${_preclear_adapter_generated}" "${_preclear_adapter_body}")
    set(_preclear_target test_jce_prepass_clear)
    set(_preclear_kind unit)
    if(NOT _preclear_variant STREQUAL "current")
        string(APPEND _preclear_target _${_preclear_variant})
        set(_preclear_kind contract)
    endif()
    jce_add_unit_test(${_preclear_target} KIND ${_preclear_kind}
        SOURCES test_jce_prepass_clear.c
            "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_sr_prepass_targets.c"
        LINK jce_core bgfx::bgfx
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src"
            "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene")
    target_compile_definitions(${_preclear_target} PRIVATE
        JCE_TEST_PREPASS_CLEAR_OWNER="${_preclear_generated}"
        JCE_TEST_PREPASS_ADAPTER_OWNER="${_preclear_adapter_generated}"
        JCE_TEST_STATIC_ALBEDO_SLOT=${_preclear_static_albedo_slot}
        JCE_TEST_STATIC_VELOCITY_SLOT=${_preclear_static_velocity_slot}
        JCE_TEST_SKINNED_ALBEDO_SLOT=${_preclear_skinned_albedo_slot}
        JCE_TEST_SKINNED_VELOCITY_SLOT=${_preclear_skinned_velocity_slot})
    if(MSVC)
        target_compile_options(${_preclear_target} PRIVATE /Zc:preprocessor)
    endif()
    if(_preclear_kind STREQUAL "contract")
        set_tests_properties(${_preclear_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
# Whole actual target-owner mutation controls consume the same lifecycle fixture.
set(_prestorage_owner "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene/jce_sr_prepass_targets.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_prestorage_owner}")
file(READ "${_prestorage_owner}" _prestorage_source)
set(_prestorage_dir "${_preclear_dir}/owner-controls")
file(MAKE_DIRECTORY "${_prestorage_dir}")
foreach(_prestorage_variant IN ITEMS old_storage no_mode_key fallback_key_false lose_staged_depth velocity_ignores_mrt)
    if(_prestorage_variant STREQUAL "old_storage")
        set(_prestorage_needle [===[    if (request->prefer_depth_only && !request->velocity) {
        target->depth = provider->create_texture(width, height,
            request->depth_format, provider->user);
        if (target->depth == UINT16_MAX) return false;
        const uint16_t depth_attachment = target->depth;
        target->frame_buffer = provider->create_frame_buffer(&depth_attachment, 1,
                                                              provider->user);
        if (target->frame_buffer != UINT16_MAX) return true;
    }
]===])
        set(_prestorage_replacement [===[]===])
    elseif(_prestorage_variant STREQUAL "no_mode_key")
        set(_prestorage_needle [===[        selected->target.request.velocity == request->velocity &&
        selected->target.request.prefer_depth_only == request->prefer_depth_only) {]===])
        set(_prestorage_replacement [===[        selected->target.request.velocity == request->velocity) {]===])
    elseif(_prestorage_variant STREQUAL "fallback_key_false")
        set(_prestorage_needle [===[        if (target->frame_buffer != UINT16_MAX) return true;
    }]===])
        set(_prestorage_replacement [===[        if (target->frame_buffer != UINT16_MAX) return true;
        target->request.prefer_depth_only = false;
    }]===])
    elseif(_prestorage_variant STREQUAL "lose_staged_depth")
        set(_prestorage_needle [===[    if (target->depth == UINT16_MAX) {
        target->depth = provider->create_texture(width, height,
            request->depth_format, provider->user);
        if (target->depth == UINT16_MAX) return false;
    }]===])
        set(_prestorage_replacement [===[    target->depth = provider->create_texture(width, height,
        request->depth_format, provider->user);
    if (target->depth == UINT16_MAX) return false;]===])
    elseif(_prestorage_variant STREQUAL "velocity_ignores_mrt")
        set(_prestorage_needle [===[    if (request->prefer_depth_only && !request->velocity) {]===])
        set(_prestorage_replacement [===[    if (request->prefer_depth_only) {]===])
    endif()
    string(FIND "${_prestorage_source}" "${_prestorage_needle}" _prestorage_first)
    string(FIND "${_prestorage_source}" "${_prestorage_needle}" _prestorage_last REVERSE)
    if(_prestorage_first LESS 0 OR NOT _prestorage_first EQUAL _prestorage_last)
        message(FATAL_ERROR "Prepass storage ${_prestorage_variant} requires one exact actual owner wire")
    endif()
    string(REPLACE "${_prestorage_needle}" "${_prestorage_replacement}"
        _prestorage_mutant "${_prestorage_source}")
    set(_prestorage_path "${_prestorage_dir}/${_prestorage_variant}.c")
    jce_test_write_if_changed("${_prestorage_path}" "${_prestorage_mutant}")
    set(_prestorage_target "test_jce_prepass_target_lifecycle_${_prestorage_variant}")
    jce_add_unit_test(${_prestorage_target} KIND contract
        SOURCES test_jce_prepass_target_lifecycle.c "${_prestorage_path}"
        LINK jce_core
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src"
            "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene")
    set_tests_properties(${_prestorage_target} PROPERTIES WILL_FAIL TRUE)
endforeach()
foreach(_prestorage_var IN ITEMS owner source dir variant needle replacement first last mutant path target)
    unset(_prestorage_${_prestorage_var})
endforeach()
unset(_prestorage_var)

foreach(_preclear_variable IN ITEMS path source begin end boundary first last begin_offset end_offset
    adapter_path adapter_source adapter_begin adapter_end adapter_begin_offset adapter_end_offset
    adapter_length adapter_current adapter_generated adapter_body adapter_wire adapter_old_depth_validation
    length current shader shader_name shader_path shader_source velocity_pattern albedo_pattern output
    matches match_count match static_velocity_slot static_albedo_slot skinned_velocity_slot
    skinned_albedo_slot wire old_index1 depth_first depth_last
    depth_wire old_depth_color old_depth_validation dir variant generated target kind)
    unset(_preclear_${_preclear_variable})
endforeach()
unset(_preclear_variable)
