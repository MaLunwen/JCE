# Complete actual follow/update/content owners, without a renderer or GUI.
function(_jce_loose_content_extract TEXT START END OUT)
    string(FIND "${TEXT}" "${START}" _begin)
    string(FIND "${TEXT}" "${START}" _last REVERSE)
    if(_begin LESS 0 OR NOT _begin EQUAL _last)
        message(FATAL_ERROR "Loose content fixture requires unique actual start: ${START}")
    endif()
    string(SUBSTRING "${TEXT}" ${_begin} -1 _tail)
    string(FIND "${_tail}" "${END}" _length)
    if(_length LESS_EQUAL 0)
        message(FATAL_ERROR "Loose content fixture requires actual end: ${END}")
    endif()
    string(SUBSTRING "${_tail}" 0 ${_length} _body)
    set(${OUT} "${_body}" PARENT_SCOPE)
endfunction()

set(_loose_scene_owner "${CMAKE_SOURCE_DIR}/editor/src/io/jce_editor_scene_serial.cpp")
set(_loose_state_owner "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_state.cpp")
set(_loose_render_owner "${CMAKE_SOURCE_DIR}/editor/src/scene/jce_editor_scene_render.cpp")
set(_loose_game_owner "${CMAKE_SOURCE_DIR}/editor/src/scene/jce_editor_game_render.cpp")
set(_loose_asset_owner "${CMAKE_SOURCE_DIR}/editor/src/panels/jce_panel_assets_grid.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_loose_scene_owner}" "${_loose_state_owner}" "${_loose_render_owner}"
    "${_loose_game_owner}" "${_loose_asset_owner}")
file(READ "${_loose_scene_owner}" _loose_scene)
file(READ "${_loose_state_owner}" _loose_state)
file(READ "${_loose_render_owner}" _loose_render)
file(READ "${_loose_game_owner}" _loose_game)
file(READ "${_loose_asset_owner}" _loose_asset)
_jce_loose_content_extract("${_loose_asset}"
    "static void open_asset_entry(const FileEntry &fe)"
    "\nstatic bool detect_open_trigger" _loose_asset_entry)
_jce_loose_content_extract("${_loose_scene}"
    "static bool prepare_scene_project_switch(const char *content_path,"
    "\nstatic bool scene_document_admitted" _loose_prepare)
_jce_loose_content_extract("${_loose_scene}"
    "static bool follow_scene_project_root(const char *scene_path,"
    "/* ── Prefab source instantiation" _loose_follow)
_jce_loose_content_extract("${_loose_state}"
    "void update_scene_dir_from_path(const char *scene_path)"
    "\nvoid clear_scene_entities" _loose_update)
_jce_loose_content_extract("${_loose_render}"
    "struct SceneContentContext {"
    "/* These are the same owned objects" _loose_apply)
_jce_loose_content_extract("${_loose_render}"
    "void jce_editor_scene_set_scene_dir(const char *dir)"
    "/* ── Ghost (drag-preview)" _loose_set_dir)
_jce_loose_content_extract("${_loose_game}"
    "    /* Same story, one step further along:"
    "    /* Game view always uses" _loose_game_apply)
foreach(_loose_entry IN ITEMS sync async)
    if(_loose_entry STREQUAL "sync")
        set(_loose_start "bool jce_state_load_scene_file(const char *scene_path)")
        set(_loose_end "\nstruct DeferredSceneLoad")
    else()
        set(_loose_start "bool jce_state_load_scene_file_async(const char *scene_path)")
        set(_loose_end "\nbool jce_state_is_scene_loading")
    endif()
    _jce_loose_content_extract("${_loose_scene}" "${_loose_start}" "${_loose_end}" _loose_entry_body)
    set(_loose_previous -1)
    foreach(_loose_wire IN ITEMS "scene_document_admitted(" "prepare_scene_project_switch(scene_path)"
            "jce_editor_scene_render_streaming_teardown();" "close_active_bundle_mount();"
            "follow_scene_project_root(scene_path)" "clear_scene_entities();")
        string(FIND "${_loose_entry_body}" "${_loose_wire}" _loose_position)
        if(NOT _loose_position GREATER _loose_previous)
            message(FATAL_ERROR "Loose ${_loose_entry} admission must precede retirement and entity creation: ${_loose_wire}")
        endif()
        set(_loose_previous ${_loose_position})
    endforeach()
endforeach()
set(_loose_body "${_loose_apply}\n${_loose_set_dir}\n${_loose_update}\n${_loose_prepare}\n${_loose_follow}\n${_loose_asset_entry}\nvoid consume_game_content_context()\n{\n${_loose_game_apply}\n}\n")

