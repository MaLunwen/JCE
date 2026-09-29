cmake_minimum_required(VERSION 3.20)

# ----------------------------------------------------------------------- #
#  Editor C++17 unit tests (doctest, vendored).                           #
#                                                                         #
#  Each test target is a small, focused exe — never link the umbrella     #
#  JCE_Editor target.  Re-compile only the editor source files the test   #
#  actually exercises, keeping link lines fast and decoupled.             #
#                                                                         #
#  Convention mirrors tests/middleware/* :                                #
#    jce_add_cxx_test(<name> SOURCES test_*.cpp [editor_src.cpp ...]      #
#                            LINK jce_core [other engine libs])           #
# ----------------------------------------------------------------------- #

# Pure header-only quaternion <-> Euler helpers.
jce_add_cxx_test(test_jce_editor_quat
    SOURCES test_jce_editor_quat.cpp
    LINK    jce_core)

# Pure path-relativization (extern "C" function in the editor).  We compile
# the editor .cpp into the test exe directly so the helper stays decoupled
# from JCE_Editor's huge link line (bgfx, SDL_image, ImGui, ...).
jce_add_cxx_test(test_jce_editor_path_util
    SOURCES test_jce_editor_path_util.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_path_util.cpp"
            test_stub_assets_get_project.cpp
    LINK    jce_core)

# Hierarchy root set - incremental maintenance policy (header-only, no scene
# or ImGui dependency; the test injects its own parent oracle).
jce_add_cxx_test(test_jce_editor_root_set
    SOURCES test_jce_editor_root_set.cpp
    LINK    jce_core)

# Editor allocator macros (header-only wrapper around the engine vtable).
jce_add_cxx_test(test_jce_editor_alloc
    SOURCES test_jce_editor_alloc.cpp
    LINK    jce_core)

# Editor file-IO helpers (header-only, wraps jce_fs_host_*).
jce_add_cxx_test(test_jce_editor_file_util
    SOURCES test_jce_editor_file_util.cpp
    LINK    jce_core)

# Editor i18n facade — uninitialized-state contract + key/id helpers.
# Compile the editor .cpp directly; depends on jce_json + jce_pak_loader.
jce_add_cxx_test(test_jce_editor_i18n
    SOURCES test_jce_editor_i18n.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_i18n.cpp"
    LINK    jce_core jce_resource)

# Editor config persistence — defaults + recent-list MRU helpers.
# Pure in-memory logic; compile the editor .cpp directly. Depends on
# jce_json + jce_editor_file_util (header-only) + jce_filesystem.
jce_add_cxx_test(test_jce_editor_config
    SOURCES test_jce_editor_config.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_config.cpp"
    LINK    jce_core jce_platform)

