# Complete real Scene Overview, bounds and orbit owners. No UI/renderer context.
# The unchanged pre-fix Overview must fail ordinary doctest assertions.
set(_overview_camera "${CMAKE_SOURCE_DIR}/editor/src/scene/jce_scene_render_camera.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_overview_camera}")
file(READ "${_overview_camera}" _overview_source)
set(_overview_dir "${CMAKE_CURRENT_BINARY_DIR}/scene-overview-controls")
file(MAKE_DIRECTORY "${_overview_dir}")

# Reuse the existing strict unique-anchor helper defined above this include.
_jce_game_camera_extract("${_overview_source}"
    "\nvoid jce_editor_scene_frame_overview(void)"
    "\nvoid jce_editor_scene_frame_vista(void)" _overview_owner)
_jce_game_camera_extract("${_overview_source}"
    "\nbool jce_editor_scene_camera_get_entity_focus_bounds("
    "\nvoid jce_editor_scene_camera_focus_entity(" _overview_entity_bounds)
_jce_game_camera_extract("${_overview_source}"
    "\nvoid orbit_apply(void)"
    "\nstatic void orbit_cancel_focus_anim(void)" _overview_orbit)
_jce_game_camera_extract("${_overview_source}"
    "\nstatic void orbit_cancel_focus_anim(void)"
    "\nvoid jce_editor_scene_camera_update(float dt_sec)" _overview_cancel)
_jce_game_camera_extract("${_overview_source}"
    "\nvoid jce_editor_scene_camera_reset(void)"
    "\nvoid jce_editor_scene_camera_focus_aabb(" _overview_reset)
_jce_game_camera_extract("${_overview_source}"
    "    if (!s_sr.initialized || !s_sr.camera || !s_sr.focus_anim.active) return;"
    "/* ── Public camera API" _overview_update_tail)
# Only debug spin/capture is omitted; the entire actual focus-update tail stays.
set(_overview_update "void jce_editor_scene_camera_update(float dt_sec)\n{\n${_overview_update_tail}")

set(_overview_constants "")
foreach(_overview_constant IN ITEMS ORBIT_DIST_MIN ORBIT_DIST_MAX
    ORBIT_CLIP_HYSTERESIS ORBIT_CLIP_MIN_NEAR_DELTA ORBIT_CLIP_MIN_FAR_DELTA)
    string(REGEX MATCHALL "#define ${_overview_constant}[ \t]+[^\n]*\n"
        _overview_matches "${_overview_source}")
    list(LENGTH _overview_matches _overview_count)
    if(NOT _overview_count EQUAL 1)
        message(FATAL_ERROR "Overview fixture requires one actual ${_overview_constant}")
    endif()
    string(APPEND _overview_constants "${_overview_matches}")
endforeach()
foreach(_overview_part IN ITEMS owner orbit cancel reset update constants)
    jce_test_write_if_changed("${_overview_dir}/overview_${_overview_part}.inc.h"
        "${_overview_${_overview_part}}")
endforeach()

# Mutate the complete actual helper, with strict one-wire admission. Neither
# control substitutes another bounds resolver or weakens the shared assertions.
set(_overview_index_wire
    "    JceEntity e = jce_scene_entity_from_index(scene, entity_id);")
set(_overview_old_index_wire "    JceEntity e = (JceEntity)entity_id;")
_jce_game_camera_unique("${_overview_entity_bounds}"
    "${_overview_index_wire}" _overview_index_position)
set(_overview_invalid_wire [=[    const auto bounds_valid = [&]() {
        for (int k = 0; k < 3; ++k) {
            if (!std::isfinite(out_min3[k]) || !std::isfinite(out_max3[k]) ||
                out_min3[k] > out_max3[k]) return false;
        }
        return true;
    };]=])
set(_overview_old_invalid_wire "    const auto bounds_valid = []() { return true; };")
_jce_game_camera_unique("${_overview_entity_bounds}"
    "${_overview_invalid_wire}" _overview_invalid_position)
string(REGEX MATCHALL "return bounds_valid\\(\\)" _overview_valid_returns
    "${_overview_entity_bounds}")
list(LENGTH _overview_valid_returns _overview_valid_return_count)
if(NOT _overview_valid_return_count EQUAL 2)
    message(FATAL_ERROR "Overview bounds helper must validate both actual success paths")
endif()
set(_overview_current "${_overview_entity_bounds}")
string(REPLACE "${_overview_index_wire}" "${_overview_old_index_wire}"
    _overview_old_index "${_overview_entity_bounds}")
string(REPLACE "${_overview_invalid_wire}" "${_overview_old_invalid_wire}"
    _overview_old_invalid "${_overview_entity_bounds}")

foreach(_overview_variant IN ITEMS current old_index old_invalid)
    set(_overview_generated "${_overview_dir}/${_overview_variant}_entity_bounds.inc.h")
    jce_test_write_if_changed("${_overview_generated}" "${_overview_${_overview_variant}}")
    file(READ "${_overview_generated}" _overview_written)
    if(NOT _overview_written STREQUAL _overview_${_overview_variant})
        message(FATAL_ERROR "Overview helper compiler input differs from its selected owner")
    endif()
    if(_overview_variant STREQUAL "old_index")
        _jce_game_camera_unique("${_overview_written}"
            "${_overview_old_index_wire}" _overview_written_position)
    elseif(_overview_variant STREQUAL "old_invalid")
        _jce_game_camera_unique("${_overview_written}"
            "${_overview_old_invalid_wire}" _overview_written_position)
    endif()
    set(_overview_target test_jce_scene_overview)
    set(_overview_kind integration)
    if(NOT _overview_variant STREQUAL "current")
        string(APPEND _overview_target "_${_overview_variant}")
        set(_overview_kind contract)
    endif()
    jce_add_cxx_test(${_overview_target} KIND ${_overview_kind}
        SOURCES test_jce_scene_overview.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/scene/jce_scene_camera_focus.cpp"
        INCLUDES "${_overview_dir}" "${CMAKE_SOURCE_DIR}/engine/src"
        LINK jce_core jce_scene jce_renderer jce_resource)
    target_compile_features(${_overview_target} PRIVATE cxx_std_20)
    target_compile_definitions(${_overview_target} PRIVATE
        JCE_TEST_SCENE_OVERVIEW_BOUNDS_OWNER="${_overview_generated}")
    if(_overview_kind STREQUAL "contract")
        # Registration convention only. Acceptance also requires ordinary EXE
        # execution with full doctest failures and rc=1; WILL_FAIL is no evidence
        # of that execution, and setup/build failure or a crash is not a control.
        set_tests_properties(${_overview_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
foreach(_overview_var IN ITEMS camera source dir owner entity_bounds orbit cancel reset
    update_tail update constants constant matches count part index_wire old_index_wire
    index_position invalid_wire old_invalid_wire invalid_position valid_returns
    valid_return_count current old_index old_invalid variant generated written
    written_position target kind)
    unset(_overview_${_overview_var})
endforeach()
unset(_overview_var)