# Mutation boundaries are actual wires in complete owners, never substitutes
# for the normal entry or compiler failures as an accepted negative.
set(_loose_old_preflight "${_loose_body}")
set(_loose_old_game "${_loose_body}")
set(_loose_old_binding "${_loose_body}")
set(_loose_preflight_wire "return jce_editor_prepare_project_switch(nullptr);")
set(_loose_game_wire "proj ? proj->cooked_assets : nullptr,\n                jce_editor_scene_content_root());")
set(_loose_binding_wire "        if (!set_current_project_root(nullptr)) return false;\n        update_scene_dir_from_path(scene_path);")
set(_loose_asset_wire "        jce_state_load_scene_file(fe.path.c_str());")
set(_loose_bundle_wire "        if (preserve_current_project) return true;\n        /* No manifest is a real transition")
foreach(_loose_mutation IN ITEMS preflight game binding asset bundle)
    string(FIND "${_loose_body}" "${_loose_${_loose_mutation}_wire}" _loose_first)
    string(FIND "${_loose_body}" "${_loose_${_loose_mutation}_wire}" _loose_last REVERSE)
    if(_loose_first LESS 0 OR NOT _loose_first EQUAL _loose_last)
        message(FATAL_ERROR "Loose scene control requires one actual ${_loose_mutation} wire")
    endif()
endforeach()
string(REPLACE "${_loose_preflight_wire}" "return true; /* old no-manifest preflight */"
    _loose_old_preflight "${_loose_body}")
string(REPLACE "${_loose_game_wire}" "proj ? proj->cooked_assets : nullptr);"
    _loose_old_game "${_loose_body}")
string(REPLACE "${_loose_binding_wire}" "        if (!set_current_project_root(nullptr)) return false;"
    _loose_old_binding "${_loose_body}")
string(REPLACE "${_loose_asset_wire}" [=[        jce_state_load_scene_file(fe.path.c_str());
        std::string dir = fe.path;
        size_t sep = dir.find_last_of("/\\");
        if (sep != std::string::npos) dir.resize(sep);
        jce_editor_scene_set_scene_dir(dir.c_str());]=]
    _loose_old_asset_entry "${_loose_body}")
string(REPLACE "${_loose_bundle_wire}" "        /* No manifest is a real transition"
    _loose_old_bundle_retire "${_loose_body}")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/loose-scene-content")
set(_loose_actual "${_loose_body}")
foreach(_loose_variant IN ITEMS actual old_preflight old_game old_binding old_asset_entry old_bundle_retire)
    set(_loose_header "${CMAKE_CURRENT_BINARY_DIR}/loose-scene-content/${_loose_variant}.inc.h")
    jce_test_write_if_changed("${_loose_header}" "${_loose_${_loose_variant}}")
    set(_loose_target test_jce_editor_loose_scene_content)
    set(_loose_kind integration)
    if(NOT _loose_variant STREQUAL "actual")
        string(APPEND _loose_target "_${_loose_variant}")
        set(_loose_kind contract)
    endif()
    jce_add_cxx_test(${_loose_target} KIND ${_loose_kind}
        SOURCES test_jce_editor_loose_scene_content.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_project_context.cpp"
            "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_path_util.cpp"
            "${CMAKE_SOURCE_DIR}/editor/src/scene/jce_scene_content_context.cpp"
        LINK jce_core jce_application)
    target_compile_definitions(${_loose_target} PRIVATE
        JCE_TEST_EDITOR_LOOSE_CONTENT_BODY="${_loose_header}")
    if(_loose_kind STREQUAL "contract")
        set_tests_properties(${_loose_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()

set(_loose_quality_owner "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_effective_render_settings.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_loose_quality_owner}")
file(READ "${_loose_quality_owner}" _loose_quality)
set(_loose_quality_wire "const bool loose = !isolated && (!project_root || !project_root[0]) &&")
string(FIND "${_loose_quality}" "${_loose_quality_wire}" _loose_first)
string(FIND "${_loose_quality}" "${_loose_quality_wire}" _loose_last REVERSE)
if(_loose_first LESS 0 OR NOT _loose_first EQUAL _loose_last)
    message(FATAL_ERROR "Loose quality control requires its one actual policy wire")
endif()
string(REPLACE "${_loose_quality_wire}"
    "const bool loose = false && !isolated && (!project_root || !project_root[0]) &&"
    _loose_old_quality "${_loose_quality}")
set(_loose_quality_path "${CMAKE_CURRENT_BINARY_DIR}/loose-scene-content/old_quality.cpp")
jce_test_write_if_changed("${_loose_quality_path}" "${_loose_old_quality}")
_jce_editor_render_settings_vfs_fixture(test_jce_editor_loose_scene_old_quality
    "${_loose_quality_path}" contract)
set_tests_properties(test_jce_editor_loose_scene_old_quality PROPERTIES WILL_FAIL TRUE)
foreach(_loose_var IN ITEMS scene_owner state_owner render_owner game_owner asset_owner scene state render
        game asset asset_entry prepare follow update apply set_dir game_apply body header preflight_wire game_wire
        binding_wire old_preflight old_game old_binding actual mutation first last variant target kind
        quality_owner quality quality_wire old_quality quality_path entry start end entry_body
        previous wire position asset_wire old_asset_entry bundle_wire old_bundle_retire)
    unset(_loose_${_loose_var})
endforeach()
unset(_loose_var)
