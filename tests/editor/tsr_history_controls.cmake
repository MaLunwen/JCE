# Real Scene TSR prepare/resolve blocks and camera/scene transition entries.
# CPU submission parameters only; no window, driver, GPU history or image claim.
set(_tsr_scene "${CMAKE_SOURCE_DIR}/editor/src/scene/jce_editor_scene_render.cpp")
set(_tsr_camera "${CMAKE_SOURCE_DIR}/editor/src/scene/jce_scene_render_camera.cpp")
set(_tsr_state "${CMAKE_SOURCE_DIR}/editor/src/scene/jce_scene_render_internal.h")
set(_tsr_viewport "${CMAKE_SOURCE_DIR}/editor/src/scene/jce_editor_viewport_common.h")
set(_tsr_dir "${CMAKE_CURRENT_BINARY_DIR}/tsr-history-controls")
file(MAKE_DIRECTORY "${_tsr_dir}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_tsr_scene}" "${_tsr_camera}" "${_tsr_state}" "${_tsr_viewport}")
file(READ "${_tsr_scene}" _tsr_scene_source)
file(READ "${_tsr_camera}" _tsr_camera_source)
file(READ "${_tsr_viewport}" _tsr_viewport_source)

function(_jce_tsr_history_extract OUTPUT INPUT START END)
    string(FIND "${INPUT}" "${START}" _first)
    string(FIND "${INPUT}" "${START}" _last REVERSE)
    string(FIND "${INPUT}" "${END}" _end)
    string(FIND "${INPUT}" "${END}" _end_last REVERSE)
    if(_first LESS 0 OR NOT _first EQUAL _last OR
        _end LESS_EQUAL _first OR NOT _end EQUAL _end_last)
        message(FATAL_ERROR "TSR history actual-owner bounds must be unique: ${START}")
    endif()
    math(EXPR _size "${_end} - ${_first}")
    string(SUBSTRING "${INPUT}" ${_first} ${_size} _body)
    set(${OUTPUT} "${_body}" PARENT_SCOPE)
endfunction()

function(_jce_tsr_history_replace OUTPUT INPUT FROM TO)
    string(FIND "${INPUT}" "${FROM}" _first)
    string(FIND "${INPUT}" "${FROM}" _last REVERSE)
    if(_first LESS 0 OR NOT _first EQUAL _last)
        message(FATAL_ERROR "TSR history mutation requires one actual production wire")
    endif()
    string(REPLACE "${FROM}" "${TO}" _body "${INPUT}")
    set(${OUTPUT} "${_body}" PARENT_SCOPE)
endfunction()

_jce_tsr_history_extract(_tsr_prepare "${_tsr_scene_source}"
    "    /* r.upscaler (LIVE):" "    /* TSR v3:")
_jce_tsr_history_extract(_tsr_resolve "${_tsr_scene_source}"
    "    if (s_sr.present && s_view_id_override == UINT16_MAX &&\n        width < disp_w && height < disp_h && tsr_active) {"
    "    } else if (upscaler == 1 && s_sr.present")
string(APPEND _tsr_resolve "    }\n")
_jce_tsr_history_extract(_tsr_scene_invalidate "${_tsr_scene_source}"
    "\nvoid jce_editor_scene_render_invalidate_model_caches(void)"
    "\nvoid jce_editor_scene_render_reset_occlusion(void)")
_jce_tsr_history_extract(_tsr_camera_set "${_tsr_camera_source}"
    "\nvoid jce_editor_scene_camera_set_state("
    "\nvoid jce_editor_scene_camera_set_projection(")
_jce_tsr_history_extract(_tsr_camera_projection "${_tsr_camera_source}"
    "\nvoid jce_editor_scene_camera_set_projection("
    "\nbool jce_editor_scene_camera_restore_pose(")
_jce_tsr_history_extract(_tsr_lifecycle "${_tsr_scene_source}"
    "static void JCE_CALL sr_temporal_lifecycle_event("
    "/* ── Init ─")
_jce_tsr_history_extract(_tsr_init_guard "${_tsr_scene_source}"
    "\nbool jce_editor_scene_render_init("
    "    memset(&s_sr, 0, sizeof(s_sr));")
_jce_tsr_history_extract(_tsr_init_tail "${_tsr_scene_source}"
    "    s_sr.initialized = true;\n"
    "/* ── Shutdown ─")
set(_tsr_init "${_tsr_init_guard}${_tsr_init_tail}")
_jce_tsr_history_extract(_tsr_shutdown "${_tsr_scene_source}"
    "\nvoid jce_editor_scene_render_shutdown(void)"
    "\nJceSceneRenderer *jce_editor_get_scene_renderer(void)")
_jce_tsr_history_extract(_tsr_view_destroy "${_tsr_scene_source}"
    "static void sr_destroy_ui_canvas(void)"
    "static void sr_cleanup_failed_init(void)")
_jce_tsr_history_extract(_tsr_query_cache "${_tsr_scene_source}"
    "#define ED_QUERY_CACHE_MAX"
    "struct SceneContentContext {")

# Constants remain extracted from their actual owners, not copied test values.
set(_tsr_constants "")
foreach(_tsr_constant IN ITEMS ORBIT_PITCH_MAX ORBIT_DIST_MIN ORBIT_DIST_MAX)
    string(REGEX MATCHALL "#define ${_tsr_constant}[^\n]*\n" _tsr_matches "${_tsr_camera_source}")
    list(LENGTH _tsr_matches _tsr_count)
    if(NOT _tsr_count EQUAL 1)
        message(FATAL_ERROR "TSR camera fixture requires actual ${_tsr_constant}")
    endif()
    string(APPEND _tsr_constants "${_tsr_matches}")