# Editor "▶ Run" launch path — which binary Run actually resolves to.
# Pure: the resolution ORDER lives in its own TU behind an injected existence
# predicate, so this needs no build_manager / assetdb / panels / console stubs.
# See editor/src/core/jce_run_manager_internal.h for why that split exists —
# before it, the launch path had zero test coverage.
jce_add_cxx_test(test_jce_editor_run_resolve
    SOURCES test_jce_editor_run_resolve.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/core/jce_run_manager_resolve.cpp")

# Editor asset DB — kind_label / set_root / get_kind fallback +
# real-filesystem rescan + reverse-ref lookup. No ImGui surface.
# jce_assetdb.cpp classifies via the engine-canonical extension table
# (REF-008).  That TU is deliberately dependency-free, so it is compiled in
# directly rather than linking the whole jce_resource layer into this small
# editor unit test (same approach as test_jce_cook_incremental).
jce_add_cxx_test(test_jce_editor_assetdb
    SOURCES test_jce_editor_assetdb.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/core/jce_assetdb.cpp"
            "${CMAKE_SOURCE_DIR}/editor/src/core/jce_asset_ref_rewrite.cpp"
            "${CMAKE_SOURCE_DIR}/engine/src/resource/jce_asset_ext.c"
    LINK    jce_core)

# Editor project-settings — defaults block-by-block + apply/current cache.
# Persistence paths skipped (would write to .jce/ in cwd).
# jce_project_settings.cpp now owns the single authoring->runtime conversion for
# the physics layer matrix (REF-017), so it calls jce_physics_layer_set_name /
# jce_physics_set_layer_collides.  Compiling jce_physics_layers.c in directly
# does not work: it calls jce_physics_body_set_collision_filter from the physics
# core, so the whole layer is linked instead.
jce_add_cxx_test(test_jce_editor_project_settings
    SOURCES test_jce_editor_project_settings.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/core/jce_project_settings.cpp"
    LINK    jce_core jce_physics jce_resource)

# Dist artifact audit primitives: PE subsystem parsing and path leakage
# scanning are pure byte operations, independent of the editor UI.
jce_add_cxx_test(test_jce_dist_audit
    SOURCES test_jce_dist_audit.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/core/jce_dist_audit.cpp"
    LINK    jce_core jce_resource)

# Dist content is rooted by the same dependency graph/cooked bytes as Bundle,
# never by a second recursive directory scan.
jce_add_cxx_test(test_jce_dist_content_graph
    SOURCES test_jce_dist_content_graph.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/core/jce_dist_content_graph.cpp"
            "${CMAKE_SOURCE_DIR}/editor/src/core/jce_dist_audit.cpp"
    LINK    jce_core jce_resource jce_application)

# Deterministic single-executable archive wrappers.  Validate generated C,
# COFF object, and assembler incbin contracts without invoking a toolchain.
jce_add_cxx_test(test_jce_binary_embed
    SOURCES test_jce_binary_embed.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/core/jce_binary_embed.cpp"
    LINK    jce_core)

# Project rendering defaults seed new scenes, while authored scene
# rendering settings remain scene-owned.
jce_add_cxx_test(test_jce_editor_scene_rendering_defaults
    SOURCES test_jce_editor_scene_rendering_defaults.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/core/jce_project_settings.cpp"
            "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_scene_rendering_defaults.cpp"
    LINK    jce_core jce_scene jce_resource)

# The per-level Anti Aliasing combo used to decide nothing: one line below the
# seven fields it composes from the level, the resolver overwrote msaa with
# graphics.default_msaa unconditionally.
jce_add_cxx_test(test_jce_editor_quality_msaa
    SOURCES test_jce_editor_quality_msaa.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/core/jce_project_settings.cpp"
            "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_effective_render_settings.cpp"
            "${CMAKE_SOURCE_DIR}/editor/src/scene/jce_scene_content_context.cpp"
    LINK    jce_core jce_scene jce_renderer jce_resource)

# Project RP ownership: a project without a valid render-pipeline asset must
# never inherit the descriptor previously applied by another project.
jce_add_cxx_test(test_jce_editor_project_render_pipeline
    SOURCES test_jce_editor_project_render_pipeline.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_project_render_pipeline.cpp"
    LINK    jce_core jce_renderer jce_resource)

# Same-editor-executable automation request parser for secure project builds.
# The parser is UI-free and validates the complete environment contract before
# the lifecycle driver can mutate editor project state or start a build.
jce_add_cxx_test(test_jce_editor_headless_build_request
    SOURCES test_jce_editor_headless_build_request.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_headless_build_request.cpp"
    LINK    jce_core)

jce_add_cxx_test(test_jce_editor_headless_build_driver
    SOURCES test_jce_editor_headless_build_driver.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_headless_build_driver.cpp"
    LINK    jce_core)

# Asset discovery must not copy repository instructions, build scripts, or
# host metadata into runtime archives.
# jce_asset_ext.c carries the engine-canonical script-extension table the
# policy consults, for the same reason jce_assetdb.cpp does above: a
# publication rule that spells out its own extension list is how .py ended
# up stripped from packaged builds while the editor kept running it loose.
jce_add_cxx_test(test_jce_build_asset_policy
    SOURCES test_jce_build_asset_policy.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/core/jce_build_asset_policy.cpp"
            "${CMAKE_SOURCE_DIR}/engine/src/resource/jce_asset_ext.c")

# Editor component descriptor registry — gives flagless components such as
# Compound Collider the same component identity path as legacy flag entries.
# The defaults TU is compiled in too (it is ImGui-free) so the parity tests
# can assert that every add_default hook actually BOUND.  Those hooks are
# installed by engine-name string and the setter silently no-ops on a miss,
# so without linking it the drift it guards against would be untestable.
jce_add_cxx_test(test_jce_editor_component_registry
    SOURCES test_jce_editor_component_registry.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_component_registry.cpp"
            "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_component_defaults.cpp"
    LINK    jce_core jce_scene jce_resource)

# Editor layout scene commands — File > New Scene / command palette /
# Ctrl+N must create an untitled default scene instead of opening the
# save-name dialog.
jce_add_cxx_test(test_jce_editor_layout_scene_commands
    SOURCES test_jce_editor_layout_scene_commands.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/ui/jce_editor_layout_scene_commands.cpp"
    LINK    jce_core)

# Reflect type registry — pure register/find/count/at, no ImGui surface.
# Split out of jce_reflect.cpp to keep the registry testable in isolation;
# the ImGui drawer now lives in jce_reflect_draw.cpp.
jce_add_cxx_test(test_jce_editor_reflect_registry
    SOURCES test_jce_editor_reflect_registry.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/core/jce_reflect.cpp"
    LINK    jce_core)

# Scene View focus math — pure target fitting, repeat-zoom stepping,
# and smooth animation sampling.
jce_add_cxx_test(test_jce_scene_camera_focus
    SOURCES test_jce_scene_camera_focus.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/scene/jce_scene_camera_focus.cpp"
    LINK    jce_core)

# Camera Align/Pilot pose math stays independent of editor globals and ImGui.
jce_add_cxx_test(test_jce_editor_camera_tools
    SOURCES test_jce_editor_camera_tools.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/scene/jce_editor_camera_tools.cpp"
    LINK    jce_core)

# Scene resource ownership: bundle previews must use relative VFS addresses
# and must never inherit the previously opened project's absolute roots.
jce_add_cxx_test(test_jce_scene_content_context
    SOURCES test_jce_scene_content_context.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/scene/jce_scene_content_context.cpp"
    LINK    jce_core)

# Scene selection outline fallback policy — visual renderer entities must
# not be represented by debug placeholder boxes when true geometry is absent.
jce_add_cxx_test(test_jce_scene_outline_policy
    SOURCES test_jce_scene_outline_policy.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/scene/jce_scene_outline_policy.cpp"
    LINK    jce_core)

# Scene View input ownership — scene picking must not react to clicks
# owned by the Asset Browser, Inspector, modal popups, or other panels.
jce_add_cxx_test(test_jce_scene_view_input_policy
    SOURCES test_jce_scene_view_input_policy.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/panels/jce_scene_view_input_policy.cpp"
    LINK    jce_core)

# Hierarchy panel keyboard routing — alpha-jump must not also consume
# keys already handled as hierarchy hotkeys, e.g. F = frame selected.
jce_add_cxx_test(test_jce_hierarchy_input
    SOURCES test_jce_hierarchy_input.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/panels/jce_panel_hierarchy_input.cpp"
    LINK    jce_core
    INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/stub_includes")

# Prefab per-COMPONENT + per-FIELD override core — byte-diff an instance
# against an in-memory source, JSON-level field diff/merge (json_deep_equal /
# compute_field_overrides / merge_fields), write/overlay the additive
# "overrides" node (bare-string legacy + { component, fields } per-field
# forms), the field-propagation payoff, and the legacy backward-compat
# guards.  The override module is pure
# (explicit JceScene*, no editor globals / ImGui), so compile it directly
# and link the scene stack (jce_scene needs jce_resource to close its
# private renderer->resource edge, same as the scene-layer tests).
jce_add_cxx_test(test_jce_editor_prefab_override
    SOURCES test_jce_editor_prefab_override.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/io/jce_editor_prefab_override.cpp"
    LINK    jce_core jce_scene jce_resource)

# Editor hotkey registry — pure name/id/get/set/reset/chord_label paths.
# We compile hotkeys.cpp directly and shadow <jce/tools/jce_imgui.hpp>
# with a test-only stub so the editor TU links without pulling in the
# entire Dear ImGui SDK or the Tools backend.
jce_add_cxx_test(test_jce_hotkeys
    SOURCES  test_jce_hotkeys.cpp
             test_stub_imgui_funcs.cpp
             "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_config.cpp"
             "${CMAKE_SOURCE_DIR}/editor/src/core/jce_hotkeys.cpp"
    LINK     jce_core jce_platform
    INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/stub_includes")

# Game View autonomous capture schedule — pure parser/timer state machine.
jce_add_cxx_test(test_jce_editor_kpi_game_capture
    SOURCES test_jce_editor_kpi_game_capture.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_kpi_game_capture.cpp"
    LINK    jce_core jce_application)

# Game View input ownership + pre-step commit order.  No ImGui/SDL/renderer.
jce_add_cxx_test(test_jce_editor_game_input_bridge
    SOURCES test_jce_editor_game_input_bridge.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_game_input_bridge.cpp"
    LINK    jce_core)

# Input Manager device strip — the battery gate and the two meanings of a
# false rumble return, driven against a real JceInput with a fake backend in
# its effector slots.  jce_platform carries the device layer.
jce_add_cxx_test(test_jce_editor_device_strip
    SOURCES test_jce_editor_device_strip.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_device_strip.cpp"
    LINK    jce_platform jce_core)

# Terrain data is side-car binary state rather than scene JSON.  The editor
# records bounded native-grid tile/channel deltas so long brush strokes remain
# undoable without duplicating an entire heightmap per stamp.
jce_add_cxx_test(test_jce_terrain_history
    SOURCES test_jce_terrain_history.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/panels/jce_terrain_history.cpp"
    LINK    jce_core jce_scene jce_resource)

# Renaming an asset in the Asset Browser called jce_fs_host_rename and touched
# nothing else, so every scene/prefab/material pointing at the old name went
# silently to a missing asset -- and the fuzzy basename index cannot help,
# because a rename is exactly the case where the basename changed.  Most of
# these cases assert the rewrite does NOT fire: a textual rewrite that is too
# eager corrupts working references in files the user never opened.
jce_add_cxx_test(test_jce_asset_ref_rewrite
    SOURCES test_jce_asset_ref_rewrite.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/core/jce_asset_ref_rewrite.cpp"
    LINK    jce_core)

# Inspector combo index <-> engine enum.  The Rigidbody2D body-type combo
# stored its raw ImGui row index while JceBodyType is STATIC/DYNAMIC/KINEMATIC
# and the list reads static/kinematic/dynamic, so two of the three rows meant
# the opposite of their label -- and the panel agreed with itself in both
# directions, so only the physics disagreed.  The map is a table in a TU with
# no ImGui in it precisely so this can link it on its own.
jce_add_cxx_test(test_jce_inspector_enum_maps
    SOURCES test_jce_inspector_enum_maps.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/panels/jce_panel_inspector_enum_maps.cpp"
    LINK    jce_core)

# What the shader graph GENERATES.  Three of its nodes were built and
# unwireable in the same way: the node existed, its GLSL was correct, and
# nothing could consume the result -- the Normal Map had no destination
# socket, the UV node produced a lone float no input accepted, and the
# per-node texture path was serialised and read by nobody, so two Texture
# nodes aimed at different maps sampled the same one.  None of that is visible
# on the canvas: the node sits there, the link snaps in, and the generated
# shader quietly says something else.  The generated TEXT is the artefact.
jce_add_cxx_test(test_jce_shadergraph_codegen
    SOURCES test_jce_shadergraph_codegen.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/shadergraph/jce_shadergraph_codegen.cpp"
            "${CMAKE_SOURCE_DIR}/editor/src/shadergraph/jce_shadergraph_registry.cpp"
            "${CMAKE_SOURCE_DIR}/editor/src/shadergraph/jce_shadergraph_graph.cpp"
            "${CMAKE_SOURCE_DIR}/editor/src/shadergraph/jce_shadergraph_topo.cpp"
            "${CMAKE_SOURCE_DIR}/editor/src/shadergraph/jce_shadergraph_typecheck.cpp"
    LINK    jce_core)
# The template path as a DEFINE, not a search: ctest's working directory is
# the build tree and a relative-path hunt is a test that passes or fails on
# where it was launched from.
target_compile_definitions(test_jce_shadergraph_codegen PRIVATE
    JCE_SG_TEMPLATE="${CMAKE_SOURCE_DIR}/engine/shaders/graph/fs_graph_template.sc")
