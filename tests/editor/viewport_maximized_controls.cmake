# Extend the existing layout-routes owner with strict actual viewport slices.
# The complete layout blocks, frame latch and actual Scene admission prefix are
# consumed by counted CPU ports. No duplicate rendering/policy implementation.
set(_vmax_layout "${CMAKE_SOURCE_DIR}/editor/src/ui/jce_editor_layout.cpp")
set(_vmax_scene "${CMAKE_SOURCE_DIR}/editor/src/panels/jce_panel_scene_view.cpp")
set(_vmax_gizmo "${CMAKE_SOURCE_DIR}/editor/src/panels/jce_scene_view_helpers.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_vmax_layout}" "${_vmax_scene}" "${_vmax_gizmo}")
file(READ "${_vmax_layout}" _vmax_layout_source)
file(READ "${_vmax_scene}" _vmax_scene_source)
file(READ "${_vmax_gizmo}" _vmax_gizmo_source)

function(_jce_vmax_slice output source start_marker end_marker)
    foreach(boundary IN ITEMS start end)
        string(FIND "${source}" "${${boundary}_marker}" first)
        string(FIND "${source}" "${${boundary}_marker}" last REVERSE)
        if(first LESS 0 OR NOT first EQUAL last)
            message(FATAL_ERROR "Viewport actual ${output} ${boundary} boundary must be unique")
        endif()
        set(${boundary} ${first})
    endforeach()
    if(end LESS_EQUAL start)
        message(FATAL_ERROR "Viewport actual ${output} boundaries are reversed")
    endif()
    math(EXPR length "${end} - ${start}")
    string(SUBSTRING "${source}" ${start} ${length} selected)
    set(${output} "${selected}" PARENT_SCOPE)
endfunction()

_jce_vmax_slice(_vmax_state "${_vmax_layout_source}"
    "static JceEditorPanel s_maximized_panel = JCE_PANEL_COUNT;"
    "/* Kept so existing call sites and the Game View toolbar read the same way. */")
_jce_vmax_slice(_vmax_dock "${_vmax_layout_source}"
    "    /* Retain dock membership even when an overlay owns viewport rendering. */"
    "    panel_phase(\"ed_p_game_view\", &ed_pt);")
_jce_vmax_slice(_vmax_console_dock "${_vmax_layout_source}"
    "    if (*jce_editor_panel_visible_ptr(JCE_PANEL_CONSOLE)) {"
    "    /* ── Timeline")
_jce_vmax_slice(_vmax_host "${_vmax_layout_source}"
    "    /* Lock viewport ownership for this UI frame."
    "    /* Panels get first refusal for context-specific editing shortcuts")
_jce_vmax_slice(_vmax_scene_prefix "${_vmax_scene_source}"
    "void jce_editor_panel_scene_view_content(bool viewport_admitted)"
    "    SceneViewCtx ctx;")
_jce_vmax_slice(_vmax_scene_cancel "${_vmax_scene_source}"
    "static void cancel_deferred_scene_pick(void)"
    "static void apply_single_pick_selection(uint32_t best_id, bool add_mode)")
_jce_vmax_slice(_vmax_gizmo_clear "${_vmax_gizmo_source}"
    "void clear_stale_gizmo_interaction_state(void)"
    "bool has_valid_gizmo_target(void)")

# Exact single-wire replacements, not regex list counts containing semicolons.
set(_vmax_scene_wire "jce_editor_panel_scene_view_content(frame_maximized == JCE_PANEL_COUNT);")
set(_vmax_game_wire "if (docked_open && frame_maximized == JCE_PANEL_COUNT)")
set(_vmax_old_dock "${_vmax_dock}")
foreach(_vmax_wire IN ITEMS scene game)
    string(FIND "${_vmax_dock}" "${_vmax_${_vmax_wire}_wire}" _vmax_first)
    string(FIND "${_vmax_dock}" "${_vmax_${_vmax_wire}_wire}" _vmax_last REVERSE)
    if(_vmax_first LESS 0 OR NOT _vmax_first EQUAL _vmax_last)
        message(FATAL_ERROR "Viewport old guard requires one actual ${_vmax_wire} wire")
    endif()
endforeach()
string(REPLACE "${_vmax_scene_wire}" "jce_editor_panel_scene_view_content(true);"
    _vmax_old_dock "${_vmax_old_dock}")
string(REPLACE "${_vmax_game_wire}" "if (docked_open && !maximized)"
    _vmax_old_dock "${_vmax_old_dock}")
if(_vmax_old_dock STREQUAL _vmax_dock)
    message(FATAL_ERROR "Viewport ordinary old guard did not alter the actual dock owner")
endif()

set(_vmax_dir "${CMAKE_CURRENT_BINARY_DIR}/viewport-maximized")
file(MAKE_DIRECTORY "${_vmax_dir}")
foreach(_vmax_part IN ITEMS state dock console_dock host scene_prefix scene_cancel gizmo_clear old_dock)
    set(_vmax_path "${_vmax_dir}/${_vmax_part}.inc.h")
    jce_test_write_if_changed("${_vmax_path}" "${_vmax_${_vmax_part}}")
endforeach()

jce_add_cxx_test(test_jce_editor_layout_routes_old_covered KIND contract
    SOURCES test_jce_editor_layout_routes.cpp)
target_compile_features(test_jce_editor_layout_routes_old_covered PRIVATE cxx_std_20)
target_compile_definitions(test_jce_editor_layout_routes_old_covered PRIVATE
    JCE_EDITOR_LAYOUT_ROUTES_OWNER="${CMAKE_CURRENT_BINARY_DIR}/layout-routes/actual.inc.h")
set_tests_properties(test_jce_editor_layout_routes_old_covered PROPERTIES WILL_FAIL TRUE)
foreach(_vmax_target IN ITEMS test_jce_editor_layout_routes
        test_jce_editor_layout_routes_old_routing test_jce_editor_layout_routes_old_covered)
    set(_vmax_dock_file dock)
    if(_vmax_target STREQUAL "test_jce_editor_layout_routes_old_covered")
        set(_vmax_dock_file old_dock)
    endif()
    target_compile_definitions(${_vmax_target} PRIVATE
        JCE_EDITOR_VIEWPORT_MAX_STATE="${_vmax_dir}/state.inc.h"
        JCE_EDITOR_VIEWPORT_MAX_DOCK="${_vmax_dir}/${_vmax_dock_file}.inc.h"
        JCE_EDITOR_VIEWPORT_MAX_HOST="${_vmax_dir}/host.inc.h"
        JCE_EDITOR_VIEWPORT_CONSOLE_DOCK="${_vmax_dir}/console_dock.inc.h"
        JCE_EDITOR_VIEWPORT_SCENE_PREFIX="${_vmax_dir}/scene_prefix.inc.h"
        JCE_EDITOR_VIEWPORT_SCENE_CANCEL="${_vmax_dir}/scene_cancel.inc.h"
        JCE_EDITOR_VIEWPORT_GIZMO_CLEAR="${_vmax_dir}/gizmo_clear.inc.h")
endforeach()