endforeach()
string(REGEX MATCHALL "JCE_EDITOR_VP_TSR_OFFSET[ ]+=[ ]+[0-9]+," _tsr_matches "${_tsr_viewport_source}")
list(LENGTH _tsr_matches _tsr_count)
if(NOT _tsr_count EQUAL 1)
    message(FATAL_ERROR "TSR fixture requires one actual viewport offset")
endif()
string(REGEX REPLACE ",[ ]*$" ";" _tsr_offset "${_tsr_matches}")
string(APPEND _tsr_constants "constexpr uint16_t ${_tsr_offset}\n")

foreach(_tsr_part IN ITEMS prepare resolve scene_invalidate camera_set camera_projection constants
    lifecycle init shutdown view_destroy query_cache)
    jce_test_write_if_changed("${_tsr_dir}/${_tsr_part}.inc.h" "${_tsr_${_tsr_part}}")
endforeach()

function(_jce_tsr_history_fixture NAME KIND DIRECTORY)
    jce_add_unit_test(${NAME} KIND ${KIND}
        SOURCES test_jce_editor_tsr_history.cpp
            "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_camera.c"
            "${CMAKE_SOURCE_DIR}/engine/src/renderer/jce_taa.c"
            "${CMAKE_SOURCE_DIR}/engine/src/application/jce_lifecycle.c"
        INCLUDES "${CMAKE_SOURCE_DIR}/editor/src" "${CMAKE_SOURCE_DIR}/engine/src"
        LINK jce_core)
    target_compile_features(${NAME} PRIVATE cxx_std_20)
    target_include_directories(${NAME} BEFORE PRIVATE "${DIRECTORY}" "${_tsr_dir}")
    set_property(TEST ${NAME} APPEND PROPERTY LABELS cxx)
    if(KIND STREQUAL "contract")
        set_tests_properties(${NAME} PROPERTIES WILL_FAIL TRUE)
    endif()
endfunction()
_jce_tsr_history_fixture(test_jce_editor_tsr_history unit "${_tsr_dir}")

# Restore old unconditional reuse without copying an alternative test model.
_jce_tsr_history_extract(_tsr_admission "${_tsr_prepare}"
    "        tsr_has_prev = tsr_previous_valid" "        jce_taa_advance(")
_jce_tsr_history_replace(_tsr_old_admission "${_tsr_prepare}" "${_tsr_admission}"
    "        tsr_has_prev = true;\n")
file(MAKE_DIRECTORY "${_tsr_dir}/old-admission")
jce_test_write_if_changed("${_tsr_dir}/old-admission/prepare.inc.h" "${_tsr_old_admission}")
_jce_tsr_history_fixture(test_jce_editor_tsr_history_old_admission contract "${_tsr_dir}/old-admission")

# Move the actual metadata commit back ahead of the possibly failing resolve.
_jce_tsr_history_extract(_tsr_commit "${_tsr_resolve}"
    "                /* Retain only a successful consecutive CPU submission. */"
    "            }\n        }\n")
set(_tsr_copy_wire [=[        if (tsr_has_prev)
            memcpy(tsr_prev_vp, s_sr.tsr_history.previous_view_projection.raw[0], sizeof(tsr_prev_vp));
]=])
_jce_tsr_history_replace(_tsr_early_prepare "${_tsr_prepare}" "${_tsr_copy_wire}"
    "${_tsr_copy_wire}${_tsr_commit}")
_jce_tsr_history_replace(_tsr_early_resolve "${_tsr_resolve}" "${_tsr_commit}" "")
file(MAKE_DIRECTORY "${_tsr_dir}/early-commit")
jce_test_write_if_changed("${_tsr_dir}/early-commit/prepare.inc.h" "${_tsr_early_prepare}")
jce_test_write_if_changed("${_tsr_dir}/early-commit/resolve.inc.h" "${_tsr_early_resolve}")
_jce_tsr_history_fixture(test_jce_editor_tsr_history_early_commit contract "${_tsr_dir}/early-commit")

_jce_tsr_history_replace(_tsr_missing_lifecycle "${_tsr_lifecycle}"
    "        state->tsr_history.valid = false;" "        (void)state;")
file(MAKE_DIRECTORY "${_tsr_dir}/missing-lifecycle")
jce_test_write_if_changed("${_tsr_dir}/missing-lifecycle/lifecycle.inc.h" "${_tsr_missing_lifecycle}")
_jce_tsr_history_fixture(test_jce_editor_tsr_history_missing_lifecycle contract "${_tsr_dir}/missing-lifecycle")
_jce_tsr_history_replace(_tsr_missing_unsubscribe "${_tsr_lifecycle}"
    "    if (listener.id) jce_lifecycle_unregister(listener);" "    (void)listener;")
file(MAKE_DIRECTORY "${_tsr_dir}/missing-unsubscribe")
jce_test_write_if_changed("${_tsr_dir}/missing-unsubscribe/lifecycle.inc.h" "${_tsr_missing_unsubscribe}")
_jce_tsr_history_fixture(test_jce_editor_tsr_history_missing_unsubscribe contract "${_tsr_dir}/missing-unsubscribe")
foreach(_tsr_var IN ITEMS scene camera state viewport dir scene_source camera_source viewport_source
    prepare resolve scene_invalidate camera_set camera_projection constants constant matches count offset part
    admission old_admission commit copy_wire early_prepare early_resolve lifecycle init_guard init_tail init
    shutdown view_destroy query_cache missing_lifecycle missing_unsubscribe)
    unset(_tsr_${_tsr_var})
endforeach()
unset(_tsr_var)
