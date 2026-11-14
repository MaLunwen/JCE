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

include("${CMAKE_CURRENT_SOURCE_DIR}/project_run_profile_controls.cmake")

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

# Editor project-settings — defaults, apply/current and root-scoped cache.
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
include(project_settings_scope_controls.cmake)

# Dist artifact audit primitives: PE subsystem parsing and path leakage
# scanning are pure byte operations, independent of the editor UI.
jce_add_cxx_test(test_jce_dist_audit
    SOURCES test_jce_dist_audit.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/core/jce_dist_audit.cpp"
    INCLUDES "${CMAKE_SOURCE_DIR}/editor/src/core"
    LINK    jce_core jce_resource)

# Compile the complete actual PE audit owner after removing only one refusal
# block. These controls retain the same secure PAK/files/ordinary assertions.
set(_dist_pe_owner "${CMAKE_SOURCE_DIR}/editor/src/core/jce_dist_audit.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_dist_pe_owner}")
file(READ "${_dist_pe_owner}" _dist_pe_source)
set(_dist_pe_machine_refusal [=[                if (input.expected_pe_machine != r.pe_machine)
                    set_error(r, "Windows dist PE machine does not match requested architecture");
]=])
set(_dist_pe_import_refusal [=[                if (!r.pe_imports_verified)
                    set_error(r, imports.empty() ? "PE artifact has no imports; cannot verify delivery"
                              : "single EXE still requires external DLLs");
]=])
foreach(_dist_pe_variant IN ITEMS machine import)
    string(REPLACE "${_dist_pe_${_dist_pe_variant}_refusal}" ""
        _dist_pe_mutant "${_dist_pe_source}")
    string(LENGTH "${_dist_pe_source}" _dist_pe_source_length)
    string(LENGTH "${_dist_pe_mutant}" _dist_pe_mutant_length)
    string(LENGTH "${_dist_pe_${_dist_pe_variant}_refusal}" _dist_pe_refusal_length)
    math(EXPR _dist_pe_removed_length "${_dist_pe_source_length} - ${_dist_pe_mutant_length}")
    if(_dist_pe_refusal_length EQUAL 0 OR
            NOT _dist_pe_removed_length EQUAL _dist_pe_refusal_length)
        message(FATAL_ERROR "PE mutation requires one exact actual ${_dist_pe_variant} refusal block")
    endif()
    set(_dist_pe_mutant_path "${CMAKE_CURRENT_BINARY_DIR}/dist-pe-controls/jce_dist_audit_old_${_dist_pe_variant}.cpp")
    unset(_dist_pe_previous)
    if(EXISTS "${_dist_pe_mutant_path}")
        file(READ "${_dist_pe_mutant_path}" _dist_pe_previous)
    endif()
    if(NOT "${_dist_pe_previous}" STREQUAL "${_dist_pe_mutant}")
        file(WRITE "${_dist_pe_mutant_path}" "${_dist_pe_mutant}")
    endif()
    get_target_property(_dist_pe_sources test_jce_dist_audit SOURCES)
    set(_dist_pe_owners "${_dist_pe_sources}")
    list(FILTER _dist_pe_owners INCLUDE REGEX "/editor/src/core/jce_dist_audit[.]cpp$")
    list(LENGTH _dist_pe_owners _dist_pe_owner_count)
    if(NOT _dist_pe_owner_count EQUAL 1)
        message(FATAL_ERROR "Expected one complete actual Dist PE owner source")
    endif()
    list(REMOVE_ITEM _dist_pe_sources ${_dist_pe_owners})
    list(APPEND _dist_pe_sources "${_dist_pe_mutant_path}")
    set(_dist_pe_target "test_jce_dist_audit_old_${_dist_pe_variant}_wire")
    jce_add_cxx_test(${_dist_pe_target} KIND contract
        SOURCES ${_dist_pe_sources}
        INCLUDES "${CMAKE_SOURCE_DIR}/editor/src/core"
        LINK jce_core jce_resource)
    set_property(TARGET ${_dist_pe_target} PROPERTY
        JCE_DIST_PE_MUTANT_SOURCE "${_dist_pe_mutant_path}")
    set_tests_properties(${_dist_pe_target} PROPERTIES WILL_FAIL TRUE)
endforeach()
function(_jce_dist_pe_inherit_final_recipe)
    foreach(_target IN ITEMS test_jce_dist_audit_old_machine_wire test_jce_dist_audit_old_import_wire)
        foreach(_property IN ITEMS CXX_STANDARD CXX_STANDARD_REQUIRED CXX_EXTENSIONS
                MSVC_RUNTIME_LIBRARY INCLUDE_DIRECTORIES COMPILE_DEFINITIONS
                COMPILE_OPTIONS COMPILE_FLAGS LINK_LIBRARIES LINK_OPTIONS LINK_FLAGS)
            get_target_property(_value test_jce_dist_audit "${_property}")
            if(NOT _value STREQUAL "_value-NOTFOUND")
                set_property(TARGET ${_target} PROPERTY "${_property}" "${_value}")
            endif()
        endforeach()
        get_target_property(_prerequisites test_jce_dist_audit MANUALLY_ADDED_DEPENDENCIES)
        if(_prerequisites)
            add_dependencies(${_target} ${_prerequisites})
        endif()
        get_target_property(_sources test_jce_dist_audit SOURCES)
        get_target_property(_mutant ${_target} JCE_DIST_PE_MUTANT_SOURCE)
        foreach(_source IN LISTS _sources)
            set(_test_source "${_source}")
            if(_source STREQUAL "${CMAKE_SOURCE_DIR}/editor/src/core/jce_dist_audit.cpp")
                set(_test_source "${_mutant}")
            endif()
            foreach(_property IN ITEMS COMPILE_DEFINITIONS INCLUDE_DIRECTORIES
                    COMPILE_OPTIONS COMPILE_FLAGS LANGUAGE OBJECT_DEPENDS)
                get_source_file_property(_value "${_source}" TARGET_DIRECTORY test_jce_dist_audit "${_property}")
                if(NOT _value STREQUAL "NOTFOUND")
                    set_source_files_properties("${_test_source}" TARGET_DIRECTORY ${_target}
                        PROPERTIES "${_property}" "${_value}")
                endif()
            endforeach()
        endforeach()
    endforeach()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _jce_dist_pe_inherit_final_recipe)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME test_jce_dist_pe_policy
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/test_jce_dist_pe_policy.py")
set_tests_properties(test_jce_dist_pe_policy PROPERTIES TIMEOUT 60
    ENVIRONMENT "${JCE_TEST_ENVIRONMENT}" LABELS "editor-evidence;cpu")
jce_set_test_kind(test_jce_dist_pe_policy contract)
set_property(GLOBAL APPEND PROPERTY JCE_TESTS_WITH_ENV test_jce_dist_pe_policy)

# Dist content is rooted by the same dependency graph/cooked bytes as Bundle,
# never by a second recursive directory scan.
# Compile the actual manager adapter with real Bundle/Archive/host IO. Its
# old missing-confidential-wire mutation must fail the same ordinary assertions.
set(_dist_manager_owner "${CMAKE_SOURCE_DIR}/editor/src/core/jce_build_manager.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_dist_manager_owner}")
file(READ "${_dist_manager_owner}" _dist_manager_source)
foreach(_dist_struct IN ITEMS ProjectAssetInput ApprovedGraphAsset)
    # CMake lists split semicolons in C++ field bodies; count only signatures.
    string(REGEX MATCHALL "struct ${_dist_struct} [{]"
        _dist_struct_matches "${_dist_manager_source}")
    list(LENGTH _dist_struct_matches _dist_struct_count)
    if(NOT _dist_struct_count EQUAL 1)
        message(FATAL_ERROR "Dist adapter fixture requires unique actual ${_dist_struct}")
    endif()
    string(REGEX MATCH "struct ${_dist_struct} [{][^}]*[}];"
        _dist_${_dist_struct} "${_dist_manager_source}")
    if(_dist_${_dist_struct} STREQUAL "")
        message(FATAL_ERROR "Dist adapter fixture cannot extract actual ${_dist_struct} fields")
    endif()
endforeach()
string(REGEX MATCHALL "bool append_dist_bundle_graph[(]" _dist_adapter_signatures "${_dist_manager_source}")
list(LENGTH _dist_adapter_signatures _dist_adapter_count)
string(FIND "${_dist_manager_source}" "bool append_dist_bundle_graph(" _dist_adapter_begin)
string(FIND "${_dist_manager_source}" "\nbool write_asset_bom_json(" _dist_adapter_end)
if(NOT _dist_adapter_count EQUAL 1 OR _dist_adapter_begin LESS 0 OR
        _dist_adapter_end LESS_EQUAL _dist_adapter_begin)
    message(FATAL_ERROR "Dist adapter actual-body extraction bounds are invalid")
endif()
math(EXPR _dist_adapter_size "${_dist_adapter_end} - ${_dist_adapter_begin}")
string(SUBSTRING "${_dist_manager_source}" ${_dist_adapter_begin} ${_dist_adapter_size} _dist_adapter_body)
string(CONCAT _dist_adapter_normal "${_dist_ProjectAssetInput}\n${_dist_ApprovedGraphAsset}\n${_dist_adapter_body}")
set(_dist_confidential_wire "item.protect_path = std::binary_search(graph.confidential_paths.begin(),\n            graph.confidential_paths.end(), source.address);")
string(FIND "${_dist_adapter_body}" "${_dist_confidential_wire}" _dist_confidential_wire_pos)
if(_dist_confidential_wire_pos LESS 0)
    message(FATAL_ERROR "Dist adapter missing-wire control cannot find its actual confidential wire")
endif()
string(REPLACE "${_dist_confidential_wire}" "item.protect_path = false;"
    _dist_adapter_missing_wire "${_dist_adapter_normal}")
if(_dist_adapter_missing_wire STREQUAL _dist_adapter_normal)
    message(FATAL_ERROR "Dist adapter missing-wire mutation did not change the actual owner")
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/dist-name-policy-mutation")
foreach(_dist_variant IN ITEMS normal missing_wire)
    set(_dist_body_var "_dist_adapter_${_dist_variant}")
    set(_dist_body_path "${CMAKE_CURRENT_BINARY_DIR}/dist-name-policy-mutation/jce_dist_adapter_${_dist_variant}.h")
    unset(_dist_previous)
    if(EXISTS "${_dist_body_path}")
        file(READ "${_dist_body_path}" _dist_previous)
    endif()
    if(NOT _dist_previous STREQUAL ${_dist_body_var})
        file(WRITE "${_dist_body_path}" "${${_dist_body_var}}")
    endif()
endforeach()
set(_dist_prepared_owner "${CMAKE_SOURCE_DIR}/editor/src/core/jce_project_prepared_assets.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_dist_prepared_owner}")
file(READ "${_dist_prepared_owner}" _dist_prepared_source)
foreach(_dist_prepared_marker IN ITEMS "options->always_included =" "options->always_included_count =")
    string(REGEX MATCHALL "${_dist_prepared_marker}" _dist_prepared_wires "${_dist_prepared_source}")
    list(LENGTH _dist_prepared_wires _dist_prepared_wire_count)
    if(NOT _dist_prepared_wire_count EQUAL 1)
        message(FATAL_ERROR "Prepared-root mutation requires each unique actual bind wire")
    endif()
endforeach()
string(REPLACE "options->always_included = inputs->root_pointers.data();"
    "options->always_included = nullptr;" _dist_prepared_mutant_pointer "${_dist_prepared_source}")
if(_dist_prepared_mutant_pointer STREQUAL _dist_prepared_source)
    message(FATAL_ERROR "Prepared-root pointer mutation did not change the actual owner")
endif()
string(REPLACE "options->always_included_count = static_cast<int>(inputs->root_pointers.size());"
    "options->always_included_count = 0;" _dist_prepared_mutant "${_dist_prepared_mutant_pointer}")
if(_dist_prepared_mutant STREQUAL _dist_prepared_mutant_pointer)
    message(FATAL_ERROR "Prepared-root count mutation did not change the actual owner")
endif()
set(_dist_prepared_mutant_path "${CMAKE_CURRENT_BINARY_DIR}/dist-name-policy-mutation/jce_project_prepared_assets_missing_roots.cpp")
unset(_dist_prepared_previous)
if(EXISTS "${_dist_prepared_mutant_path}")
    file(READ "${_dist_prepared_mutant_path}" _dist_prepared_previous)
endif()
if(NOT _dist_prepared_previous STREQUAL _dist_prepared_mutant)
    file(WRITE "${_dist_prepared_mutant_path}" "${_dist_prepared_mutant}")
endif()
string(REPLACE "        {\"settings/project-settings.json\", \".jce/project-settings.json\"},\n"
    "" _dist_runtime_settings_mutant "${_dist_prepared_source}")
if(_dist_runtime_settings_mutant STREQUAL _dist_prepared_source)
    message(FATAL_ERROR "Runtime project-settings omission did not change the actual owner")
endif()
set(_dist_runtime_settings_mutant_path "${CMAKE_CURRENT_BINARY_DIR}/dist-name-policy-mutation/jce_project_prepared_assets_missing_runtime_settings.cpp")
unset(_dist_runtime_settings_previous)
if(EXISTS "${_dist_runtime_settings_mutant_path}")
    file(READ "${_dist_runtime_settings_mutant_path}" _dist_runtime_settings_previous)
endif()
if(NOT _dist_runtime_settings_previous STREQUAL _dist_runtime_settings_mutant)
    file(WRITE "${_dist_runtime_settings_mutant_path}" "${_dist_runtime_settings_mutant}")
endif()

foreach(_dist_target IN ITEMS test_jce_dist_content_graph test_jce_dist_content_graph_missing_confidential_wire
        test_jce_dist_content_graph_missing_prepared_roots test_jce_dist_content_graph_missing_runtime_settings)
    if(_dist_target STREQUAL "test_jce_dist_content_graph")
        set(_dist_variant normal)
        set(_dist_kind integration)
        set(_dist_fixture_prefix "dist_graph_normal")
        set(_dist_prepared_test_source "${_dist_prepared_owner}")
    elseif(_dist_target STREQUAL "test_jce_dist_content_graph_missing_confidential_wire")
        set(_dist_variant missing_wire)
        set(_dist_kind contract)
        set(_dist_fixture_prefix "dist_graph_no_names")
        set(_dist_prepared_test_source "${_dist_prepared_owner}")
    elseif(_dist_target STREQUAL "test_jce_dist_content_graph_missing_runtime_settings")
        set(_dist_variant normal)
        set(_dist_kind contract)
        set(_dist_fixture_prefix "dist_graph_no_runtime_settings")
        set(_dist_prepared_test_source "${_dist_runtime_settings_mutant_path}")
    else()
        set(_dist_variant normal)
        set(_dist_kind contract)
        set(_dist_fixture_prefix "dist_graph_no_roots")
        set(_dist_prepared_test_source "${_dist_prepared_mutant_path}")
    endif()
    jce_add_cxx_test(${_dist_target} KIND ${_dist_kind}
        SOURCES test_jce_dist_content_graph.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/core/jce_dist_content_graph.cpp"
            "${CMAKE_SOURCE_DIR}/editor/src/core/jce_dist_name_policy.cpp"
            "${CMAKE_SOURCE_DIR}/editor/src/core/jce_dist_audit.cpp"
            "${_dist_prepared_test_source}"
        INCLUDES "${CMAKE_SOURCE_DIR}/editor/src/core"
        LINK jce_core jce_resource jce_application)
    target_compile_features(${_dist_target} PRIVATE cxx_std_20)
    target_compile_definitions(${_dist_target} PRIVATE
        JCE_TEST_DIST_FIXTURE_PREFIX="${_dist_fixture_prefix}"
        JCE_TEST_DIST_MANAGER_ADAPTER="${CMAKE_CURRENT_BINARY_DIR}/dist-name-policy-mutation/jce_dist_adapter_${_dist_variant}.h")
endforeach()
set_tests_properties(test_jce_dist_content_graph_missing_confidential_wire
    test_jce_dist_content_graph_missing_prepared_roots
    test_jce_dist_content_graph_missing_runtime_settings PROPERTIES WILL_FAIL TRUE)

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
include(bundle_pipeline_controls.cmake)
include(project_bundle_retirement_controls.cmake)
include(play_swap_retirement_controls.cmake)
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

# Real ImGui context/event queue controls, distinct from the registry stub.
include(hotkey_persistence_controls.cmake)

function(_jce_editor_hotkey_input_fixture target kind)
    jce_add_cxx_test(${target}
        KIND ${kind}
        SOURCES test_jce_editor_hotkey_input.cpp
                "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_config.cpp"
                "${CMAKE_SOURCE_DIR}/editor/src/core/jce_hotkeys.cpp"
        LINK jce_core jce_platform jce_tools_imgui)
    target_compile_features(${target} PRIVATE cxx_std_20)
endfunction()
_jce_editor_hotkey_input_fixture(test_jce_editor_hotkey_input integration)
_jce_editor_hotkey_input_fixture(test_jce_editor_hotkey_input_dist_diag integration)
target_compile_definitions(test_jce_editor_hotkey_input_dist_diag PRIVATE
    JCE_DIST=1 JCE_TEST_HOTKEY_DIAG_RECORDS=1)
set_tests_properties(test_jce_editor_hotkey_input_dist_diag PROPERTIES
    ENVIRONMENT "JCE_EDITOR_HOTKEY_DIAG=1")

# Compile the same real owner with only event modifier forwarding removed.
# A modifier-less mutant must compile and fail actual chord assertions.
set(_hotkey_input_header "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_key_event.h")
file(READ "${_hotkey_input_header}" _hotkey_input_source)
string(REGEX MATCHALL "jce_editor_key_modifiers\\(io, event\\.key\\.mod\\)"
       _hotkey_input_markers "${_hotkey_input_source}")
list(LENGTH _hotkey_input_markers _hotkey_input_marker_count)
if(NOT _hotkey_input_marker_count EQUAL 1)
    message(FATAL_ERROR "Editor key-event modifier mutation requires one real forwarding call")
endif()
string(REPLACE "jce_editor_key_modifiers(io, event.key.mod);"
               "jce_editor_key_modifiers(io, JCE_KMOD_NONE);"
       _hotkey_input_mutant "${_hotkey_input_source}")
if(_hotkey_input_mutant STREQUAL _hotkey_input_source)
    message(FATAL_ERROR "Editor key-event modifier mutation did not change its owner")
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/hotkey-input-mutation")
set(_hotkey_input_mutant_header "${CMAKE_CURRENT_BINARY_DIR}/hotkey-input-mutation/jce_editor_key_event.h")
if(EXISTS "${_hotkey_input_mutant_header}")
    file(READ "${_hotkey_input_mutant_header}" _hotkey_input_previous)
endif()
if(NOT _hotkey_input_previous STREQUAL _hotkey_input_mutant)
    file(WRITE "${_hotkey_input_mutant_header}" "${_hotkey_input_mutant}")
endif()
_jce_editor_hotkey_input_fixture(test_jce_editor_hotkey_input_no_mods contract)
target_compile_definitions(test_jce_editor_hotkey_input_no_mods PRIVATE
    JCE_TEST_EDITOR_KEY_EVENT_HEADER="${_hotkey_input_mutant_header}")
set_tests_properties(test_jce_editor_hotkey_input_no_mods PROPERTIES WILL_FAIL TRUE)

# Compile the actual registry without only its global capture/completion gate.
# Real ImGui capture tests must then fail at runtime, not during compilation.
set(_hotkey_capture_owner "${CMAKE_SOURCE_DIR}/editor/src/core/jce_hotkeys.cpp")
file(READ "${_hotkey_capture_owner}" _hotkey_capture_source)
string(REGEX MATCHALL "s_runtime\\.capture_blocked_frame == ImGui::GetFrameCount\\(\\)"
    _hotkey_capture_markers "${_hotkey_capture_source}")
list(LENGTH _hotkey_capture_markers _hotkey_capture_marker_count)
set(_hotkey_capture_gate [=[    if (s_runtime.capture_id != JCE_HK_COUNT ||
        s_runtime.capture_blocked_frame == ImGui::GetFrameCount()) return false;
]=])
string(FIND "${_hotkey_capture_source}" "${_hotkey_capture_gate}" _hotkey_capture_gate_offset)
if(NOT _hotkey_capture_marker_count EQUAL 1 OR _hotkey_capture_gate_offset LESS 0)
    message(FATAL_ERROR "Hotkey capture mutation requires one actual global capture gate")
endif()
string(REPLACE "${_hotkey_capture_gate}" ""
    _hotkey_capture_mutated "${_hotkey_capture_source}")
if(_hotkey_capture_mutated STREQUAL _hotkey_capture_source)
    message(FATAL_ERROR "Hotkey capture mutation did not remove its actual gate")
endif()
set(_hotkey_capture_mutant_owner "${CMAKE_CURRENT_BINARY_DIR}/jce_hotkeys_capture_no_gate.cpp")
set(_hotkey_capture_previous "")
if(EXISTS "${_hotkey_capture_mutant_owner}")
    file(READ "${_hotkey_capture_mutant_owner}" _hotkey_capture_previous)
endif()
if(NOT _hotkey_capture_previous STREQUAL _hotkey_capture_mutated)
    file(WRITE "${_hotkey_capture_mutant_owner}" "${_hotkey_capture_mutated}")
endif()
jce_add_cxx_test(test_jce_editor_hotkey_capture_no_gate
    KIND contract
    SOURCES test_jce_editor_hotkey_input.cpp
        "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_config.cpp"
        "${_hotkey_capture_mutant_owner}"
    LINK jce_core jce_platform jce_tools_imgui
    INCLUDES "${CMAKE_SOURCE_DIR}/editor/src/core")
target_compile_features(test_jce_editor_hotkey_capture_no_gate PRIVATE cxx_std_20)
set_tests_properties(test_jce_editor_hotkey_capture_no_gate PROPERTIES WILL_FAIL TRUE)
unset(_hotkey_capture_owner)
unset(_hotkey_capture_source)
unset(_hotkey_capture_markers)
unset(_hotkey_capture_marker_count)
unset(_hotkey_capture_gate)
unset(_hotkey_capture_gate_offset)
unset(_hotkey_capture_mutated)
unset(_hotkey_capture_mutant_owner)
unset(_hotkey_capture_previous)

# Game View autonomous capture schedule — pure parser/timer state machine.
jce_add_cxx_test(test_jce_editor_kpi_game_capture
    SOURCES test_jce_editor_kpi_game_capture.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_kpi_game_capture.cpp"
    LINK    jce_core jce_application)

# Game View input ownership + pre-step commit order.  No ImGui/SDL/renderer.
jce_add_cxx_test(test_jce_editor_game_input_bridge
    SOURCES test_jce_editor_game_input_bridge.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_game_input_bridge.cpp"
    LINK    jce_core jce_platform)

include("${CMAKE_CURRENT_LIST_DIR}/game_input_replay_controls.cmake")

# Remove only the actual hover-prime call; native event, capture, focus and
# commit-order controls still compile and use the same complete owner.
set(_game_input_owner "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_game_input_bridge.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_game_input_owner}")
file(READ "${_game_input_owner}" _game_input_source)
string(REGEX MATCHALL "prime_hover_position[(]bridge[)]"
    _game_input_prime_marks "${_game_input_source}")
list(LENGTH _game_input_prime_marks _game_input_prime_count)
if(NOT _game_input_prime_count EQUAL 1)
    message(FATAL_ERROR "Game input control requires one actual hover-prime wire")
endif()
string(REPLACE "    prime_hover_position(bridge);"
    "    /* old wire: no position until the next accepted native event */"
    _game_input_old_prime "${_game_input_source}")
if(_game_input_old_prime STREQUAL _game_input_source)
    message(FATAL_ERROR "Game input control did not remove the actual hover-prime wire")
endif()
set(_game_input_old_path "${CMAKE_CURRENT_BINARY_DIR}/game-input-mutation/jce_editor_game_input_old_prime.cpp")
unset(_game_input_previous)
include("${CMAKE_CURRENT_LIST_DIR}/game_input_virtual_controls.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/game_canvas_pointer_controls.cmake")
if(EXISTS "${_game_input_old_path}")
    file(READ "${_game_input_old_path}" _game_input_previous)
endif()
if(NOT _game_input_previous STREQUAL _game_input_old_prime)
    file(WRITE "${_game_input_old_path}" "${_game_input_old_prime}")
endif()
jce_add_cxx_test(test_jce_editor_game_input_bridge_old_prime
    KIND contract
    SOURCES test_jce_editor_game_input_bridge.cpp "${_game_input_old_path}"
    LINK jce_core jce_platform)
set_tests_properties(test_jce_editor_game_input_bridge_old_prime PROPERTIES WILL_FAIL TRUE)
unset(_game_input_owner)
unset(_game_input_source)
unset(_game_input_prime_marks)
unset(_game_input_prime_count)
unset(_game_input_old_prime)
unset(_game_input_old_path)
unset(_game_input_previous)

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

# Large hierarchy capacity arithmetic stays independent of the UI runtime.
jce_add_cxx_test(test_jce_hierarchy_capacity
    SOURCES test_jce_hierarchy_capacity.cpp)

# Stable history identity is tested without editor initialization.
jce_add_cxx_test(test_jce_history_identity
    SOURCES test_jce_history_identity.cpp)

jce_add_cxx_test(test_jce_fv_video_policy
    SOURCES test_jce_fv_video_policy.cpp
    LINK jce_core)

# Real scoped relation/component restore uses public ECS, without a GUI host.
jce_add_cxx_test(test_jce_history_entity
    SOURCES test_jce_history_entity.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_history_entity.cpp"
    LINK jce_core jce_scene jce_resource)

# Default-mode hierarchy reuse admission/key, no ImGui or native window.
jce_add_cxx_test(test_jce_hierarchy_flat_cache
    SOURCES test_jce_hierarchy_flat_cache.cpp)

# Production Play transaction failure sequencing, without a graphical editor.
jce_add_cxx_test(test_jce_editor_play_transaction
    SOURCES test_jce_editor_play_transaction.cpp)
include(editor_exit_recorder_controls.cmake)
include(editor_recorder_controls.cmake)
# Actual Play presentation capture/apply/restore with counted public CPU ports.
# Runtime source-wire negatives are compiled by the formal Python gate below.
jce_add_cxx_test(test_jce_editor_play_presentation
    KIND unit
    SOURCES test_jce_editor_play_presentation.cpp
    LINK jce_core)
set_tests_properties(test_jce_editor_play_presentation PROPERTIES
    JCE_TEST_DOMAIN editor)

# Actual host font input ownership with headless ImGui atlas lifetime.
jce_add_cxx_test(test_jce_editor_font_data
    SOURCES test_jce_editor_font_data.cpp
    LINK jce_core jce_tools_imgui)

# Actual bounded editor backbuffer owner, with SDK submission/completion seams.
jce_add_cxx_test(test_jce_editor_screenshot_completion
    SOURCES test_jce_editor_screenshot_completion.cpp)
include(editor_screenshot_close_controls.cmake)

# Exact shared Scene/Game capture source selection; CPU request ports only.
include(viewport_composite_controls.cmake)
set(_capture_extent_path "${CMAKE_SOURCE_DIR}/editor/src/scene/jce_editor_viewport_common.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_capture_extent_path}")
file(READ "${_capture_extent_path}" _capture_extent_source)
set(_capture_extent_start "inline bool jce_editor_viewport_screenshot_submit(")
set(_capture_extent_end "/* Per-entity hardware-query occlusion culling")
foreach(_capture_extent_boundary IN ITEMS start end)
    string(FIND "${_capture_extent_source}" "${_capture_extent_${_capture_extent_boundary}}" _capture_extent_${_capture_extent_boundary}_first)
    string(FIND "${_capture_extent_source}" "${_capture_extent_${_capture_extent_boundary}}" _capture_extent_${_capture_extent_boundary}_last REVERSE)
    if(_capture_extent_${_capture_extent_boundary}_first LESS 0 OR
       NOT _capture_extent_${_capture_extent_boundary}_first EQUAL _capture_extent_${_capture_extent_boundary}_last)
        message(FATAL_ERROR "Viewport capture requires one actual ${_capture_extent_boundary} boundary")
    endif()
endforeach()
if(NOT _capture_extent_end_first GREATER _capture_extent_start_first)
    message(FATAL_ERROR "Viewport capture source extraction bounds are not ordered")
endif()
math(EXPR _capture_extent_length "${_capture_extent_end_first} - ${_capture_extent_start_first}")
string(SUBSTRING "${_capture_extent_source}" ${_capture_extent_start_first} ${_capture_extent_length} _capture_extent_owner)
string(REGEX MATCHALL "JCE_EDITOR_VP_SCREENSHOT_OFFSET[ ]*=[ ]*[0-9]+" _capture_extent_offsets "${_capture_extent_source}")
list(LENGTH _capture_extent_offsets _capture_extent_offset_count)
if(NOT _capture_extent_offset_count EQUAL 1)
    message(FATAL_ERROR "Viewport capture requires the one actual screenshot offset")
endif()
set(_capture_extent_wire
"        uint32_t target_width = 0, target_height = 0;
        if (!jce_offscreen_target_get_extent(bridge, &target_width, &target_height) ||
            !target_width || !target_height ||
            target_width > UINT16_MAX || target_height > UINT16_MAX)
            return false;
        width = static_cast<uint16_t>(target_width);
        height = static_cast<uint16_t>(target_height);")
string(FIND "${_capture_extent_owner}" "${_capture_extent_wire}" _capture_extent_first)
string(FIND "${_capture_extent_owner}" "${_capture_extent_wire}" _capture_extent_last REVERSE)
if(_capture_extent_first LESS 0 OR NOT _capture_extent_first EQUAL _capture_extent_last)
    message(FATAL_ERROR "Viewport capture requires one actual target extent wire")
endif()
set(_capture_extent_dir "${CMAKE_CURRENT_BINARY_DIR}/viewport-capture-extent")
file(MAKE_DIRECTORY "${_capture_extent_dir}")
foreach(_capture_extent_variant IN ITEMS current old_requested_extent)
    set(_capture_extent_body "${_capture_extent_owner}")
    set(_capture_extent_target test_jce_editor_viewport_capture_extent)
    set(_capture_extent_kind unit)
    if(_capture_extent_variant STREQUAL "old_requested_extent")
        string(REPLACE "${_capture_extent_wire}" "" _capture_extent_body "${_capture_extent_body}")
        string(APPEND _capture_extent_target "_old_requested_extent")
        set(_capture_extent_kind contract)
    endif()
    set(_capture_extent_output "${_capture_extent_dir}/${_capture_extent_variant}.inc.h")
    set(_capture_extent_content "enum { ${_capture_extent_offsets} };\n${_capture_extent_body}")
    unset(_capture_extent_previous)
    if(EXISTS "${_capture_extent_output}")
        file(READ "${_capture_extent_output}" _capture_extent_previous)
    endif()
    if(NOT _capture_extent_previous STREQUAL _capture_extent_content)
        file(WRITE "${_capture_extent_output}" "${_capture_extent_content}")
    endif()
    jce_add_cxx_test(${_capture_extent_target} KIND ${_capture_extent_kind}
        SOURCES test_jce_editor_viewport_capture_extent.cpp)
    target_compile_definitions(${_capture_extent_target} PRIVATE
        JCE_TEST_VIEWPORT_CAPTURE_OWNER="${_capture_extent_output}")
    if(_capture_extent_kind STREQUAL "contract")
        set_tests_properties(${_capture_extent_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
foreach(_capture_extent_var IN ITEMS path source start end boundary start_first start_last
        end_first end_last length owner offsets offset_count wire first last dir variant body
        target kind output content previous)
    unset(_capture_extent_${_capture_extent_var})
endforeach()
unset(_capture_extent_var)

# Actual stateless QA event decoding, without hardware/window injection.
jce_add_cxx_test(test_jce_editor_event_fixture
    SOURCES test_jce_editor_event_fixture.cpp
    LINK jce_core)
include("${CMAKE_CURRENT_LIST_DIR}/event_fixture_schedule_controls.cmake")

# Public project resolver plus exact scene/dialog admission bodies. Publication
# ports do not certify GUI/ECS rollback; a negative must fail ordinary assertions.
set(_project_gate_dialog_owner "${CMAKE_SOURCE_DIR}/editor/src/dialogs/jce_dialog_project.cpp")
set(_project_gate_scene_owner "${CMAKE_SOURCE_DIR}/editor/src/io/jce_editor_scene_serial.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_project_gate_dialog_owner}" "${_project_gate_scene_owner}")
file(READ "${_project_gate_dialog_owner}" _project_gate_dialog)
file(READ "${_project_gate_scene_owner}" _project_gate_scene)
function(_jce_project_gate_once SOURCE NEEDLE)
    string(FIND "${${SOURCE}}" "${NEEDLE}" _begin)
    if(_begin LESS 0)
        message(FATAL_ERROR "Project admission fixture missing actual marker: ${NEEDLE}")
    endif()
    string(LENGTH "${NEEDLE}" _length)
    math(EXPR _after "${_begin} + ${_length}")
    string(SUBSTRING "${${SOURCE}}" ${_after} -1 _tail)
    string(FIND "${_tail}" "${NEEDLE}" _duplicate)
    if(NOT _duplicate LESS 0)
        message(FATAL_ERROR "Project admission fixture marker is not unique: ${NEEDLE}")
    endif()
endfunction()
function(_jce_project_gate_extract SOURCE START END OUTPUT)
    _jce_project_gate_once(${SOURCE} "${START}")
    string(FIND "${${SOURCE}}" "${START}" _begin)
    string(SUBSTRING "${${SOURCE}}" ${_begin} -1 _tail)
    string(FIND "${_tail}" "${END}" _length)
    if(_length LESS_EQUAL 0)
        message(FATAL_ERROR "Project admission fixture invalid actual-body bounds")
    endif()
    string(SUBSTRING "${_tail}" 0 ${_length} _body)
    set(${OUTPUT} "${_body}" PARENT_SCOPE)
endfunction()
# The existing headless driver fixture consumes the real Build log/pipe bodies.
# No second build runner, actual manager TU or test registration is introduced.
set(_build_log_owner "${CMAKE_SOURCE_DIR}/editor/src/core/jce_build_manager.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_build_log_owner}")
file(READ "${_build_log_owner}" _build_log_source)
_jce_project_gate_extract(_build_log_source
    "void log_line(JceConsoleLevel level, const std::string &line)"
    "\nvoid drain_pipes()" _build_log_lines)
_jce_project_gate_extract(_build_log_source
    "void flush_partial(std::string &partial, JceConsoleLevel level)"
    "\nvoid release_process()" _build_log_partial)
_jce_project_gate_extract(_build_log_source
    "void jce_build_manager_get_log(JceBuildLogView *out)"
    "\nvoid jce_build_manager_get_status(" _build_log_snapshot)
_jce_project_gate_once(_build_log_lines "g_build.log.append_line(line)")
string(REPLACE "g_build.log.append_line(line);" "(void)line;"
    _build_log_old_lines "${_build_log_lines}")
if(_build_log_old_lines STREQUAL _build_log_lines)
    message(FATAL_ERROR "Build log ordinary old-wire control did not mutate the actual capture")
endif()
set(_build_log_probe_prefix "\nstruct BuildProbe { JceBuildLog log; };\nBuildProbe g_build;\nusing WorkerLogSink = JceBuildWorkerLog<JceConsoleLevel>;\nWorkerLogSink *t_log_sink = nullptr;\nint console_count = 0;\nauto jce_editor_console_log_level = [](JceConsoleLevel, const char *, auto...) { ++console_count; };\n")
set(_build_log_probe "${CMAKE_CURRENT_BINARY_DIR}/jce_build_log_actual_controls.h")
jce_test_write_if_changed("${_build_log_probe}"
    "namespace build_log_actual {${_build_log_probe_prefix}${_build_log_lines}${_build_log_partial}${_build_log_snapshot}}\nnamespace build_log_old_wire {${_build_log_probe_prefix}${_build_log_old_lines}${_build_log_partial}${_build_log_snapshot}}\n")
target_compile_definitions(test_jce_editor_headless_build_driver PRIVATE
    "JCE_TEST_BUILD_LOG_ACTUAL_HEADER=\"${_build_log_probe}\"")
_jce_project_gate_extract(_project_gate_dialog
    "bool jce_editor_prepare_project_switch(const char *next_root)"
    "\nbool set_current_project_root" _project_gate_publication)
_jce_project_gate_extract(_project_gate_dialog
    "bool set_current_project_root(const char *path)"
    "\n    if (!buf[0])" _project_gate_dialog_prefix)
_jce_project_gate_extract(_project_gate_scene
    "static bool prepare_scene_project_switch(const char *content_path,"
    "\nstatic bool scene_document_admitted" _project_gate_selection)
# Check the real three entry points, including preserve-A Bundle policy and
# refusal before teardown. The runtime fixture consumes the same narrow bodies.
_jce_project_gate_extract(_project_gate_scene
    "bool jce_state_load_scene_file(const char *scene_path)"
    "\nstruct DeferredSceneLoad" _project_gate_sync)
_jce_project_gate_extract(_project_gate_scene
    "bool jce_state_load_scene_file_async(const char *scene_path)"
    "\nbool jce_state_is_scene_loading" _project_gate_async)
_jce_project_gate_extract(_project_gate_scene
    "bool jce_state_load_scene_from_jbundle(const char *jbundle_path)"
    "\nbool jce_state_load_scene_from_catalog" _project_gate_bundle)
foreach(_project_gate_entry IN ITEMS sync async bundle)
    if(_project_gate_entry STREQUAL "bundle")
        set(_project_gate_wire "if (!prepare_scene_project_switch(jbundle_path, true)) return false;")
        set(_project_gate_teardown "close_active_bundle_mount();")
    else()
        set(_project_gate_wire "if (!prepare_scene_project_switch(scene_path)) return false;")
        set(_project_gate_teardown "jce_editor_scene_render_streaming_teardown();")
    endif()
    _jce_project_gate_once(_project_gate_${_project_gate_entry} "${_project_gate_wire}")
    string(FIND "${_project_gate_${_project_gate_entry}}" "${_project_gate_wire}" _gate_pos)
    string(FIND "${_project_gate_${_project_gate_entry}}" "${_project_gate_teardown}" _teardown_pos)
    if(_teardown_pos LESS 0 OR NOT _gate_pos LESS _teardown_pos)
        message(FATAL_ERROR "Project admission must precede ${_project_gate_entry} teardown")
    endif()
endforeach()
_jce_project_gate_once(_project_gate_dialog_prefix
    "if (!jce_editor_prepare_project_switch(buf)) return false;")
# Stop the setter excerpt at its admission boundary; subsystem rebind is not
# reproduced or certified by this fixture.
string(CONCAT _project_gate_normal "${_project_gate_publication}\n${_project_gate_selection}\n"
    "${_project_gate_dialog_prefix}    return true;\n}\n")
set(_project_gate_identity_guard
    "if (jce_editor_project_path_equal(current, next_root)) return true;")
_jce_project_gate_once(_project_gate_normal "${_project_gate_identity_guard}")
string(REPLACE "${_project_gate_identity_guard}" "(void)current;"
    _project_gate_old_always_flush "${_project_gate_normal}")
if(_project_gate_old_always_flush STREQUAL _project_gate_normal)
    message(FATAL_ERROR "Project admission old-always-flush mutation did not change source")
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/project-admission")
foreach(_project_gate_variant IN ITEMS normal old_always_flush)
    set(_project_gate_body_var "_project_gate_${_project_gate_variant}")
    set(_project_gate_path "${CMAKE_CURRENT_BINARY_DIR}/project-admission/${_project_gate_variant}.inc.h")
    unset(_project_gate_previous)
    if(EXISTS "${_project_gate_path}")
        file(READ "${_project_gate_path}" _project_gate_previous)
    endif()
    if(NOT _project_gate_previous STREQUAL "${${_project_gate_body_var}}")
        file(WRITE "${_project_gate_path}" "${${_project_gate_body_var}}")
    endif()
    if(_project_gate_variant STREQUAL "normal")
        set(_project_gate_target test_jce_editor_project_context)
        set(_project_gate_kind integration)
    else()
        set(_project_gate_target test_jce_editor_project_context_old_always_flush)
        set(_project_gate_kind contract)
    endif()
    jce_add_cxx_test(${_project_gate_target} KIND ${_project_gate_kind}
        SOURCES test_jce_editor_project_context.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_project_context.cpp"
        LINK jce_core jce_application)
    target_compile_definitions(${_project_gate_target} PRIVATE
        JCE_TEST_EDITOR_PROJECT_PREFLIGHT_BODY="${_project_gate_path}")
endforeach()
set_tests_properties(test_jce_editor_project_context_old_always_flush PROPERTIES WILL_FAIL TRUE)

# Actual foliage output path/write/readback owner; CPU only, no editor window.
jce_add_cxx_test(test_jce_editor_foliage_bake_output
    SOURCES test_jce_editor_foliage_bake_output.cpp
        "${CMAKE_SOURCE_DIR}/editor/src/panels/jce_editor_foliage_bake_output.cpp"
    LINK jce_core
    INCLUDES "${CMAKE_SOURCE_DIR}/editor/src")

# Native/SDK target admission with real host files; no compiler, product or GPU execution.
jce_add_cxx_test(test_jce_project_build_target
    SOURCES test_jce_project_build_target.cpp
        "${CMAKE_SOURCE_DIR}/editor/src/core/jce_project_build_target.cpp"
    LINK jce_core)
# Only this exact-owner fixture redirects read/free to counted public-host ports.
target_compile_definitions(test_jce_project_build_target PRIVATE
    jce_fs_host_read_capped=jce_test_sdk_read_capped
    jce_fs_buffer_free=jce_test_sdk_buffer_free)

# Exact package replacement helper with real host files; CPU, not GUI/relocation.
jce_add_cxx_test(test_jce_build_package
    SOURCES test_jce_build_package.cpp
        "${CMAKE_SOURCE_DIR}/editor/src/core/jce_build_package.cpp"
    LINK jce_core)

# Real request admission/refusal + Play acceptance bodies with counted CPU ports.
# Stops before asset preparation; no compiler, child, GUI or GPU is started.
set(_build_request_panel "${CMAKE_SOURCE_DIR}/editor/src/panels/jce_panel_build_profiles.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_build_request_panel}")
file(READ "${_build_request_panel}" _build_request_panel_source)
_jce_project_gate_extract(_build_log_source "void reset_pipeline()"
    "\n/* ---------------------------------------------------------------- *\n * Worker-thread log redirect."
    _build_request_reset)
_jce_project_gate_extract(_build_log_source "void set_error(const std::string &msg)"
    "\nbool spawn_tool(" _build_request_begin)
_jce_project_gate_extract(_build_log_source
    "bool jce_build_manager_reject_request(const char *label, const char *error)"
    "\nbool jce_build_manager_start_project_build(" _build_request_reject)
_jce_project_gate_extract(_build_log_source
    "bool jce_build_manager_start_project_build(const JceBuildProjectConfig *cfg)"
    "\n    const std::string cooked =" _build_request_admit)
_jce_project_gate_extract(_build_request_panel_source
    "        auto start_play_build = [&]() {" "\n        /* Script options strip"
    _build_request_play)
string(REPLACE "auto start_play_build = [&]()" "void dispatch_play_build()"
    _build_request_play "${_build_request_play}")
_jce_project_gate_extract(_build_request_panel_source
    "            case decltype(s_bp)::PLAY_BUILDING:"
    "\n            case decltype(s_bp)::PLAY_RUNNING:" _build_request_advance)
string(APPEND _build_request_advance "\n            default: break;\n        }\n}\n")
string(PREPEND _build_request_advance "void advance_play_build() { switch (s_bp.play_stage) {\n")
# The actual normal start guard is mutated, rather than writing a second model.
_jce_project_gate_once(_build_request_play "if (accepted)")
string(REPLACE "if (accepted)" "if (true)" _build_request_old_play "${_build_request_play}")
# Both actual Cook-completed and direct Play callsites must consume this body.
# MATCHALL results containing ';' are CMake lists, so count exact removed
# bytes instead; also require one call in each real production branch.
set(_build_request_call "start_play_build();")
string(LENGTH "${_build_request_panel_source}" _build_request_source_length)
string(REPLACE "${_build_request_call}" "" _build_request_without_calls "${_build_request_panel_source}")
string(LENGTH "${_build_request_without_calls}" _build_request_without_length)
string(LENGTH "${_build_request_call}" _build_request_call_length)
math(EXPR _build_request_removed_length "${_build_request_source_length} - ${_build_request_without_length}")
math(EXPR _build_request_expected_length "2 * ${_build_request_call_length}")
if(NOT _build_request_removed_length EQUAL _build_request_expected_length)
    message(FATAL_ERROR "Build Profiles Play fixture requires both actual acceptance callsites")
endif()
_jce_project_gate_extract(_build_request_panel_source
    "            case decltype(s_bp)::PLAY_COOKING:"
    "\n            case decltype(s_bp)::PLAY_BUILDING:" _build_request_cook_entry)
_jce_project_gate_once(_build_request_cook_entry "${_build_request_call}")
_jce_project_gate_extract(_build_request_panel_source
    "                /* Skip cook stage — straight to build. */"
    "\n            }" _build_request_direct_entry)
_jce_project_gate_once(_build_request_direct_entry "${_build_request_call}")
_jce_project_gate_once(_build_request_begin "g_build.state = JCE_BUILD_FAILED;")
string(REPLACE "g_build.state = JCE_BUILD_FAILED;" "/* old request retained its previous state */"
    _build_request_old_begin "${_build_request_begin}")
foreach(_contract normal old_play_acceptance old_refusal_status)
    set(_request_header "${CMAKE_CURRENT_BINARY_DIR}/jce_build_request_${_contract}.h")
    set(_request_play "${_build_request_play}")
    set(_request_begin "${_build_request_begin}")
    if(_contract STREQUAL "old_refusal_status")
        set(_request_begin "${_build_request_old_begin}")
    endif()
    if(_contract STREQUAL "old_play_acceptance")
        set(_request_play "${_build_request_old_play}")
    endif()
    jce_test_write_if_changed("${_request_header}" "${_build_request_reset}${_request_begin}${_build_request_reject}${_build_request_admit}\n    return true;\n}\n${_request_play}${_build_request_advance}")
    if(_contract STREQUAL "normal")
        set(_request_target test_jce_build_request_admission)
        jce_add_cxx_test(${_request_target} SOURCES test_jce_build_request_admission.cpp
            INCLUDES "${CMAKE_SOURCE_DIR}/editor/src")
    else()
        set(_request_target test_jce_build_request_${_contract})
        jce_add_cxx_test(${_request_target} KIND contract SOURCES test_jce_build_request_admission.cpp
            INCLUDES "${CMAKE_SOURCE_DIR}/editor/src")
        set_tests_properties(${_request_target} PROPERTIES WILL_FAIL TRUE)
    endif()
    target_compile_definitions(${_request_target} PRIVATE
        "JCE_TEST_BUILD_REQUEST_ACTUAL_HEADER=\"${_request_header}\"")
endforeach()

# CPU session/argv policy only; real panel consumption is a separate UI check.
jce_add_cxx_test(test_jce_editor_ai_panel_session
    SOURCES test_jce_editor_ai_panel_session.cpp)

# Exact public-toolchain resolver with counted host ports; CPU, no process/UI.
jce_add_cxx_test(test_jce_build_tool_paths
    SOURCES test_jce_build_tool_paths.cpp
        "${CMAKE_SOURCE_DIR}/editor/src/core/jce_build_tool_paths.cpp"
    LINK jce_core)
target_compile_definitions(test_jce_build_tool_paths PRIVATE
    jce_toolchain_get=jce_test_build_toolchain_get
    jce_host_resolve_executable=jce_test_build_resolve_executable
    jce_fs_host_exists_file=jce_test_build_exists_file)

include("${CMAKE_CURRENT_SOURCE_DIR}/tool_probe_controls.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/build_completion_controls.cmake")

# Exact first-startup renderer request + actual persisted config, CPU only.
jce_add_cxx_test(test_jce_editor_renderer_startup
    SOURCES test_jce_editor_renderer_startup.cpp
        "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_config.cpp"
    LINK jce_core jce_platform)

# Only restore the old missing-profile early guard. The unchanged fixture must
# fail behaviorally; no copied loader, parser, defaults or renderer are used.
set(_startup_header "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_renderer_startup.h")
file(READ "${_startup_header}" _startup_source)
# Keep the semicolon out of the CMake list: one source match is one item.
string(REGEX MATCHALL "\\(void\\)load\\(&config\\)" _startup_guards "${_startup_source}")
list(LENGTH _startup_guards _startup_guard_count)
if(NOT _startup_guard_count EQUAL 1)
    message(FATAL_ERROR "Startup old-guard control requires one exact production load marker")
endif()
string(REPLACE "    (void)load(&config);" "    if (!load(&config)) return false;"
    _startup_mutant "${_startup_source}")
set(_startup_mutant_dir "${CMAKE_CURRENT_BINARY_DIR}/renderer-startup-controls")
file(MAKE_DIRECTORY "${_startup_mutant_dir}")
set(_startup_mutant_header "${_startup_mutant_dir}/jce_editor_renderer_startup_old_guard.h")
jce_test_write_if_changed("${_startup_mutant_header}" "${_startup_mutant}")
jce_add_cxx_test(test_jce_editor_renderer_startup_old_guard
    KIND contract
    SOURCES test_jce_editor_renderer_startup.cpp
        "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_config.cpp"
    LINK jce_core jce_platform)
# The mutated header lives in the build tree; keep its original private sibling include owner.
target_include_directories(test_jce_editor_renderer_startup_old_guard PRIVATE
    "${CMAKE_SOURCE_DIR}/editor/src/core")
target_compile_definitions(test_jce_editor_renderer_startup_old_guard PRIVATE
    JCE_EDITOR_RENDERER_STARTUP_HEADER="${_startup_mutant_header}")
set_tests_properties(test_jce_editor_renderer_startup_old_guard PROPERTIES WILL_FAIL TRUE)

# Only remove the real profile-to-engine setter. The same actual-loader fixture
# must compile and fail its binding witness; no second config owner is linked.
string(REGEX MATCHALL "set_path\\(path\\)" _startup_config_wires "${_startup_source}")
list(LENGTH _startup_config_wires _startup_config_wire_count)
if(NOT _startup_config_wire_count EQUAL 1)
    message(FATAL_ERROR "Startup config binding control requires one exact setter wire")
endif()
string(REPLACE "    set_path(path);" "    (void)set_path;"
    _startup_config_mutant "${_startup_source}")
if(_startup_config_mutant STREQUAL _startup_source)
    message(FATAL_ERROR "Startup config binding mutation did not change actual owner")
endif()
set(_startup_config_mutant_header "${_startup_mutant_dir}/jce_editor_renderer_startup_old_config_scope.h")
jce_test_write_if_changed("${_startup_config_mutant_header}" "${_startup_config_mutant}")
jce_add_cxx_test(test_jce_editor_renderer_startup_old_config_scope
    KIND contract
    SOURCES test_jce_editor_renderer_startup.cpp
        "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_config.cpp"
    LINK jce_core jce_platform)
target_include_directories(test_jce_editor_renderer_startup_old_config_scope PRIVATE
    "${CMAKE_SOURCE_DIR}/editor/src/core")
target_compile_definitions(test_jce_editor_renderer_startup_old_config_scope PRIVATE
    JCE_EDITOR_RENDERER_STARTUP_HEADER="${_startup_config_mutant_header}")
set_tests_properties(test_jce_editor_renderer_startup_old_config_scope PROPERTIES WILL_FAIL TRUE)
# Real application wrapper must forward the public GPU setter before creation.
file(READ "${CMAKE_SOURCE_DIR}/editor/src/jce_editor_main.cpp" _startup_main_source)
string(REGEX MATCHALL
    "jce_editor_apply_startup_renderer\\(jce_editor_config_load,[ \t\r\n]+jce_engine_set_renderer_override,[ \t\r\n]+jce_renderer_set_gpu_adapter_preference\\)"
    _startup_gpu_main_wires "${_startup_main_source}")
list(LENGTH _startup_gpu_main_wires _startup_gpu_main_wire_count)
if(NOT _startup_gpu_main_wire_count EQUAL 1)
    message(FATAL_ERROR "Editor startup requires the actual public GPU preference setter wire")
endif()
string(REGEX MATCHALL "set_gpu_adapter\\(name\\)" _startup_gpu_wires "${_startup_source}")
list(LENGTH _startup_gpu_wires _startup_gpu_wire_count)
if(NOT _startup_gpu_wire_count EQUAL 1)
    message(FATAL_ERROR "GPU preference old-wire control requires one actual setter call")
endif()
string(REPLACE "    set_gpu_adapter(name);" "    (void)set_gpu_adapter;"
    _startup_gpu_mutant "${_startup_source}")
if(_startup_gpu_mutant STREQUAL _startup_source)
    message(FATAL_ERROR "GPU preference old-wire mutation did not change actual owner")
endif()
set(_startup_gpu_mutant_header "${_startup_mutant_dir}/jce_editor_renderer_startup_old_gpu_wire.h")
jce_test_write_if_changed("${_startup_gpu_mutant_header}" "${_startup_gpu_mutant}")
jce_add_cxx_test(test_jce_editor_renderer_startup_old_gpu_wire
    KIND contract
    SOURCES test_jce_editor_renderer_startup.cpp
        "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_config.cpp"
    LINK jce_core jce_platform)
target_include_directories(test_jce_editor_renderer_startup_old_gpu_wire PRIVATE
    "${CMAKE_SOURCE_DIR}/editor/src/core")
target_compile_definitions(test_jce_editor_renderer_startup_old_gpu_wire PRIVATE
    JCE_EDITOR_RENDERER_STARTUP_HEADER="${_startup_gpu_mutant_header}")
set_tests_properties(test_jce_editor_renderer_startup_old_gpu_wire PROPERTIES WILL_FAIL TRUE)
unset(_startup_main_source)
unset(_startup_gpu_main_wires)
unset(_startup_gpu_main_wire_count)
unset(_startup_gpu_wires)
unset(_startup_gpu_wire_count)
unset(_startup_gpu_mutant)
unset(_startup_gpu_mutant_header)

unset(_startup_config_wires)
unset(_startup_config_wire_count)
unset(_startup_config_mutant)
unset(_startup_config_mutant_header)
unset(_startup_header)
unset(_startup_source)
unset(_startup_guards)
unset(_startup_guard_count)
unset(_startup_mutant)
unset(_startup_mutant_dir)
unset(_startup_mutant_header)


# Actual ImGui navigation-item ownership; no window/renderer/GPU is started.
function(_jce_scene_view_navigation_fixture target cube_source kind)
    jce_add_cxx_test(${target}
        KIND ${kind}
        SOURCES test_jce_scene_view_navigation.cpp
                "${cube_source}"
                "${CMAKE_SOURCE_DIR}/editor/src/panels/jce_scene_view_input_policy.cpp"
        LINK jce_core jce_tools_imgui
        INCLUDES "${CMAKE_SOURCE_DIR}/editor/src/panels")
    target_compile_features(${target} PRIVATE cxx_std_20)
endfunction()
set(_scene_nav_cube "${CMAKE_SOURCE_DIR}/editor/src/panels/jce_scene_view_cube.cpp")
_jce_scene_view_navigation_fixture(test_jce_scene_view_navigation "${_scene_nav_cube}" integration)
file(READ "${_scene_nav_cube}" _scene_nav_source)
string(REGEX MATCHALL "ctx->cube_left_clicked = submit_navigation_item\\(\"##SceneViewCubeInput\", cube\\.bounds, ctx\\)"
       _scene_nav_markers "${_scene_nav_source}")
list(LENGTH _scene_nav_markers _scene_nav_marker_count)
if(NOT _scene_nav_marker_count EQUAL 1)
    message(FATAL_ERROR "Scene View cube input mutation requires one actual navigation-item call")
endif()
string(REPLACE "ctx->cube_left_clicked = submit_navigation_item(\"##SceneViewCubeInput\", cube.bounds, ctx);"
               "(void)cube;" _scene_nav_mutant "${_scene_nav_source}")
if(_scene_nav_mutant STREQUAL _scene_nav_source)
    message(FATAL_ERROR "Scene View cube input mutation did not change its owner")
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/scene-navigation-mutation")
set(_scene_nav_mutant_source "${CMAKE_CURRENT_BINARY_DIR}/scene-navigation-mutation/jce_scene_view_cube.cpp")
if(EXISTS "${_scene_nav_mutant_source}")
    file(READ "${_scene_nav_mutant_source}" _scene_nav_previous)
endif()
if(NOT _scene_nav_previous STREQUAL _scene_nav_mutant)
    file(WRITE "${_scene_nav_mutant_source}" "${_scene_nav_mutant}")
endif()
_jce_scene_view_navigation_fixture(test_jce_scene_view_navigation_old_cube_item
    "${_scene_nav_mutant_source}" contract)
set_tests_properties(test_jce_scene_view_navigation_old_cube_item PROPERTIES WILL_FAIL TRUE)


# Actual ImGui End/Render cursor-layout regression. Restore the old post-Image
# SetCursorScreenPos wire only; the fixture reports errors as ordinary checks.
string(REGEX MATCHALL "ItemSize already leaves the cursor after the viewport\\."
    _scene_nav_cursor_markers "${_scene_nav_source}")
list(LENGTH _scene_nav_cursor_markers _scene_nav_cursor_marker_count)
if(NOT _scene_nav_cursor_marker_count EQUAL 1)
    message(FATAL_ERROR "Scene View cursor mutation requires one actual layout marker")
endif()
string(REPLACE "/* ItemSize already leaves the cursor after the viewport. */"
    "/* ItemSize already leaves the cursor after the viewport. */\n    ImGui::SetCursorScreenPos(ImGui::GetCursorScreenPos());"
    _scene_nav_cursor_mutant "${_scene_nav_source}")
if(_scene_nav_cursor_mutant STREQUAL _scene_nav_source)
    message(FATAL_ERROR "Scene View cursor mutation did not change its owner")
endif()
set(_scene_nav_cursor_mutant_source
    "${CMAKE_CURRENT_BINARY_DIR}/scene-navigation-mutation/jce_scene_view_cube_old_cursor_restore.cpp")
if(EXISTS "${_scene_nav_cursor_mutant_source}")
    file(READ "${_scene_nav_cursor_mutant_source}" _scene_nav_cursor_previous)
endif()
if(NOT _scene_nav_cursor_previous STREQUAL _scene_nav_cursor_mutant)
    file(WRITE "${_scene_nav_cursor_mutant_source}" "${_scene_nav_cursor_mutant}")
endif()
_jce_scene_view_navigation_fixture(test_jce_scene_view_navigation_old_cursor_restore
    "${_scene_nav_cursor_mutant_source}" contract)
set_tests_properties(test_jce_scene_view_navigation_old_cursor_restore PROPERTIES WILL_FAIL TRUE)


# Actual settings persistence uses isolated host files. A failed publication
# retains dirty state for debounce/forced retry; no Editor window is involved.
function(_jce_editor_config_persistence_fixture NAME OWNER KIND)
    jce_add_cxx_test(${NAME} KIND ${KIND}
        SOURCES test_jce_editor_config_persistence.cpp "${OWNER}"
        LINK jce_core jce_platform
        INCLUDES "${CMAKE_SOURCE_DIR}/editor/src/core")
    target_compile_features(${NAME} PRIVATE cxx_std_20)
endfunction()
set(_editor_config_owner "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_config.cpp")
_jce_editor_config_persistence_fixture(test_jce_editor_config_persistence
    "${_editor_config_owner}" integration)

# Compile the actual owner with only the old loss-of-dirty-state behavior.
# Both call sites are strict markers; semicolons are excluded from list counts.
file(READ "${_editor_config_owner}" _editor_config_source)
string(REGEX MATCHALL "s_dirty = !config_flush_to_disk\\(\\)"
    _editor_config_retry_markers "${_editor_config_source}")
list(LENGTH _editor_config_retry_markers _editor_config_retry_marker_count)
if(NOT _editor_config_retry_marker_count EQUAL 2)
    message(FATAL_ERROR "Config retry mutation requires the actual timed and forced flush wires")
endif()
string(REPLACE "s_dirty = !config_flush_to_disk();"
    "s_dirty = false; (void)config_flush_to_disk();"
    _editor_config_mutant "${_editor_config_source}")
if(_editor_config_mutant STREQUAL _editor_config_source)
    message(FATAL_ERROR "Config retry mutation did not change its owner")
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/config-retry-mutation")
set(_editor_config_mutant_source
    "${CMAKE_CURRENT_BINARY_DIR}/config-retry-mutation/jce_editor_config_old_clear_dirty.cpp")
if(EXISTS "${_editor_config_mutant_source}")
    file(READ "${_editor_config_mutant_source}" _editor_config_mutant_previous)
endif()
if(NOT _editor_config_mutant_previous STREQUAL _editor_config_mutant)
    file(WRITE "${_editor_config_mutant_source}" "${_editor_config_mutant}")
endif()
_jce_editor_config_persistence_fixture(test_jce_editor_config_persistence_old_clear_dirty
    "${_editor_config_mutant_source}" contract)
set_tests_properties(test_jce_editor_config_persistence_old_clear_dirty PROPERTIES WILL_FAIL TRUE)

# Complete shared panel owner; unrelated UI providers are typed counted ports.
# Visibility and Console CPU behavior are separate from OS/UI/GPU acceptance.
jce_add_cxx_test(test_jce_editor_panel_state KIND integration
    SOURCES test_jce_editor_panel_state.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/ui/jce_editor_panels.cpp"
    LINK jce_core jce_resource jce_tools_imgui)
target_compile_features(test_jce_editor_panel_state PRIVATE cxx_std_20)


# New-project continuation: real template, pipeline, manifest and host scene
# serialization; activation/default-scene boundaries are counted typed ports.
jce_add_cxx_test(test_jce_editor_new_project KIND integration
    SOURCES test_jce_editor_new_project.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_new_project.cpp"
            "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_project.cpp"
    LINK JCE)
target_compile_features(test_jce_editor_new_project PRIVATE cxx_std_20)
target_compile_definitions(test_jce_editor_new_project PRIVATE
    JCE_TEST_TEMPLATES_DIR="${CMAKE_SOURCE_DIR}/engine/templates")

# Complete project adapter with real metadata/JSON and atomic host IO. No UI.
jce_add_cxx_test(test_jce_editor_project_persistence KIND integration
    SOURCES test_jce_editor_project_persistence.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_project.cpp"
    LINK JCE)
target_compile_features(test_jce_editor_project_persistence PRIVATE cxx_std_20)

# Camera defaults actually used by 2D/3D scene builders, resolved and
# serialized/reloaded through the normal scene owner (no graphics context).
jce_add_cxx_test(test_jce_editor_default_camera KIND integration
    SOURCES test_jce_editor_default_camera.cpp
    LINK jce_core jce_scene jce_resource)
target_compile_features(test_jce_editor_default_camera PRIVATE cxx_std_20)
file(READ "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_default_camera.h" _default_camera_source)
string(REGEX MATCHALL "camera->is_primary = true" _default_camera_markers "${_default_camera_source}")
list(LENGTH _default_camera_markers _default_camera_marker_count)
if(NOT _default_camera_marker_count EQUAL 1)
    message(FATAL_ERROR "Default camera mutation requires its one actual primary assignment")
endif()
string(REPLACE "camera->is_primary = true" "camera->is_primary = false"
    _default_camera_mutant "${_default_camera_source}")
if(_default_camera_mutant STREQUAL _default_camera_source)
    message(FATAL_ERROR "Default camera mutation did not change its owner")
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/default-camera-mutation")
set(_default_camera_mutant_header
    "${CMAKE_CURRENT_BINARY_DIR}/default-camera-mutation/jce_editor_default_camera_old_primary.h")
if(EXISTS "${_default_camera_mutant_header}")
    file(READ "${_default_camera_mutant_header}" _default_camera_mutant_previous)
endif()
if(NOT _default_camera_mutant_previous STREQUAL _default_camera_mutant)
    file(WRITE "${_default_camera_mutant_header}" "${_default_camera_mutant}")
endif()
jce_add_cxx_test(test_jce_editor_default_camera_old_primary KIND contract
    SOURCES test_jce_editor_default_camera.cpp
    LINK jce_core jce_scene jce_resource)
target_compile_features(test_jce_editor_default_camera_old_primary PRIVATE cxx_std_20)
target_compile_definitions(test_jce_editor_default_camera_old_primary PRIVATE
    JCE_EDITOR_DEFAULT_CAMERA_HEADER="${_default_camera_mutant_header}")
set_tests_properties(test_jce_editor_default_camera_old_primary PROPERTIES WILL_FAIL TRUE)

# Actual grid, localization, JSON and logger owners; no UI/GPU startup.
jce_add_cxx_test(test_jce_editor_game_l10n KIND integration
    SOURCES test_jce_editor_game_l10n.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_game_l10n.cpp"
            "${CMAKE_SOURCE_DIR}/engine/src/middleware/ui/jce_localization.c"
    LINK jce_core jce_resource)
target_compile_features(test_jce_editor_game_l10n PRIVATE cxx_std_20)

# Compile the current complete factory/activation bodies against counted
# failure ports. No copied scene factory, physical ECS OOM or GUI claim.
file(READ "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_state.cpp" _new_scene_owner)
set(_new_scene_signature "bool jce_state_new_default_scene_for_project(bool is_2d)")
string(REGEX MATCHALL "bool jce_state_new_default_scene_for_project[(]bool is_2d[)]"
    _new_scene_signatures "${_new_scene_owner}")
list(LENGTH _new_scene_signatures _new_scene_signature_count)
if(NOT _new_scene_signature_count EQUAL 1)
    message(FATAL_ERROR "New scene fixture requires the unique production factory")
endif()
string(FIND "${_new_scene_owner}" "${_new_scene_signature}" _new_scene_begin)
string(FIND "${_new_scene_owner}" "\nvoid jce_editor_state_shutdown(void)" _new_scene_end)
if(_new_scene_begin LESS 0 OR _new_scene_end LESS_EQUAL _new_scene_begin)
    message(FATAL_ERROR "New scene production body bounds are invalid")
endif()
math(EXPR _new_scene_size "${_new_scene_end} - ${_new_scene_begin}")
string(SUBSTRING "${_new_scene_owner}" ${_new_scene_begin} ${_new_scene_size} _new_scene_body_normal)
string(REGEX MATCHALL "Detach the old scene identity before any destructive scene operation"
    _new_scene_detach_markers "${_new_scene_body_normal}")
list(LENGTH _new_scene_detach_markers _new_scene_detach_count)
if(NOT _new_scene_detach_count EQUAL 1)
    message(FATAL_ERROR "Old-identity control requires exactly one actual detach block")
endif()
string(FIND "${_new_scene_body_normal}"
    "    /* Detach the old scene identity before any destructive scene operation."
    _new_scene_detach_begin)
string(FIND "${_new_scene_body_normal}" "\n    if (!s.scene)" _new_scene_detach_end)
if(_new_scene_detach_begin LESS 0 OR _new_scene_detach_end LESS_EQUAL _new_scene_detach_begin)
    message(FATAL_ERROR "New scene identity mutation bounds are invalid")
endif()
string(SUBSTRING "${_new_scene_body_normal}" 0 ${_new_scene_detach_begin} _new_scene_before_detach)
string(SUBSTRING "${_new_scene_body_normal}" ${_new_scene_detach_end} -1 _new_scene_after_detach)
string(CONCAT _new_scene_body_old_identity "${_new_scene_before_detach}" "${_new_scene_after_detach}")
if(_new_scene_body_old_identity STREQUAL _new_scene_body_normal)
    message(FATAL_ERROR "Old-identity control did not remove the actual detach wire")
endif()
file(READ "${CMAKE_SOURCE_DIR}/editor/src/dialogs/jce_dialog_project.cpp" _new_project_dialog_owner)
string(REGEX MATCHALL "static bool activate_new_project[(]const char [*]root[)]"
    _new_project_activation_markers "${_new_project_dialog_owner}")
list(LENGTH _new_project_activation_markers _new_project_activation_count)
if(NOT _new_project_activation_count EQUAL 1)
    message(FATAL_ERROR "New project fixture requires the unique activation adapter")
endif()
string(FIND "${_new_project_dialog_owner}" "static bool activate_new_project(const char *root)"
    _new_project_activation_begin)
string(FIND "${_new_project_dialog_owner}" "\nbool is_valid_project_dir(const char *path)"
    _new_project_activation_end)
if(_new_project_activation_begin LESS 0 OR _new_project_activation_end LESS_EQUAL _new_project_activation_begin)
    message(FATAL_ERROR "New project activation body bounds are invalid")
endif()
math(EXPR _new_project_activation_size "${_new_project_activation_end} - ${_new_project_activation_begin}")
string(SUBSTRING "${_new_project_dialog_owner}" ${_new_project_activation_begin}
    ${_new_project_activation_size} _new_scene_body_activation)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/new-scene-failure")
foreach(_new_scene_variant IN ITEMS normal old_identity activation)
    set(_new_scene_content_var "_new_scene_body_${_new_scene_variant}")
    set(_new_scene_file "${CMAKE_CURRENT_BINARY_DIR}/new-scene-failure/${_new_scene_variant}.inc")
    unset(_new_scene_previous)
    if(EXISTS "${_new_scene_file}")
        file(READ "${_new_scene_file}" _new_scene_previous)
    endif()
    if(NOT _new_scene_previous STREQUAL "${${_new_scene_content_var}}")
        file(WRITE "${_new_scene_file}" "${${_new_scene_content_var}}")
    endif()
endforeach()
foreach(_new_scene_target IN ITEMS test_jce_editor_new_scene_failure test_jce_editor_new_scene_failure_old_identity)
    if(_new_scene_target STREQUAL "test_jce_editor_new_scene_failure")
        set(_new_scene_kind unit)
        set(_new_scene_variant normal)
    else()
        set(_new_scene_kind contract)
        set(_new_scene_variant old_identity)
    endif()
    jce_add_cxx_test(${_new_scene_target} KIND ${_new_scene_kind}
        SOURCES test_jce_editor_new_scene_failure.cpp LINK jce_core)
    target_compile_features(${_new_scene_target} PRIVATE cxx_std_20)
    target_compile_definitions(${_new_scene_target} PRIVATE
        JCE_TEST_EDITOR_NEW_SCENE_BODY="${CMAKE_CURRENT_BINARY_DIR}/new-scene-failure/${_new_scene_variant}.inc"
        JCE_TEST_EDITOR_NEW_PROJECT_ACTIVATION_BODY="${CMAKE_CURRENT_BINARY_DIR}/new-scene-failure/activation.inc")
endforeach()
set_tests_properties(test_jce_editor_new_scene_failure_old_identity PROPERTIES WILL_FAIL TRUE)

# Real public process children, complete stdout/stderr and host log failures.
jce_add_cxx_test(test_jce_editor_test_runner KIND integration
    SOURCES test_jce_editor_test_runner.cpp
            "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_test_run.cpp"
    LINK jce_core)
target_compile_features(test_jce_editor_test_runner PRIVATE cxx_std_20)
file(READ "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_test_run.cpp" _test_run_owner)
string(REGEX MATCHALL "run.exit_code == 0" _test_run_exit_guards "${_test_run_owner}")
list(LENGTH _test_run_exit_guards _test_run_exit_guard_count)
if(NOT _test_run_exit_guard_count EQUAL 1)
    message(FATAL_ERROR "Test runner exit-status mutation requires its unique actual guard")
endif()
string(REPLACE "run.exit_code == 0" "true" _test_run_old_exit "${_test_run_owner}")
if(_test_run_old_exit STREQUAL _test_run_owner)
    message(FATAL_ERROR "Test runner exit-status mutation did not change its owner")
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-runner-mutation")
set(_test_run_old_exit_source "${CMAKE_CURRENT_BINARY_DIR}/test-runner-mutation/jce_editor_test_run_old_exit.cpp")
unset(_test_run_old_previous)
if(EXISTS "${_test_run_old_exit_source}")
    file(READ "${_test_run_old_exit_source}" _test_run_old_previous)
endif()
if(NOT _test_run_old_previous STREQUAL _test_run_old_exit)
    file(WRITE "${_test_run_old_exit_source}" "${_test_run_old_exit}")
endif()
jce_add_cxx_test(test_jce_editor_test_runner_old_exit_status KIND contract
    SOURCES test_jce_editor_test_runner.cpp "${_test_run_old_exit_source}"
    LINK jce_core
    INCLUDES "${CMAKE_SOURCE_DIR}/editor/src/core")
target_compile_features(test_jce_editor_test_runner_old_exit_status PRIVATE cxx_std_20)
set_tests_properties(test_jce_editor_test_runner_old_exit_status PROPERTIES WILL_FAIL TRUE)
# Actual observed-completion deadline disabled, with exit-status checking intact.
string(REGEX MATCHALL "now<run.started_ms [|][|] now-run.started_ms>=run.timeout_ms"
    _test_run_deadline_guards "${_test_run_owner}")
list(LENGTH _test_run_deadline_guards _test_run_deadline_guard_count)
if(NOT _test_run_deadline_guard_count EQUAL 1)
    message(FATAL_ERROR "Test runner deadline mutation requires its unique actual guard")
endif()
string(REPLACE "now<run.started_ms || now-run.started_ms>=run.timeout_ms" "false"
    _test_run_old_deadline "${_test_run_owner}")
if(_test_run_old_deadline STREQUAL _test_run_owner)
    message(FATAL_ERROR "Test runner deadline mutation did not change its owner")
endif()
set(_test_run_old_deadline_source "${CMAKE_CURRENT_BINARY_DIR}/test-runner-mutation/jce_editor_test_run_old_deadline.cpp")
unset(_test_run_old_previous)
if(EXISTS "${_test_run_old_deadline_source}")
    file(READ "${_test_run_old_deadline_source}" _test_run_old_previous)
endif()
if(NOT _test_run_old_previous STREQUAL _test_run_old_deadline)
    file(WRITE "${_test_run_old_deadline_source}" "${_test_run_old_deadline}")
endif()
jce_add_cxx_test(test_jce_editor_test_runner_old_completion_deadline KIND contract
    SOURCES test_jce_editor_test_runner.cpp "${_test_run_old_deadline_source}"
    LINK jce_core
    INCLUDES "${CMAKE_SOURCE_DIR}/editor/src/core")
target_compile_features(test_jce_editor_test_runner_old_completion_deadline PRIVATE cxx_std_20)
set_tests_properties(test_jce_editor_test_runner_old_completion_deadline PROPERTIES WILL_FAIL TRUE)


# Authoring cadence + actual main/Play receiver, with the real lifecycle registry.
set(_editor_cadence_main "${CMAKE_SOURCE_DIR}/editor/src/jce_editor_main.cpp")
set(_editor_cadence_play "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_play.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_editor_cadence_main}" "${_editor_cadence_play}"
    "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_cadence.h"
    "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_frame_rate.h")
file(READ "${_editor_cadence_main}" _editor_cadence_main_source)
string(REGEX MATCHALL "jce_editor_play_host_tick[(]state[.]cadence_rates[.]idle, state[.]cadence_rates[.]play_background, state[.]cadence_rates[.]play[)]"
    _editor_cadence_main_calls "${_editor_cadence_main_source}")
list(LENGTH _editor_cadence_main_calls _editor_cadence_main_count)
if(NOT _editor_cadence_main_count EQUAL 1)
    message(FATAL_ERROR "Cadence Play control requires the unique actual main call")
endif()
file(READ "${_editor_cadence_play}" _editor_cadence_play_source)
string(REGEX MATCHALL "void jce_editor_play_host_tick[(]" _editor_cadence_receiver_markers "${_editor_cadence_play_source}")
list(LENGTH _editor_cadence_receiver_markers _editor_cadence_receiver_count)
if(NOT _editor_cadence_receiver_count EQUAL 1)
    message(FATAL_ERROR "Cadence control requires the unique actual Play receiver")
endif()
string(FIND "${_editor_cadence_play_source}" "void jce_editor_play_host_tick(" _editor_cadence_receiver_start)
string(FIND "${_editor_cadence_play_source}" "float jce_editor_play_render_delta(" _editor_cadence_receiver_end)
if(_editor_cadence_receiver_end LESS _editor_cadence_receiver_start)
    message(FATAL_ERROR "Cadence Play receiver boundary changed")
endif()
math(EXPR _editor_cadence_receiver_size "${_editor_cadence_receiver_end} - ${_editor_cadence_receiver_start}")
string(SUBSTRING "${_editor_cadence_play_source}" ${_editor_cadence_receiver_start} ${_editor_cadence_receiver_size}
    _editor_cadence_receiver)
function(_jce_editor_cadence_replace_once SOURCE WIRE REPLACEMENT OUT)
    string(FIND "${SOURCE}" "${WIRE}" _first)
    string(FIND "${SOURCE}" "${WIRE}" _last REVERSE)
    if(_first LESS 0 OR NOT _first EQUAL _last)
        message(FATAL_ERROR "Cadence control requires exactly one actual policy wire: ${WIRE}")
    endif()
    string(REPLACE "${WIRE}" "${REPLACEMENT}" _changed "${SOURCE}")
    if(_changed STREQUAL SOURCE)
        message(FATAL_ERROR "Cadence mutation left the actual owner unchanged")
    endif()
    set(${OUT} "${_changed}" PARENT_SCOPE)
endfunction()
# Capture the complete real begin expression, before the adjacent module begin.
# The test supplies only typed borrowed services/config and returns its result.
set(_editor_cadence_begin_marker "jce_runtime_host_policy_begin(&s_play_host.policy,")
set(_editor_cadence_begin_end_marker " ||\n        !jce_editor_play_module_begin(")
string(FIND "${_editor_cadence_play_source}" "${_editor_cadence_begin_marker}" _editor_cadence_begin_start)
string(FIND "${_editor_cadence_play_source}" "${_editor_cadence_begin_marker}" _editor_cadence_begin_start_last REVERSE)
string(FIND "${_editor_cadence_play_source}" "${_editor_cadence_begin_end_marker}" _editor_cadence_begin_end)
string(FIND "${_editor_cadence_play_source}" "${_editor_cadence_begin_end_marker}" _editor_cadence_begin_end_last REVERSE)
if(_editor_cadence_begin_start LESS 0 OR _editor_cadence_begin_end LESS_EQUAL _editor_cadence_begin_start OR
   NOT _editor_cadence_begin_start EQUAL _editor_cadence_begin_start_last OR
   NOT _editor_cadence_begin_end EQUAL _editor_cadence_begin_end_last)
    message(FATAL_ERROR "Cadence initial policy begin requires unique ordered actual expression boundaries")
endif()
math(EXPR _editor_cadence_begin_size "${_editor_cadence_begin_end} - ${_editor_cadence_begin_start}")
string(SUBSTRING "${_editor_cadence_play_source}" ${_editor_cadence_begin_start} ${_editor_cadence_begin_size}
    _editor_cadence_initial_expression)
set(_editor_cadence_initial_begin "return ${_editor_cadence_initial_expression};\n")
set(_editor_cadence_pause_wire "jce_editor_play_preview_fps(s_play_host.presentation.running_target_fps, cadence_rates.idle)")
set(_editor_cadence_background_wire "jce_editor_play_preview_fps(s_play_host.presentation.running_target_fps, cadence_rates.play_background)")
_jce_editor_cadence_replace_once("${_editor_cadence_initial_expression}"
    "${_editor_cadence_pause_wire}" "30" _editor_cadence_old_initial_expression)
_jce_editor_cadence_replace_once("${_editor_cadence_old_initial_expression}"
    "${_editor_cadence_background_wire}" "15" _editor_cadence_old_initial_expression)
set(_editor_cadence_old_initial "return ${_editor_cadence_old_initial_expression};\n")
set(_editor_cadence_temporary_pause_wire "    s_play_host.policy.paused_fps = jce_editor_play_preview_fps(\n        s_play_host.presentation.running_target_fps, paused_fps);")
set(_editor_cadence_temporary_background_wire "    s_play_host.policy.background_fps = jce_editor_play_preview_fps(\n        s_play_host.presentation.running_target_fps, background_fps);")
_jce_editor_cadence_replace_once("${_editor_cadence_receiver}"
    "${_editor_cadence_temporary_pause_wire}"
    "    s_play_host.policy.paused_fps = paused_fps < 0 ? 0 : paused_fps;" _editor_cadence_old_temporary)
_jce_editor_cadence_replace_once("${_editor_cadence_old_temporary}"
    "${_editor_cadence_temporary_background_wire}"
    "    s_play_host.policy.background_fps = background_fps < 0 ? 0 : background_fps;" _editor_cadence_old_temporary)
set(_editor_cadence_pause_marker "void jce_state_pause(void)")
set(_editor_cadence_pause_end_marker "void jce_state_stop(void)")
string(FIND "${_editor_cadence_play_source}" "${_editor_cadence_pause_marker}" _editor_cadence_pause_start)
string(FIND "${_editor_cadence_play_source}" "${_editor_cadence_pause_marker}" _editor_cadence_pause_start_last REVERSE)
string(FIND "${_editor_cadence_play_source}" "${_editor_cadence_pause_end_marker}" _editor_cadence_pause_end)
string(FIND "${_editor_cadence_play_source}" "${_editor_cadence_pause_end_marker}" _editor_cadence_pause_end_last REVERSE)
if(_editor_cadence_pause_start LESS 0 OR _editor_cadence_pause_end LESS_EQUAL _editor_cadence_pause_start OR
   NOT _editor_cadence_pause_start EQUAL _editor_cadence_pause_start_last OR
   NOT _editor_cadence_pause_end EQUAL _editor_cadence_pause_end_last)
    message(FATAL_ERROR "Cadence Pause control requires unique ordered actual function boundaries")
endif()
math(EXPR _editor_cadence_pause_size "${_editor_cadence_pause_end} - ${_editor_cadence_pause_start}")
string(SUBSTRING "${_editor_cadence_play_source}" ${_editor_cadence_pause_start} ${_editor_cadence_pause_size}
    _editor_cadence_pause_body)
set(_editor_cadence_pause_adapter_call "jce_editor_play_host_tick(s_play_host.requested_paused_fps,\n            s_play_host.requested_background_fps, s_play_host.presentation.preview_cap);")
_jce_editor_cadence_replace_once("${_editor_cadence_pause_body}"
    "s_play_host.render_dt = 0.0f;\n        ${_editor_cadence_pause_adapter_call}"
    "s_play_host.render_dt = 0.0f;\n        jce_runtime_host_policy_tick(&s_play_host.policy, true);"
    _editor_cadence_old_pause)
_jce_editor_cadence_replace_once("${_editor_cadence_old_pause}"
    "s.play_state = JCE_PLAY_PLAYING;\n        ${_editor_cadence_pause_adapter_call}"
    "s.play_state = JCE_PLAY_PLAYING;\n        jce_runtime_host_policy_tick(&s_play_host.policy, false);"
    _editor_cadence_old_pause)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/cadence-mutation")
set(_editor_cadence_receiver_path "${CMAKE_CURRENT_BINARY_DIR}/cadence-mutation/jce_editor_play_host_tick.inc.h")
set(_editor_cadence_host_call "${CMAKE_CURRENT_BINARY_DIR}/cadence-mutation/jce_editor_cadence_main_play.inc.h")
set(_editor_cadence_old_host_call "${CMAKE_CURRENT_BINARY_DIR}/cadence-mutation/jce_editor_cadence_main_old_shared.inc.h")
set(_editor_cadence_preview_host_call "${CMAKE_CURRENT_BINARY_DIR}/cadence-mutation/jce_editor_cadence_main_missing_preview.inc.h")
set(_editor_cadence_initial_begin_path "${CMAKE_CURRENT_BINARY_DIR}/cadence-mutation/jce_editor_play_policy_begin.inc.h")
set(_editor_cadence_old_initial_path "${CMAKE_CURRENT_BINARY_DIR}/cadence-mutation/jce_editor_play_policy_begin_old_caps.inc.h")
set(_editor_cadence_old_temporary_path "${CMAKE_CURRENT_BINARY_DIR}/cadence-mutation/jce_editor_play_host_old_temporary_caps.inc.h")
set(_editor_cadence_pause_body_path "${CMAKE_CURRENT_BINARY_DIR}/cadence-mutation/jce_editor_play_pause.inc.h")
set(_editor_cadence_old_pause_path "${CMAKE_CURRENT_BINARY_DIR}/cadence-mutation/jce_editor_play_pause_old_caps.inc.h")
set(_editor_cadence_actual_call "${_editor_cadence_main_calls};\n")
string(REPLACE ".play_background" ".background" _editor_cadence_old_call "${_editor_cadence_actual_call}")
if(_editor_cadence_actual_call STREQUAL _editor_cadence_old_call)
    message(FATAL_ERROR "Cadence shared-background mutation did not change the actual call")
endif()
string(REPLACE ", state.cadence_rates.play)" ", 0)" _editor_cadence_preview_call "${_editor_cadence_actual_call}")
if(_editor_cadence_actual_call STREQUAL _editor_cadence_preview_call)
    message(FATAL_ERROR "Preview cadence mutation did not change its actual main call")
endif()
foreach(_cadence_piece IN ITEMS receiver actual_call old_call preview_call initial_begin old_initial old_temporary pause_body old_pause)
    if(_cadence_piece STREQUAL "receiver")
        set(_cadence_path "${_editor_cadence_receiver_path}")
    elseif(_cadence_piece STREQUAL "actual_call")
        set(_cadence_path "${_editor_cadence_host_call}")
    elseif(_cadence_piece STREQUAL "preview_call")
        set(_cadence_path "${_editor_cadence_preview_host_call}")
    elseif(_cadence_piece STREQUAL "initial_begin")
        set(_cadence_path "${_editor_cadence_initial_begin_path}")
    elseif(_cadence_piece STREQUAL "old_initial")
        set(_cadence_path "${_editor_cadence_old_initial_path}")
    elseif(_cadence_piece STREQUAL "old_temporary")
        set(_cadence_path "${_editor_cadence_old_temporary_path}")
    elseif(_cadence_piece STREQUAL "pause_body")
        set(_cadence_path "${_editor_cadence_pause_body_path}")
    elseif(_cadence_piece STREQUAL "old_pause")
        set(_cadence_path "${_editor_cadence_old_pause_path}")
    else()
        set(_cadence_path "${_editor_cadence_old_host_call}")
    endif()
    unset(_cadence_previous)
    if(EXISTS "${_cadence_path}")
        file(READ "${_cadence_path}" _cadence_previous)
    endif()
    if(NOT _cadence_previous STREQUAL _editor_cadence_${_cadence_piece})
        file(WRITE "${_cadence_path}" "${_editor_cadence_${_cadence_piece}}")
    endif()
endforeach()
function(_jce_editor_cadence_fixture target kind)
    jce_add_cxx_test(${target} KIND ${kind}
        SOURCES test_jce_editor_cadence.cpp
                "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_config.cpp"
                "${CMAKE_SOURCE_DIR}/engine/src/application/jce_lifecycle.c"
        LINK jce_core jce_platform)
    target_compile_features(${target} PRIVATE cxx_std_20)
    set(_cadence_call "${_editor_cadence_host_call}")
    set(_cadence_receiver "${_editor_cadence_receiver_path}")
    set(_cadence_initial "${_editor_cadence_initial_begin_path}")
    set(_cadence_pause "${_editor_cadence_pause_body_path}")
    if(ARGC GREATER 2)
        set(_cadence_call "${ARGV2}")
    endif()
    if(ARGC GREATER 3)
        set(_cadence_receiver "${ARGV3}")
    endif()
    if(ARGC GREATER 4)
        set(_cadence_initial "${ARGV4}")
    endif()
    if(ARGC GREATER 5)
        set(_cadence_pause "${ARGV5}")
    endif()
    target_compile_definitions(${target} PRIVATE
        JCE_TEST_EDITOR_CADENCE_HOST_CALL="${_cadence_call}"
        JCE_TEST_EDITOR_PLAY_HOST_BODY="${_cadence_receiver}"
        JCE_TEST_EDITOR_PLAY_POLICY_BEGIN="${_cadence_initial}"
        JCE_TEST_EDITOR_PLAY_PAUSE_BODY="${_cadence_pause}")
endfunction()
_jce_editor_cadence_fixture(test_jce_editor_cadence unit)
file(READ "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_cadence.h" _editor_cadence_owner)
string(REGEX MATCHALL "jce_engine_set_target_fps[(]target[)]" _editor_cadence_wires "${_editor_cadence_owner}")
list(LENGTH _editor_cadence_wires _editor_cadence_wire_count)
if(NOT _editor_cadence_wire_count EQUAL 1)
    message(FATAL_ERROR "Editor cadence requires one actual target-FPS apply wire")
endif()
string(REPLACE "jce_engine_set_target_fps(target);" "(void)target;" _editor_cadence_old "${_editor_cadence_owner}")
if(_editor_cadence_old STREQUAL _editor_cadence_owner)
    message(FATAL_ERROR "Editor cadence mutation did not change its actual owner")
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/cadence-mutation")
set(_editor_cadence_old_header "${CMAKE_CURRENT_BINARY_DIR}/cadence-mutation/jce_editor_cadence_old_apply.h")
unset(_editor_cadence_previous)
if(EXISTS "${_editor_cadence_old_header}")
    file(READ "${_editor_cadence_old_header}" _editor_cadence_previous)
endif()
if(NOT _editor_cadence_previous STREQUAL _editor_cadence_old)
    file(WRITE "${_editor_cadence_old_header}" "${_editor_cadence_old}")
endif()
_jce_editor_cadence_fixture(test_jce_editor_cadence_old_apply contract)
target_include_directories(test_jce_editor_cadence_old_apply PRIVATE "${CMAKE_SOURCE_DIR}/editor/src/core")
target_compile_definitions(test_jce_editor_cadence_old_apply PRIVATE
    JCE_TEST_EDITOR_CADENCE_HEADER="${_editor_cadence_old_header}")
set_tests_properties(test_jce_editor_cadence_old_apply PROPERTIES WILL_FAIL TRUE)
_jce_editor_cadence_fixture(test_jce_editor_cadence_old_shared_background contract "${_editor_cadence_old_host_call}")
set_tests_properties(test_jce_editor_cadence_old_shared_background PROPERTIES WILL_FAIL TRUE)
_jce_editor_cadence_fixture(test_jce_editor_cadence_missing_preview_cap contract "${_editor_cadence_preview_host_call}")
set_tests_properties(test_jce_editor_cadence_missing_preview_cap PROPERTIES WILL_FAIL TRUE)
_jce_editor_cadence_fixture(test_jce_editor_cadence_old_temporary_caps contract
    "${_editor_cadence_host_call}" "${_editor_cadence_old_temporary_path}")
_jce_editor_cadence_fixture(test_jce_editor_cadence_old_initial_caps contract
    "${_editor_cadence_host_call}" "${_editor_cadence_receiver_path}" "${_editor_cadence_old_initial_path}")
_jce_editor_cadence_fixture(test_jce_editor_cadence_old_pause_caps contract
    "${_editor_cadence_host_call}" "${_editor_cadence_receiver_path}"
    "${_editor_cadence_initial_begin_path}" "${_editor_cadence_old_pause_path}")
# Restore the former independent idle/background selection while keeping the
# complete real cadence header, declarations and FPS-apply wire intact.
set(_editor_cadence_limits_wire "    int target = idle ? jce_editor_frame_rate_limit(rates.active, rates.idle) : rates.active;\n    if (!focused) target = jce_editor_frame_rate_limit(target, rates.background);")
_jce_editor_cadence_replace_once("${_editor_cadence_owner}"
    "${_editor_cadence_limits_wire}"
    "    const int target = !focused ? rates.background : idle ? rates.idle : rates.active;"
    _editor_cadence_old_limits)
set(_editor_cadence_old_limits_path "${CMAKE_CURRENT_BINARY_DIR}/cadence-mutation/jce_editor_cadence_old_limits.h")
unset(_cadence_previous)
if(EXISTS "${_editor_cadence_old_limits_path}")
    file(READ "${_editor_cadence_old_limits_path}" _cadence_previous)
endif()
if(NOT _cadence_previous STREQUAL _editor_cadence_old_limits)
    file(WRITE "${_editor_cadence_old_limits_path}" "${_editor_cadence_old_limits}")
endif()
_jce_editor_cadence_fixture(test_jce_editor_cadence_old_limits contract)
target_include_directories(test_jce_editor_cadence_old_limits PRIVATE "${CMAKE_SOURCE_DIR}/editor/src/core")
target_compile_definitions(test_jce_editor_cadence_old_limits PRIVATE
    JCE_TEST_EDITOR_CADENCE_HEADER="${_editor_cadence_old_limits_path}")
set_tests_properties(test_jce_editor_cadence_old_temporary_caps
    test_jce_editor_cadence_old_initial_caps test_jce_editor_cadence_old_pause_caps test_jce_editor_cadence_old_limits
    PROPERTIES WILL_FAIL TRUE)

# Exact visible Editor pre-frame callsite; the CPU-only service poll stays
# separate. Runtime negative removes only this wire, not the helper's policy.
set(_editor_viewport_core "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_editor_viewport_core}"
    "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_viewport_work.h")
file(READ "${_editor_viewport_core}" _editor_viewport_frame_source)
foreach(_editor_viewport_marker IN ITEMS "Begin ImGui frame[.]" "Horizontal scroll direction")
    string(REGEX MATCHALL "${_editor_viewport_marker}" _editor_viewport_markers "${_editor_viewport_frame_source}")
    list(LENGTH _editor_viewport_markers _editor_viewport_marker_count)
    if(NOT _editor_viewport_marker_count EQUAL 1)
        message(FATAL_ERROR "Viewport focus callsite requires unique actual pre-frame bounds")
    endif()
endforeach()
string(FIND "${_editor_viewport_frame_source}" "    /* Begin ImGui frame. */" _editor_viewport_frame_begin)
string(FIND "${_editor_viewport_frame_source}" "    /* Horizontal scroll direction" _editor_viewport_frame_end)
if(_editor_viewport_frame_begin LESS 0 OR _editor_viewport_frame_end LESS_EQUAL _editor_viewport_frame_begin)
    message(FATAL_ERROR "Viewport focus pre-frame extraction bounds invalid")
endif()
math(EXPR _editor_viewport_frame_size "${_editor_viewport_frame_end} - ${_editor_viewport_frame_begin}")
string(SUBSTRING "${_editor_viewport_frame_source}" ${_editor_viewport_frame_begin}
    ${_editor_viewport_frame_size} _editor_viewport_frame_normal)
string(REGEX MATCHALL "jce_editor_viewport_sync_window_focus[(]window[)]"
    _editor_viewport_focus_wires "${_editor_viewport_frame_normal}")
list(LENGTH _editor_viewport_focus_wires _editor_viewport_focus_wire_count)
string(REGEX MATCHALL "ImGui::NewFrame[(][)]" _editor_viewport_newframes "${_editor_viewport_frame_normal}")
list(LENGTH _editor_viewport_newframes _editor_viewport_newframe_count)
if(NOT _editor_viewport_focus_wire_count EQUAL 1 OR NOT _editor_viewport_newframe_count EQUAL 1)
    message(FATAL_ERROR "Viewport focus controls require one actual sync wire and NewFrame")
endif()
string(REPLACE "jce_editor_viewport_sync_window_focus(window);" "(void)window;"
    _editor_viewport_frame_missing_focus_sync "${_editor_viewport_frame_normal}")
if(_editor_viewport_frame_missing_focus_sync STREQUAL _editor_viewport_frame_normal)
    message(FATAL_ERROR "Viewport missing-focus mutation did not change the actual callsite")
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/cadence-mutation")
foreach(_editor_viewport_variant IN ITEMS normal missing_focus_sync)
    set(_editor_viewport_body_var "_editor_viewport_frame_${_editor_viewport_variant}")
    set(_editor_viewport_body_path "${CMAKE_CURRENT_BINARY_DIR}/cadence-mutation/jce_editor_viewport_frame_${_editor_viewport_variant}.inc.h")
    unset(_editor_viewport_previous)
    if(EXISTS "${_editor_viewport_body_path}")
        file(READ "${_editor_viewport_body_path}" _editor_viewport_previous)
    endif()
    if(NOT _editor_viewport_previous STREQUAL ${_editor_viewport_body_var})
        file(WRITE "${_editor_viewport_body_path}" "${${_editor_viewport_body_var}}")
    endif()
endforeach()

function(_jce_editor_viewport_work_fixture target kind)
    jce_add_cxx_test(${target} KIND ${kind}
        SOURCES test_jce_editor_viewport_work.cpp LINK jce_core jce_tools_imgui)
    target_compile_features(${target} PRIVATE cxx_std_20)
    # Whole-owner mutations live under build; resolve their same-directory
    # private dependencies from the actual production owner, never copied stubs.
    target_include_directories(${target} PRIVATE "${CMAKE_SOURCE_DIR}/editor/src/core")
    set(_editor_viewport_variant normal)
    if(ARGC GREATER 2)
        set(_editor_viewport_variant "${ARGV2}")
    endif()
    target_compile_definitions(${target} PRIVATE
        JCE_TEST_EDITOR_VIEWPORT_FRAME_BODY="${CMAKE_CURRENT_BINARY_DIR}/cadence-mutation/jce_editor_viewport_frame_${_editor_viewport_variant}.inc.h")
endfunction()
_jce_editor_viewport_work_fixture(test_jce_editor_viewport_work unit)
file(READ "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_viewport_work.h" _editor_viewport_work_owner)
string(REGEX MATCHALL "return !ImGui::GetIO[(][)].AppFocusLost &&" _editor_viewport_work_guards "${_editor_viewport_work_owner}")
list(LENGTH _editor_viewport_work_guards _editor_viewport_work_guard_count)
if(NOT _editor_viewport_work_guard_count EQUAL 1)
    message(FATAL_ERROR "Viewport work mutation requires its actual focus/clip gate")
endif()
string(REPLACE "!ImGui::GetIO().AppFocusLost &&\n        ImGui::IsRectVisible(start, ImVec2(start.x + size.x, start.y + size.y))"
    "true" _editor_viewport_work_old "${_editor_viewport_work_owner}")
if(_editor_viewport_work_old STREQUAL _editor_viewport_work_owner)
    message(FATAL_ERROR "Viewport work mutation did not change its actual owner")
endif()
set(_editor_viewport_work_old_header "${CMAKE_CURRENT_BINARY_DIR}/cadence-mutation/jce_editor_viewport_work_old_visibility.h")
unset(_editor_viewport_work_previous)
if(EXISTS "${_editor_viewport_work_old_header}")
    file(READ "${_editor_viewport_work_old_header}" _editor_viewport_work_previous)
endif()
if(NOT _editor_viewport_work_previous STREQUAL _editor_viewport_work_old)
    file(WRITE "${_editor_viewport_work_old_header}" "${_editor_viewport_work_old}")
endif()
_jce_editor_viewport_work_fixture(test_jce_editor_viewport_work_old_visibility contract)
target_compile_definitions(test_jce_editor_viewport_work_old_visibility PRIVATE
    JCE_TEST_EDITOR_VIEWPORT_WORK_HEADER="${_editor_viewport_work_old_header}")
set_tests_properties(test_jce_editor_viewport_work_old_visibility PROPERTIES WILL_FAIL TRUE)
_jce_editor_viewport_work_fixture(test_jce_editor_viewport_work_missing_focus_sync contract missing_focus_sync)
set_tests_properties(test_jce_editor_viewport_work_missing_focus_sync PROPERTIES WILL_FAIL TRUE)

# Module callback owner is independent of Game View render admission.
jce_add_cxx_test(test_jce_editor_play_module
    SOURCES test_jce_editor_play_module.cpp LINK jce_core)
target_compile_features(test_jce_editor_play_module PRIVATE cxx_std_20)
file(READ "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_play_module.h" _play_module_owner)
string(REGEX MATCHALL "if [(]module->update[)] module->update[(]dt, module->user_data[)]" _play_module_update_wires "${_play_module_owner}")
string(REGEX MATCHALL "if [(]module->draw[)] module->draw[(]state.services, module->user_data[)]" _play_module_draw_wires "${_play_module_owner}")
list(LENGTH _play_module_update_wires _play_module_update_wire_count)
list(LENGTH _play_module_draw_wires _play_module_draw_wire_count)
if(NOT _play_module_update_wire_count EQUAL 1 OR NOT _play_module_draw_wire_count EQUAL 1)
    message(FATAL_ERROR "Play module old draw binding requires unique actual update/draw wires")
endif()
string(REPLACE "if (module->update) module->update(dt, module->user_data);"
    "(void)dt; /* old module updated only during view draw */" _play_module_old "${_play_module_owner}")
string(REPLACE "if (module->draw) module->draw(state.services, module->user_data);"
    "if (module->update) module->update(dt, module->user_data);\n    if (module->draw) module->draw(state.services, module->user_data);"
    _play_module_old "${_play_module_old}")
if(_play_module_old STREQUAL _play_module_owner)
    message(FATAL_ERROR "Play module old draw binding mutation did not change its owner")
endif()
set(_play_module_old_header "${CMAKE_CURRENT_BINARY_DIR}/cadence-mutation/jce_editor_play_module_old_draw.h")
unset(_play_module_previous)
if(EXISTS "${_play_module_old_header}")
    file(READ "${_play_module_old_header}" _play_module_previous)
endif()
if(NOT _play_module_previous STREQUAL _play_module_old)
    file(WRITE "${_play_module_old_header}" "${_play_module_old}")
endif()
jce_add_cxx_test(test_jce_editor_play_module_old_draw_bound KIND contract
    SOURCES test_jce_editor_play_module.cpp LINK jce_core)
target_compile_features(test_jce_editor_play_module_old_draw_bound PRIVATE cxx_std_20)
target_compile_definitions(test_jce_editor_play_module_old_draw_bound PRIVATE
    JCE_TEST_EDITOR_PLAY_MODULE_HEADER="${_play_module_old_header}")
set_tests_properties(test_jce_editor_play_module_old_draw_bound PROPERTIES WILL_FAIL TRUE)

include(host_services_controls.cmake)
# Actual project-state atomic publication: failed timed/forced writes must
# preserve dirty state for the existing retry/debounce owner. No GUI required.
function(_jce_editor_pstate_retry_fixture NAME OWNER KIND)
    jce_add_cxx_test(${NAME} KIND ${KIND}
        SOURCES test_jce_editor_pstate_persistence.cpp "${OWNER}"
        LINK jce_core jce_platform
        INCLUDES "${CMAKE_SOURCE_DIR}/editor/src/core")
    target_compile_features(${NAME} PRIVATE cxx_std_20)
endfunction()
set(_editor_pstate_owner "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_project_state.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_editor_pstate_owner}")
_jce_editor_pstate_retry_fixture(test_jce_editor_pstate_persistence
    "${_editor_pstate_owner}" integration)
file(READ "${_editor_pstate_owner}" _editor_pstate_source)
string(REGEX MATCHALL "s_dirty = !flush_to_disk\\(s_root\\)"
    _editor_pstate_retry_markers "${_editor_pstate_source}")
list(LENGTH _editor_pstate_retry_markers _editor_pstate_retry_marker_count)
if(NOT _editor_pstate_retry_marker_count EQUAL 2)
    message(FATAL_ERROR "Project-state retry mutation requires actual timed and forced wires")
endif()
string(REPLACE "s_dirty = !flush_to_disk(s_root);"
    "s_dirty = false; (void)flush_to_disk(s_root);"
    _editor_pstate_mutant "${_editor_pstate_source}")
if(_editor_pstate_mutant STREQUAL _editor_pstate_source)
    message(FATAL_ERROR "Project-state retry mutation did not change its actual owner")
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/pstate-retry-mutation")
set(_editor_pstate_mutant_source
    "${CMAKE_CURRENT_BINARY_DIR}/pstate-retry-mutation/jce_editor_project_state_old_clear_dirty.cpp")
if(EXISTS "${_editor_pstate_mutant_source}")
    file(READ "${_editor_pstate_mutant_source}" _editor_pstate_previous)
endif()
if(NOT _editor_pstate_previous STREQUAL _editor_pstate_mutant)
    file(WRITE "${_editor_pstate_mutant_source}" "${_editor_pstate_mutant}")
endif()
_jce_editor_pstate_retry_fixture(test_jce_editor_pstate_persistence_old_clear_dirty
    "${_editor_pstate_mutant_source}" contract)
set_tests_properties(test_jce_editor_pstate_persistence_old_clear_dirty PROPERTIES WILL_FAIL TRUE)

# Real public Archive/PAK/VFS/JSON input with conflicting working directories.
# Only the private project-quality snapshot ports are counted; no GUI/GPU starts.
function(_jce_editor_render_settings_vfs_fixture NAME OWNER KIND)
    jce_add_cxx_test(${NAME} KIND ${KIND}
        SOURCES test_jce_editor_render_settings_vfs.cpp "${OWNER}"
            "${CMAKE_SOURCE_DIR}/editor/src/scene/jce_scene_content_context.cpp"
        LINK jce_core jce_renderer jce_resource)
    target_compile_features(${NAME} PRIVATE cxx_std_20)
endfunction()
set(_editor_render_settings_owner
    "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_effective_render_settings.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_editor_render_settings_owner}")
_jce_editor_render_settings_vfs_fixture(test_jce_editor_render_settings_vfs
    "${_editor_render_settings_owner}" integration)
file(READ "${_editor_render_settings_owner}" _editor_render_settings_source)
string(REGEX MATCHALL "return editor_load_isolated_render_settings[(]path, out[)]"
    _editor_render_settings_wires "${_editor_render_settings_source}")
list(LENGTH _editor_render_settings_wires _editor_render_settings_wire_count)
if(NOT _editor_render_settings_wire_count EQUAL 1)
    message(FATAL_ERROR "Isolated RenderSettings mutation requires its unique real dispatch wire")
endif()
string(REPLACE "return editor_load_isolated_render_settings(path, out);"
    "return (static_cast<void>(&editor_load_isolated_render_settings), jce_render_settings_load_json(path.c_str(), out));"
    _editor_render_settings_old_host "${_editor_render_settings_source}")
if(_editor_render_settings_old_host STREQUAL _editor_render_settings_source)
    message(FATAL_ERROR "Isolated RenderSettings mutation did not replace its real dispatch")
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/render-settings-vfs-mutation")
set(_editor_render_settings_old_host_source
    "${CMAKE_CURRENT_BINARY_DIR}/render-settings-vfs-mutation/jce_editor_effective_render_settings_old_host.cpp")
if(EXISTS "${_editor_render_settings_old_host_source}")
    file(READ "${_editor_render_settings_old_host_source}" _editor_render_settings_previous)
endif()
if(NOT _editor_render_settings_previous STREQUAL _editor_render_settings_old_host)
    file(WRITE "${_editor_render_settings_old_host_source}" "${_editor_render_settings_old_host}")
endif()
_jce_editor_render_settings_vfs_fixture(test_jce_editor_render_settings_vfs_old_host_read
    "${_editor_render_settings_old_host_source}" contract)
set_tests_properties(test_jce_editor_render_settings_vfs_old_host_read PROPERTIES WILL_FAIL TRUE)

# Actual Play request/publication and Game View extent delegation with CPU ports.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_play.cpp"
    "${CMAKE_SOURCE_DIR}/editor/src/scene/jce_editor_game_render.cpp")
file(READ "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_play.cpp" _play_viewport_source)
string(FIND "${_play_viewport_source}" "void jce_state_play(void)" _play_viewport_begin)
string(FIND "${_play_viewport_source}" "void jce_state_pause(void)" _play_viewport_end)
if(_play_viewport_begin LESS 0 OR _play_viewport_end LESS _play_viewport_begin)
    message(FATAL_ERROR "Play viewport fixture requires actual request/publication owners")
endif()
math(EXPR _play_viewport_length "${_play_viewport_end} - ${_play_viewport_begin}")
string(SUBSTRING "${_play_viewport_source}" ${_play_viewport_begin} ${_play_viewport_length} _play_viewport_wire)
file(READ "${CMAKE_SOURCE_DIR}/editor/src/scene/jce_editor_game_render.cpp" _play_extent_source)
string(FIND "${_play_extent_source}" "void jce_editor_game_render_resolve_extent(" _play_extent_begin)
string(FIND "${_play_extent_source}" "void jce_editor_game_render_frame(" _play_extent_end)
if(_play_extent_begin LESS 0 OR _play_extent_end LESS _play_extent_begin)
    message(FATAL_ERROR "Play viewport fixture requires its actual pipeline extent adapter")
endif()
math(EXPR _play_extent_length "${_play_extent_end} - ${_play_extent_begin}")
string(SUBSTRING "${_play_extent_source}" ${_play_extent_begin} ${_play_extent_length} _play_extent_wire)
set(_play_viewport_wire "${_play_extent_wire}
${_play_viewport_wire}")
set(_play_viewport_directory "${CMAKE_CURRENT_BINARY_DIR}/play-viewport-mutation")
file(MAKE_DIRECTORY "${_play_viewport_directory}")
set(_play_viewport_normal "${_play_viewport_directory}/jce_editor_play_viewport_wire.inc")
unset(_viewport_previous_generated)
if(EXISTS "${_play_viewport_normal}")
    file(READ "${_play_viewport_normal}" _viewport_previous_generated)
endif()
if(NOT _viewport_previous_generated STREQUAL _play_viewport_wire)
    file(WRITE "${_play_viewport_normal}" "${_play_viewport_wire}")
endif()
unset(_viewport_previous_generated)
jce_add_cxx_test(test_jce_editor_play_viewport
    SOURCES test_jce_editor_play_viewport.cpp
    INCLUDES "${_play_viewport_directory}"
    LINK jce_core)
target_compile_features(test_jce_editor_play_viewport PRIVATE cxx_std_20)
foreach(_play_viewport_kind IN ITEMS begin extent)
    if(_play_viewport_kind STREQUAL "begin")
        set(_play_viewport_marker "(void)jce_editor_play_viewport_begin_pending(s_play_host.viewport, [] { play_begin_ready(); })")
        set(_play_viewport_old "(void)0")
    else()
        set(_play_viewport_marker "if (s_play_host.runtime_stats.runtime)")
        set(_play_viewport_old "if (false)")
    endif()
    string(FIND "${_play_viewport_wire}" "${_play_viewport_marker}" _play_viewport_mark)
    if(_play_viewport_mark LESS 0)
        message(FATAL_ERROR "Play viewport control requires its actual ${_play_viewport_kind} wire")
    endif()
    string(LENGTH "${_play_viewport_marker}" _play_viewport_marker_length)
    math(EXPR _play_viewport_after_marker "${_play_viewport_mark} + ${_play_viewport_marker_length}")
    string(SUBSTRING "${_play_viewport_wire}" ${_play_viewport_after_marker} -1 _play_viewport_remainder)
    string(FIND "${_play_viewport_remainder}" "${_play_viewport_marker}" _play_viewport_second_marker)
    if(NOT _play_viewport_second_marker LESS 0)
        message(FATAL_ERROR "Viewport mutation requires one unique actual wire")
    endif()
    string(REPLACE "${_play_viewport_marker}" "${_play_viewport_old}" _play_viewport_mutant "${_play_viewport_wire}")
    set(_play_viewport_mutant_path "${_play_viewport_directory}/jce_editor_play_viewport_old_${_play_viewport_kind}.inc")
    unset(_viewport_previous_generated)
    if(EXISTS "${_play_viewport_mutant_path}")
        file(READ "${_play_viewport_mutant_path}" _viewport_previous_generated)
    endif()
    if(NOT _viewport_previous_generated STREQUAL _play_viewport_mutant)
        file(WRITE "${_play_viewport_mutant_path}" "${_play_viewport_mutant}")
    endif()
    unset(_viewport_previous_generated)
    jce_add_cxx_test(test_jce_editor_play_viewport_old_${_play_viewport_kind}
        KIND contract SOURCES test_jce_editor_play_viewport.cpp LINK jce_core)
    target_compile_features(test_jce_editor_play_viewport_old_${_play_viewport_kind} PRIVATE cxx_std_20)
    target_compile_definitions(test_jce_editor_play_viewport_old_${_play_viewport_kind} PRIVATE
        JCE_EDITOR_PLAY_VIEWPORT_WIRE_HEADER="${_play_viewport_mutant_path}")
    set_tests_properties(test_jce_editor_play_viewport_old_${_play_viewport_kind} PROPERTIES WILL_FAIL TRUE)
endforeach()
unset(_play_viewport_source)
unset(_play_viewport_begin)
unset(_play_viewport_end)
unset(_play_viewport_length)
unset(_play_viewport_wire)
unset(_play_extent_source)
unset(_play_extent_begin)
unset(_play_extent_end)
unset(_play_extent_length)
unset(_play_extent_wire)
unset(_play_viewport_directory)
unset(_play_viewport_normal)
unset(_play_viewport_kind)
unset(_play_viewport_marker)
unset(_play_viewport_old)
unset(_play_viewport_mark)
unset(_play_viewport_mutant)
unset(_play_viewport_mutant_path)

unset(_play_viewport_marker_length)
unset(_play_viewport_after_marker)
unset(_play_viewport_remainder)
unset(_play_viewport_second_marker)


# Actual Profiler observation and Memory snapshot interpretation owners.
# These are CPU-only controls; native UI and GPU timing require separate evidence.
jce_add_cxx_test(test_jce_profiler_observation
    SOURCES test_jce_profiler_observation.cpp)
function(_jce_memory_snapshot_fixture name kind)
    jce_add_cxx_test(${name} KIND ${kind}
        SOURCES test_jce_memory_snapshot.cpp
                "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_config.cpp"
        LINK jce_core jce_platform)
endfunction()
_jce_memory_snapshot_fixture(test_jce_memory_snapshot unit)

function(_jce_profiler_memory_control name owner marker replacement fixture header_definition)
    file(READ "${owner}" content)
    string(FIND "${content}" "${marker}" first)
    if(first LESS 0)
        message(FATAL_ERROR "${name}: actual diagnostic owner wire missing")
    endif()
    string(LENGTH "${marker}" marker_size)
    math(EXPR after "${first} + ${marker_size}")
    string(SUBSTRING "${content}" ${after} -1 remainder)
    string(FIND "${remainder}" "${marker}" duplicate)
    if(NOT duplicate LESS 0)
        message(FATAL_ERROR "${name}: diagnostic mutation wire must be unique")
    endif()
    string(REPLACE "${marker}" "${replacement}" mutated "${content}")
    set(directory "${CMAKE_CURRENT_BINARY_DIR}/profiler-memory-mutation")
    file(MAKE_DIRECTORY "${directory}")
    set(header "${directory}/${name}.h")
    if(EXISTS "${header}")
        file(READ "${header}" previous)
    endif()
    if(NOT previous STREQUAL mutated)
        file(WRITE "${header}" "${mutated}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${owner}")
    if(fixture STREQUAL "memory")
        _jce_memory_snapshot_fixture(${name} contract)
    else()
        jce_add_cxx_test(${name} KIND contract SOURCES test_jce_profiler_observation.cpp)
    endif()
    target_compile_definitions(${name} PRIVATE "${header_definition}=\"${header}\"")
    set_tests_properties(${name} PROPERTIES WILL_FAIL TRUE)
endfunction()
set(_profiler_observation_owner "${CMAKE_SOURCE_DIR}/editor/src/panels/jce_profiler_observation.h")
_jce_profiler_memory_control(test_jce_profiler_observation_old_wait_math "${_profiler_observation_owner}"
    "sample.cpu_interval_ms=static_cast<double>(stats->cpu_time_frame)*scale;"
    "sample.cpu_interval_ms=(static_cast<double>(stats->cpu_time_frame)-static_cast<double>(stats->wait_render)-static_cast<double>(stats->wait_submit))*scale;"
    observation JCE_PROFILER_OBSERVATION_HEADER)
set(_memory_snapshot_owner "${CMAKE_SOURCE_DIR}/editor/src/panels/jce_panel_memory_snapshot.h")
_jce_profiler_memory_control(test_jce_memory_snapshot_old_tag_budget "${_memory_snapshot_owner}"
    "return snapshot.process_valid?snapshot.proc.process_rss_bytes:0;"
    "return snapshot.total_cur;"
    memory JCE_EDITOR_MEMORY_SNAPSHOT_HEADER)
_jce_profiler_memory_control(test_jce_memory_snapshot_old_profile_path "${_memory_snapshot_owner}"
    "jce_editor_dotjce_path(name,desired,sizeof desired)"
    "old_profile_path(name,desired,sizeof desired)"
    memory JCE_EDITOR_MEMORY_SNAPSHOT_HEADER)
_jce_profiler_memory_control(test_jce_memory_snapshot_old_nonatomic "${_memory_snapshot_owner}"
    "jce_fs_host_write_all_atomic(published,body,static_cast<uint64_t>(length))"
    "jce_fs_host_write_all(published,body,static_cast<uint64_t>(length))"
    memory JCE_EDITOR_MEMORY_SNAPSHOT_HEADER)
_jce_profiler_memory_control(test_jce_memory_snapshot_old_truncate "${_memory_snapshot_owner}"
    "return JceEditorMemorySnapshotResult::Incomplete;"
    "length=0;"
    memory JCE_EDITOR_MEMORY_SNAPSHOT_HEADER)
# Complete former GPU admission wire: normal assertions reject legacy-only
# availability; the mutation cannot dereference a missing public observation.
set(_profiler_gpu_current [=[    if (gpu && gpu->version==1 && gpu->struct_size>=sizeof(*gpu) &&
        gpu->context_available && gpu->backend_available && gpu->timer_available &&
        gpu->unavailable_reason==JCE_GPU_TIMING_AVAILABLE &&
        std::isfinite(gpu->duration_ms) && gpu->duration_ms>0) {
        sample.gpu_available=true;
        sample.gpu_ms=gpu->duration_ms;
    }]=])
set(_profiler_gpu_old [=[    sample.gpu_available=stats->gpu_timer_freq>0 && stats->gpu_time_begin>=0 &&
        stats->gpu_time_end>0 && stats->gpu_time_end>=stats->gpu_time_begin;
    if (sample.gpu_available)
        sample.gpu_ms=(static_cast<double>(stats->gpu_time_end)-static_cast<double>(stats->gpu_time_begin))*
            1000.0/static_cast<double>(stats->gpu_timer_freq);]=])
_jce_profiler_memory_control(test_jce_profiler_observation_old_gpu_wire "${_profiler_observation_owner}"
    "${_profiler_gpu_current}" "${_profiler_gpu_old}"
    observation JCE_PROFILER_OBSERVATION_HEADER)
unset(_profiler_gpu_current)
unset(_profiler_gpu_old)
unset(_profiler_observation_owner)
unset(_memory_snapshot_owner)


# Actual Bundle Browser file/cached-snapshot owner over real Bundler/Async/FS.
set(_bundle_inspection_owner "${CMAKE_SOURCE_DIR}/editor/src/panels/jce_bundle_inspection_internal.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_bundle_inspection_owner}")
file(READ "${_bundle_inspection_owner}" _bundle_inspection_source)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bundle-inspection")
foreach(_bundle_inspection_variant IN ITEMS normal missing_file_target missing_memory_target)
    set(_bundle_inspection_content "${_bundle_inspection_source}")
    set(_bundle_inspection_kind unit)
    set(_bundle_inspection_target test_jce_editor_bundle_inspection)
    if(NOT _bundle_inspection_variant STREQUAL "normal")
        string(APPEND _bundle_inspection_target "_${_bundle_inspection_variant}")
        set(_bundle_inspection_kind contract)
        if(_bundle_inspection_variant STREQUAL "missing_file_target")
            set(_bundle_inspection_wire "jce_bundle_file_inspect_file(path, target)")
            set(_bundle_inspection_replacement "jce_bundle_file_inspect_file(path, \"web\")")
        else()
            set(_bundle_inspection_wire "jce_bundle_file_inspect_memory(data, size, target)")
            set(_bundle_inspection_replacement "jce_bundle_file_inspect_memory(data, size, \"web\")")
        endif()
        string(FIND "${_bundle_inspection_source}" "${_bundle_inspection_wire}" _bundle_inspection_first)
        string(FIND "${_bundle_inspection_source}" "${_bundle_inspection_wire}" _bundle_inspection_last REVERSE)
        if(_bundle_inspection_first LESS 0 OR NOT _bundle_inspection_first EQUAL _bundle_inspection_last)
            message(FATAL_ERROR "Bundle inspection target control requires one actual public API wire")
        endif()
        string(REPLACE "${_bundle_inspection_wire}" "${_bundle_inspection_replacement}"
            _bundle_inspection_content "${_bundle_inspection_source}")
    endif()
    set(_bundle_inspection_path "${CMAKE_CURRENT_BINARY_DIR}/bundle-inspection/${_bundle_inspection_variant}.h")
    unset(_bundle_inspection_existing)
    if(EXISTS "${_bundle_inspection_path}")
        file(READ "${_bundle_inspection_path}" _bundle_inspection_existing)
    endif()
    if(NOT _bundle_inspection_existing STREQUAL _bundle_inspection_content)
        file(WRITE "${_bundle_inspection_path}" "${_bundle_inspection_content}")
    endif()
    jce_add_cxx_test(${_bundle_inspection_target} KIND ${_bundle_inspection_kind}
        SOURCES test_jce_editor_bundle_inspection.cpp LINK jce_core jce_resource)
    target_compile_features(${_bundle_inspection_target} PRIVATE cxx_std_20)
    target_compile_definitions(${_bundle_inspection_target} PRIVATE
        JCE_TEST_BUNDLE_INSPECTION_OWNER="${_bundle_inspection_path}"
        JCE_TEST_BUNDLE_INSPECTION_PREFIX="${_bundle_inspection_target}")
    if(_bundle_inspection_kind STREQUAL "contract")
        set_tests_properties(${_bundle_inspection_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()

# Actual Scene adaptive policy and exact bounded-getter/consume source wire.
# Latched identity controls are CPU-only; no GPU completion or render proof.
set(_dynres_policy_owner "${CMAKE_SOURCE_DIR}/editor/src/scene/jce_editor_dynres_policy.h")
set(_dynres_scene_owner "${CMAKE_SOURCE_DIR}/editor/src/scene/jce_editor_scene_render.cpp")
file(READ "${_dynres_policy_owner}" _dynres_policy_source)
file(READ "${_dynres_scene_owner}" _dynres_scene_source)
set(_dynres_actual_wire [=[            JceGpuTimingObservation sample{};
            const bool observed = jce_renderer_get_gpu_timing_observation(&sample, sizeof(sample));
            jce_editor_dynres_consume(&s_dynres, observed ? &sample : nullptr);
]=])
string(FIND "${_dynres_scene_source}" "${_dynres_actual_wire}" _dynres_first)
string(FIND "${_dynres_scene_source}" "${_dynres_actual_wire}" _dynres_last REVERSE)
if(_dynres_first LESS 0 OR NOT _dynres_first EQUAL _dynres_last)
    message(FATAL_ERROR "Scene dynres controls require one exact production getter/consume wire")
endif()
string(LENGTH "${_dynres_actual_wire}" _dynres_wire_length)
string(SUBSTRING "${_dynres_scene_source}" ${_dynres_first} ${_dynres_wire_length} _dynres_extracted_wire)
set(_dynres_repeat_guard [=[    if (state->has_sample &&
        state->context_generation == sample->context_generation &&
        state->source_frame == sample->gpu_source_frame &&
        state->raw_begin == sample->raw_begin && state->raw_end == sample->raw_end &&
        state->timer_frequency == sample->timer_frequency) return false;
]=])
string(FIND "${_dynres_policy_source}" "${_dynres_repeat_guard}" _dynres_first)
string(FIND "${_dynres_policy_source}" "${_dynres_repeat_guard}" _dynres_last REVERSE)
if(_dynres_first LESS 0 OR NOT _dynres_first EQUAL _dynres_last)
    message(FATAL_ERROR "Scene dynres stale control requires one exact whole-policy identity guard")
endif()
set(_dynres_getter_wire "            const bool observed = jce_renderer_get_gpu_timing_observation(&sample, sizeof(sample));")
set(_dynres_dir "${CMAKE_CURRENT_BINARY_DIR}/dynres-gpu-timing")
file(MAKE_DIRECTORY "${_dynres_dir}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_dynres_policy_owner}" "${_dynres_scene_owner}")
foreach(_dynres_variant IN ITEMS current old_repeat missing_getter)
    set(_dynres_header "${_dynres_policy_owner}")
    set(_dynres_wire "${_dynres_extracted_wire}")
    set(_dynres_kind unit)
    set(_dynres_target test_jce_editor_dynres_gpu_timing)
    if(_dynres_variant STREQUAL "old_repeat")
        set(_dynres_kind contract)
        set(_dynres_target test_jce_editor_dynres_gpu_timing_old_repeat)
        string(REPLACE "${_dynres_repeat_guard}" "    (void)state->has_sample;
"
            _dynres_header_content "${_dynres_policy_source}")
        set(_dynres_header "${_dynres_dir}/old_repeat.h")
        unset(_dynres_previous)
        if(EXISTS "${_dynres_header}")
            file(READ "${_dynres_header}" _dynres_previous)
        endif()
        if(NOT _dynres_previous STREQUAL _dynres_header_content)
            file(WRITE "${_dynres_header}" "${_dynres_header_content}")
        endif()
    elseif(_dynres_variant STREQUAL "missing_getter")
        set(_dynres_kind contract)
        set(_dynres_target test_jce_editor_dynres_gpu_timing_missing_getter)
        string(REPLACE "${_dynres_getter_wire}" "            const bool observed = false;"
            _dynres_wire "${_dynres_extracted_wire}")
    endif()
    set(_dynres_wire_path "${_dynres_dir}/${_dynres_variant}.inc.h")
    unset(_dynres_previous)
    if(EXISTS "${_dynres_wire_path}")
        file(READ "${_dynres_wire_path}" _dynres_previous)
    endif()
    if(NOT _dynres_previous STREQUAL _dynres_wire)
        file(WRITE "${_dynres_wire_path}" "${_dynres_wire}")
    endif()
    jce_add_cxx_test(${_dynres_target} KIND ${_dynres_kind}
        SOURCES test_jce_editor_dynres_gpu_timing.cpp)
    target_compile_features(${_dynres_target} PRIVATE cxx_std_20)
    target_compile_definitions(${_dynres_target} PRIVATE
        JCE_DYNRES_POLICY_HEADER="${_dynres_header}"
        JCE_DYNRES_WIRE_HEADER="${_dynres_wire_path}")
    if(_dynres_kind STREQUAL "contract")
        set_tests_properties(${_dynres_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
foreach(_dynres_var IN ITEMS policy_owner scene_owner policy_source scene_source actual_wire first last
    wire_length extracted_wire repeat_guard getter_wire dir variant header wire kind target
    header_content previous wire_path)
    unset(_dynres_${_dynres_var})
endforeach()
unset(_dynres_var)

# Benchmark wall observer + uniquely extracted actual panel sampling wire.
set(_bench_owner "${CMAKE_SOURCE_DIR}/editor/src/panels/jce_benchmark_observation.h")
set(_bench_panel "${CMAKE_SOURCE_DIR}/editor/src/panels/jce_panel_benchmark.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_bench_owner}" "${_bench_panel}")
file(READ "${_bench_owner}" _bench_source)
file(READ "${_bench_panel}" _bench_panel_source)
set(_bench_wire [=[    const auto cadence = jce_benchmark_poll(&s_observation.cadence, ImGui::GetFrameCount(), blocked, profile, &gpu);
    if (cadence.accepted) ft_push(static_cast<float>(cadence.ms));
]=])
string(FIND "${_bench_panel_source}" "${_bench_wire}" _bench_first)
string(FIND "${_bench_panel_source}" "${_bench_wire}" _bench_last REVERSE)
if(_bench_first LESS 0 OR NOT _bench_first EQUAL _bench_last)
    message(FATAL_ERROR "Benchmark controls require one exact production sampling wire")
endif()
string(LENGTH "${_bench_wire}" _bench_length)
string(SUBSTRING "${_bench_panel_source}" ${_bench_first} ${_bench_length} _bench_actual_wire)
set(_bench_dir "${CMAKE_CURRENT_BINARY_DIR}/benchmark-observation")
file(MAKE_DIRECTORY "${_bench_dir}")
set(_bench_wire_path "${_bench_dir}/actual.inc.h")
unset(_bench_previous)
if(EXISTS "${_bench_wire_path}")
    file(READ "${_bench_wire_path}" _bench_previous)
endif()
if(NOT _bench_previous STREQUAL _bench_actual_wire)
    file(WRITE "${_bench_wire_path}" "${_bench_actual_wire}")
endif()
foreach(_bench_variant IN ITEMS current missing_wall_clock old_profile)
    set(_bench_header "${_bench_owner}")
    set(_bench_kind unit)
    set(_bench_target test_jce_editor_benchmark_observation)
    if(NOT _bench_variant STREQUAL "current")
        string(APPEND _bench_target "_${_bench_variant}")
        set(_bench_kind contract)
        if(_bench_variant STREQUAL "missing_wall_clock")
            set(_bench_needle "    const uint64_t tick = jce_time_perf_counter();")
            set(_bench_replacement "    const uint64_t tick = 0;")
        else()
            set(_bench_needle "        jce_benchmark_same_profile(value.profile,profile);")
            set(_bench_replacement "        true;")
        endif()
        string(FIND "${_bench_source}" "${_bench_needle}" _bench_first)
        string(FIND "${_bench_source}" "${_bench_needle}" _bench_last REVERSE)
        if(_bench_first LESS 0 OR NOT _bench_first EQUAL _bench_last)
            message(FATAL_ERROR "Benchmark control requires one complete-owner needle")
        endif()
        string(REPLACE "${_bench_needle}" "${_bench_replacement}" _bench_content "${_bench_source}")
        set(_bench_header "${_bench_dir}/${_bench_variant}.h")
        unset(_bench_previous)
        if(EXISTS "${_bench_header}")
            file(READ "${_bench_header}" _bench_previous)
        endif()
        if(NOT _bench_previous STREQUAL _bench_content)
            file(WRITE "${_bench_header}" "${_bench_content}")
        endif()
    endif()
    jce_add_cxx_test(${_bench_target} KIND ${_bench_kind}
        SOURCES test_jce_benchmark_observation.cpp
                "${CMAKE_SOURCE_DIR}/engine/src/application/jce_lifecycle.c"
        LINK jce_core jce_platform)
    target_compile_features(${_bench_target} PRIVATE cxx_std_20)
    target_compile_definitions(${_bench_target} PRIVATE
        JCE_BENCHMARK_OBSERVATION_HEADER="${_bench_header}"
        JCE_BENCHMARK_WIRE_HEADER="${_bench_wire_path}")
    if(_bench_kind STREQUAL "contract")
        set_tests_properties(${_bench_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
foreach(_bench_var IN ITEMS owner panel source panel_source wire first last length actual_wire dir wire_path previous variant header kind target needle replacement content)
    unset(_bench_${_bench_var})
endforeach()
unset(_bench_var)

# Actual Play mixer descriptor selection. Only proven metadata absence may
# choose legacy/default paths; UNKNOWN retains the original attempted read.
set(_play_mixer_owner "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_play.cpp")
file(READ "${_play_mixer_owner}" _play_mixer_source)
set(_play_mixer_start_marker "    /* Seed the runtime mixer from the same audio_mixer.json")
set(_play_mixer_end_marker "    /* Navmesh: hand the runtime the .navmesh.bin baked by the NavMesh panel")
foreach(_play_mixer_marker IN ITEMS start end)
    string(FIND "${_play_mixer_source}" "${_play_mixer_${_play_mixer_marker}_marker}" _play_mixer_first)
    string(FIND "${_play_mixer_source}" "${_play_mixer_${_play_mixer_marker}_marker}" _play_mixer_last REVERSE)
    if(_play_mixer_first LESS 0 OR NOT _play_mixer_first EQUAL _play_mixer_last)
        message(FATAL_ERROR "Play mixer controls require one actual segment ${_play_mixer_marker} marker")
    endif()
    set(_play_mixer_${_play_mixer_marker} "${_play_mixer_first}")
endforeach()
if(NOT _play_mixer_end GREATER _play_mixer_start)
    message(FATAL_ERROR "Play mixer segment markers are reversed")
endif()
math(EXPR _play_mixer_length "${_play_mixer_end} - ${_play_mixer_start}")
string(SUBSTRING "${_play_mixer_source}" ${_play_mixer_start} ${_play_mixer_length} _play_mixer_actual)
set(_play_mixer_needle "            jce_fs_probe_read(s_mixer_cfg) != JCE_FS_PRESENCE_ABSENT)")
string(FIND "${_play_mixer_actual}" "${_play_mixer_needle}" _play_mixer_first)
string(FIND "${_play_mixer_actual}" "${_play_mixer_needle}" _play_mixer_last REVERSE)
if(_play_mixer_first LESS 0 OR NOT _play_mixer_first EQUAL _play_mixer_last)
    message(FATAL_ERROR "Play mixer control requires one actual fallback presence guard")
endif()
set(_play_mixer_dir "${CMAKE_CURRENT_BINARY_DIR}/play-mixer")
file(MAKE_DIRECTORY "${_play_mixer_dir}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_play_mixer_owner}")
foreach(_play_mixer_variant IN ITEMS current missing_fallback_check)
    set(_play_mixer_target test_jce_editor_play_mixer)
    set(_play_mixer_kind unit)
    set(_play_mixer_content "${_play_mixer_actual}")
    if(NOT _play_mixer_variant STREQUAL "current")
        string(APPEND _play_mixer_target "_${_play_mixer_variant}")
        set(_play_mixer_kind contract)
        string(REPLACE "${_play_mixer_needle}" "            true) /* Former missing fallback presence check. */"
            _play_mixer_content "${_play_mixer_actual}")
    endif()
    set(_play_mixer_wire "${_play_mixer_dir}/${_play_mixer_variant}.inc.h")
    unset(_play_mixer_previous)
    if(EXISTS "${_play_mixer_wire}")
        file(READ "${_play_mixer_wire}" _play_mixer_previous)
    endif()
    if(NOT _play_mixer_previous STREQUAL _play_mixer_content)
        file(WRITE "${_play_mixer_wire}" "${_play_mixer_content}")
    endif()
    jce_add_cxx_test(${_play_mixer_target} KIND ${_play_mixer_kind}
        SOURCES test_jce_editor_play_mixer.cpp)
    target_compile_definitions(${_play_mixer_target} PRIVATE
        JCE_EDITOR_PLAY_MIXER_WIRE="${_play_mixer_wire}")
    if(_play_mixer_kind STREQUAL "contract")
        set_tests_properties(${_play_mixer_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
foreach(_play_mixer_var IN ITEMS owner source start_marker end_marker marker first last start end length actual needle dir variant target kind content wire previous)
    unset(_play_mixer_${_play_mixer_var})
endforeach()
unset(_play_mixer_var)

include("${CMAKE_CURRENT_LIST_DIR}/profiler_chart_controls.cmake")

# Complete actual shared DockBuilder routes, ten presets and their dispatcher.
# Typed CPU ports inspect final stable-ID destinations; no ImGui/GPU is created.
set(_layout_routes_owner "${CMAKE_SOURCE_DIR}/editor/src/ui/jce_editor_layout.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_layout_routes_owner}")
file(READ "${_layout_routes_owner}" _layout_routes_source)
set(_layout_routes_start_marker "static void dock_extension_panels(ImGuiID left_id,")
set(_layout_routes_end_marker "\n/* Buddy table for the first-open dock fallback below:")
foreach(_layout_routes_boundary IN ITEMS start end)
    string(FIND "${_layout_routes_source}" "${_layout_routes_${_layout_routes_boundary}_marker}"
        _layout_routes_first)
    string(FIND "${_layout_routes_source}" "${_layout_routes_${_layout_routes_boundary}_marker}"
        _layout_routes_last REVERSE)
    if(_layout_routes_first LESS 0 OR NOT _layout_routes_first EQUAL _layout_routes_last)
        message(FATAL_ERROR "Layout route actual ${_layout_routes_boundary} marker must be unique")
    endif()
    set(_layout_routes_${_layout_routes_boundary} ${_layout_routes_first})
endforeach()
if(_layout_routes_end LESS_EQUAL _layout_routes_start)
    message(FATAL_ERROR "Layout route actual-body bounds are invalid")
endif()
math(EXPR _layout_routes_length "${_layout_routes_end} - ${_layout_routes_start}")
string(SUBSTRING "${_layout_routes_source}" ${_layout_routes_start} ${_layout_routes_length}
    _layout_routes_actual)

# Restore only Profiling's old late fallback call, preserving complete bodies
# and all assertions. Removal/insertion needles must each occur exactly once.
set(_layout_routes_early
    "    /* Apply shared fallback routes before this preset's explicit cluster. */\n    dock_extension_panels(left_id, right_id, right_id, bottom_id);\n\n")
set(_layout_routes_finish
    "    ImGui::DockBuilderDockWindow(\"Timeline###timeline\",              bottom_id);\n    ImGui::DockBuilderFinish(dockspace_id);")
foreach(_layout_routes_needle IN ITEMS early finish)
    string(FIND "${_layout_routes_actual}" "${_layout_routes_${_layout_routes_needle}}"
        _layout_routes_first)
    string(FIND "${_layout_routes_actual}" "${_layout_routes_${_layout_routes_needle}}"
        _layout_routes_last REVERSE)
    if(_layout_routes_first LESS 0 OR NOT _layout_routes_first EQUAL _layout_routes_last)
        message(FATAL_ERROR "Layout route ${_layout_routes_needle} mutation wire must be unique")
    endif()
endforeach()
string(REPLACE "${_layout_routes_early}" "" _layout_routes_old "${_layout_routes_actual}")
string(REPLACE "${_layout_routes_finish}"
    "    ImGui::DockBuilderDockWindow(\"Timeline###timeline\",              bottom_id);\n    dock_extension_panels(left_id, right_id, right_id, bottom_id);\n    ImGui::DockBuilderFinish(dockspace_id);"
    _layout_routes_old "${_layout_routes_old}")
if(_layout_routes_old STREQUAL _layout_routes_actual)
    message(FATAL_ERROR "Layout route old-routing mutation did not change its owner")
endif()

set(_layout_routes_dir "${CMAKE_CURRENT_BINARY_DIR}/layout-routes")
file(MAKE_DIRECTORY "${_layout_routes_dir}")
foreach(_layout_routes_variant IN ITEMS actual old)
    set(_layout_routes_content "${_layout_routes_${_layout_routes_variant}}")
    set(_layout_routes_path "${_layout_routes_dir}/${_layout_routes_variant}.inc.h")
    unset(_layout_routes_previous)
    if(EXISTS "${_layout_routes_path}")
        file(READ "${_layout_routes_path}" _layout_routes_previous)
    endif()
    if(NOT _layout_routes_previous STREQUAL _layout_routes_content)
        file(WRITE "${_layout_routes_path}" "${_layout_routes_content}")
    endif()
    if(_layout_routes_variant STREQUAL "actual")
        set(_layout_routes_target test_jce_editor_layout_routes)
        set(_layout_routes_kind integration)
    else()
        set(_layout_routes_target test_jce_editor_layout_routes_old_routing)
        set(_layout_routes_kind contract)
    endif()
    jce_add_cxx_test(${_layout_routes_target} KIND ${_layout_routes_kind}
        SOURCES test_jce_editor_layout_routes.cpp)
    target_compile_features(${_layout_routes_target} PRIVATE cxx_std_20)
    target_compile_definitions(${_layout_routes_target} PRIVATE
        JCE_EDITOR_LAYOUT_ROUTES_OWNER="${_layout_routes_path}")
    if(_layout_routes_kind STREQUAL "contract")
        set_tests_properties(${_layout_routes_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
foreach(_layout_routes_var IN ITEMS owner source start_marker end_marker boundary first last start end length actual early finish needle old dir variant content path previous target kind)
    unset(_layout_routes_${_layout_routes_var})
endforeach()
unset(_layout_routes_var)

include("${CMAKE_CURRENT_SOURCE_DIR}/viewport_maximized_controls.cmake")

# Complete private Analyzer CSV producer and actual ring/button/status wires.
# Real profile resolver and host IO remain linked; only faults/UI are ports.
set(_analyzer_export_owner "${CMAKE_SOURCE_DIR}/editor/src/panels/jce_panel_profile_analyzer.h")
set(_analyzer_export_panel "${CMAKE_SOURCE_DIR}/editor/src/panels/jce_panel_profile_analyzer.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_analyzer_export_owner}" "${_analyzer_export_panel}")
file(READ "${_analyzer_export_owner}" _analyzer_export_header)
file(READ "${_analyzer_export_panel}" _analyzer_export_source)
function(_jce_analyzer_export_extract output start_marker end_marker)
    string(FIND "${_analyzer_export_source}" "${start_marker}" first)
    string(FIND "${_analyzer_export_source}" "${start_marker}" last REVERSE)
    string(FIND "${_analyzer_export_source}" "${end_marker}" end)
    string(FIND "${_analyzer_export_source}" "${end_marker}" end_last REVERSE)
    if(first LESS 0 OR end LESS 0 OR NOT first EQUAL last OR NOT end EQUAL end_last OR end LESS_EQUAL first)
        message(FATAL_ERROR "Analyzer export actual slice markers must be unique: ${output}")
    endif()
    math(EXPR length "${end} - ${first}")
    string(SUBSTRING "${_analyzer_export_source}" ${first} ${length} content)
    set(${output} "${content}" PARENT_SCOPE)
endfunction()
_jce_analyzer_export_extract(_analyzer_export_ring
    "constexpr int   kBufLen" "/* Thin bridge for ImGui PlotLines")
_jce_analyzer_export_extract(_analyzer_export_request
    "void export_csv(void)" "void draw_stats_summary(void)")
_jce_analyzer_export_extract(_analyzer_export_toolbar
    "void draw_toolbar(void)" "\n} /* namespace */")
set(_analyzer_export_wire
    "namespace analyzer_actual {\n${_analyzer_export_ring}\n${_analyzer_export_request}\n${_analyzer_export_toolbar}\n}\n")
set(_analyzer_export_dir "${CMAKE_CURRENT_BINARY_DIR}/analyzer-export")
file(MAKE_DIRECTORY "${_analyzer_export_dir}")
foreach(_analyzer_export_variant IN ITEMS current old_profile old_nonatomic old_partial missing_button)
    set(_analyzer_export_content "${_analyzer_export_header}")
    set(_analyzer_export_body "${_analyzer_export_wire}")
    set(_analyzer_export_mutation_var _analyzer_export_content)
    if(_analyzer_export_variant STREQUAL "old_profile")
        set(_analyzer_export_needle "jce_editor_dotjce_path(")
        set(_analyzer_export_replacement "old_profile_path(")
    elseif(_analyzer_export_variant STREQUAL "old_nonatomic")
        set(_analyzer_export_needle "jce_fs_host_write_all_atomic(")
        set(_analyzer_export_replacement "jce_fs_host_write_all(")
    elseif(_analyzer_export_variant STREQUAL "old_partial")
        set(_analyzer_export_needle "        ED_FREE(body);\n        return Result::Incomplete;")
        set(_analyzer_export_replacement "        length = 1;")
    elseif(_analyzer_export_variant STREQUAL "missing_button")
        set(_analyzer_export_mutation_var _analyzer_export_body)
        set(_analyzer_export_needle "        export_csv();")
        set(_analyzer_export_replacement "        (void)0;")
    endif()
    if(NOT _analyzer_export_variant STREQUAL "current")
        string(FIND "${${_analyzer_export_mutation_var}}" "${_analyzer_export_needle}" _analyzer_export_first)
        string(FIND "${${_analyzer_export_mutation_var}}" "${_analyzer_export_needle}" _analyzer_export_last REVERSE)
        if(_analyzer_export_first LESS 0 OR NOT _analyzer_export_first EQUAL _analyzer_export_last)
            message(FATAL_ERROR "Analyzer export ${_analyzer_export_variant} actual wire must be unique")
        endif()
        string(REPLACE "${_analyzer_export_needle}" "${_analyzer_export_replacement}"
            ${_analyzer_export_mutation_var} "${${_analyzer_export_mutation_var}}")
    endif()
    foreach(_analyzer_export_part IN ITEMS content body)
        set(_analyzer_export_path "${_analyzer_export_dir}/${_analyzer_export_variant}_${_analyzer_export_part}.inc.h")
        unset(_analyzer_export_previous)
        if(EXISTS "${_analyzer_export_path}")
            file(READ "${_analyzer_export_path}" _analyzer_export_previous)
        endif()
        if(NOT _analyzer_export_previous STREQUAL "${_analyzer_export_${_analyzer_export_part}}")
            file(WRITE "${_analyzer_export_path}" "${_analyzer_export_${_analyzer_export_part}}")
        endif()
        set(_analyzer_export_${_analyzer_export_part}_path "${_analyzer_export_path}")
    endforeach()
    if(_analyzer_export_variant STREQUAL "current")
        set(_analyzer_export_target test_jce_profile_analyzer_export)
        set(_analyzer_export_kind integration)
    else()
        set(_analyzer_export_target "test_jce_profile_analyzer_export_${_analyzer_export_variant}")
        set(_analyzer_export_kind contract)
    endif()
    jce_add_cxx_test(${_analyzer_export_target} KIND ${_analyzer_export_kind}
        SOURCES test_jce_profile_analyzer_export.cpp
                "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_config.cpp"
        LINK jce_core jce_platform)
    target_compile_features(${_analyzer_export_target} PRIVATE cxx_std_20)
    target_compile_definitions(${_analyzer_export_target} PRIVATE
        JCE_EDITOR_ANALYZER_EXPORT_HEADER="${_analyzer_export_content_path}"
        JCE_EDITOR_ANALYZER_ACTUAL_WIRE="${_analyzer_export_body_path}")
    if(_analyzer_export_kind STREQUAL "contract")
        set_tests_properties(${_analyzer_export_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
foreach(_analyzer_export_var IN ITEMS owner panel header source ring request toolbar wire dir variant content body mutation_var needle replacement first last part path previous content_path body_path target kind)
    unset(_analyzer_export_${_analyzer_export_var})
endforeach()
unset(_analyzer_export_var)

# Actual trace display qualification: complete model/poll/clear plus UI functions.
# The unchanged fixture uses the real core trace producer and typed UI text ports.
set(_trace_display_owner "${CMAKE_SOURCE_DIR}/editor/src/panels/jce_panel_profiler_trace.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_trace_display_owner}")
file(READ "${_trace_display_owner}" _trace_display_source)
set(_trace_display_model_start_marker "namespace {\n")
set(_trace_display_model_end_marker "\nvoid export_trace(void)")
set(_trace_display_render_start_marker "void draw_summary(const JceTraceStats &stats)")
set(_trace_display_render_end_marker "\n} /* anonymous namespace */")
set(_trace_display_toolbar_start_marker "void draw_toolbar(const JceTraceStats &stats)")
set(_trace_display_toolbar_end_marker "\nvoid draw_summary(const JceTraceStats &stats)")
foreach(_trace_display_part IN ITEMS model render toolbar)
    foreach(_trace_display_boundary IN ITEMS start end)
        set(_trace_display_marker "${_trace_display_${_trace_display_part}_${_trace_display_boundary}_marker}")
        string(FIND "${_trace_display_source}" "${_trace_display_marker}" _trace_display_first)
        string(FIND "${_trace_display_source}" "${_trace_display_marker}" _trace_display_last REVERSE)
        if(_trace_display_first LESS 0 OR NOT _trace_display_first EQUAL _trace_display_last)
            message(FATAL_ERROR "Trace display ${_trace_display_part}/${_trace_display_boundary} owner boundary must be unique")
        endif()
        set(_trace_display_${_trace_display_part}_${_trace_display_boundary} ${_trace_display_first})
    endforeach()
    if(_trace_display_${_trace_display_part}_end LESS_EQUAL _trace_display_${_trace_display_part}_start)
        message(FATAL_ERROR "Trace display ${_trace_display_part} actual-body bounds are invalid")
    endif()
    math(EXPR _trace_display_length
        "${_trace_display_${_trace_display_part}_end} - ${_trace_display_${_trace_display_part}_start}")
    string(SUBSTRING "${_trace_display_source}" ${_trace_display_${_trace_display_part}_start}
        ${_trace_display_length} _trace_display_${_trace_display_part}_actual)
endforeach()
# Import the complete actual public panel entry, including passive polling.
set(_trace_display_entry_marker "extern \"C\" void jce_editor_panel_profiler_trace_content(void)")
string(FIND "${_trace_display_source}" "${_trace_display_entry_marker}" _trace_display_first)
string(FIND "${_trace_display_source}" "${_trace_display_entry_marker}" _trace_display_last REVERSE)
if(_trace_display_first LESS 0 OR NOT _trace_display_first EQUAL _trace_display_last)
    message(FATAL_ERROR "Trace display actual content entry must be unique")
endif()
string(SUBSTRING "${_trace_display_source}" ${_trace_display_first} -1 _trace_display_entry_actual)
# Close only the original namespace opened by the actual model prefix.
string(APPEND _trace_display_model_actual "\n} // namespace\n")
set(_trace_display_model_old_state "${_trace_display_model_actual}")
set(_trace_display_model_old_gap "${_trace_display_model_actual}")
set(_trace_display_model_old_clear_unsigned_wrap "${_trace_display_model_actual}")
set(_trace_display_model_old_unbounded "${_trace_display_model_actual}")
set(_trace_display_render_old_memory "${_trace_display_render_actual}")
set(_trace_display_toolbar_old_recording "${_trace_display_toolbar_actual}")
set(_trace_display_state_unknown "return tr(\"profiler.trace.registrationUnknown\", \"Unknown\");")
set(_trace_display_state_unregistered "return tr(\"profiler.trace.unregistrationObserved\", \"Unregistered\");")
set(_trace_display_gap "        if (s_trace.cursor.lost_events != previous_loss)\n            invalidate_registration_prefix();")
set(_trace_display_memory "if (counter_unavailable(entry.first, entry.second))")
set(_trace_display_clear "        stats.recorded_events >= UINT32_MAX ? UINT32_MAX\n            : static_cast<uint32_t>(stats.recorded_events) + 1u;")
set(_trace_display_recording "    ImGui::SameLine();\n    ImGui::TextUnformatted(stats.enabled\n        ? jce_editor_i18n_or(\"profiler.trace.recording\", \"Recording: on\")\n        : jce_editor_i18n_or(\"profiler.trace.recordingOff\", \"Recording: off\"));\n    if (!stats.enabled)\n        ImGui::TextWrapped(\"%s\", jce_editor_i18n_or(\"profiler.trace.retainedHint\",\n            \"Showing retained history; auto refresh does not enable recording.\"));")
foreach(_trace_display_wire IN ITEMS state_unknown state_unregistered gap memory recording clear)
    if(_trace_display_wire STREQUAL "memory")
        set(_trace_display_content "${_trace_display_render_actual}")
    elseif(_trace_display_wire STREQUAL "recording")
        set(_trace_display_content "${_trace_display_toolbar_actual}")
    else()
        set(_trace_display_content "${_trace_display_model_actual}")
    endif()
    string(FIND "${_trace_display_content}" "${_trace_display_${_trace_display_wire}}" _trace_display_first)
    string(FIND "${_trace_display_content}" "${_trace_display_${_trace_display_wire}}" _trace_display_last REVERSE)
    if(_trace_display_first LESS 0 OR NOT _trace_display_first EQUAL _trace_display_last)
        message(FATAL_ERROR "Trace display ${_trace_display_wire} mutation wire must be unique")
    endif()
endforeach()
foreach(_trace_display_wire IN ITEMS state_unknown state_unregistered)
    string(REPLACE "${_trace_display_${_trace_display_wire}}"
        "return tr(\"profiler.trace.exited\", \"Exited\");"
        _trace_display_model_old_state "${_trace_display_model_old_state}")
endforeach()
string(REPLACE "${_trace_display_gap}"
    "        (void)previous_loss; /* old consumer retains stale registration and open spans */"
    _trace_display_model_old_gap "${_trace_display_model_old_gap}")
string(REPLACE "${_trace_display_memory}" "if (false) /* old raw-memory display */"
    _trace_display_render_old_memory "${_trace_display_render_old_memory}")
string(REPLACE "${_trace_display_recording}"
    "    if (stats.enabled) {\n        ImGui::SameLine();\n        ImGui::TextDisabled(\"%s\", tr(\"profiler.trace.recording\", \"recording\"));\n    }"
    _trace_display_toolbar_old_recording "${_trace_display_toolbar_old_recording}")
string(REPLACE "${_trace_display_clear}" "        static_cast<uint32_t>(stats.recorded_events) + 1u;"
    _trace_display_model_old_clear_unsigned_wrap "${_trace_display_model_old_clear_unsigned_wrap}")
# Restore only the three original unbounded admissions. Existing-value and
# accounting behavior stays current, so the control fails ordinary cap assertions.
foreach(_trace_display_cap IN ITEMS
    "if (s_trace.threads.size() >= kMaxObservedThreads) {"
    "if (s_trace.counters.size() >= kMaxCounterNames) {"
    "if (s_trace.active_spans.size() >= kMaxActiveSpans) {")
    string(FIND "${_trace_display_model_old_unbounded}" "${_trace_display_cap}" _trace_display_first)
    string(FIND "${_trace_display_model_old_unbounded}" "${_trace_display_cap}" _trace_display_last REVERSE)
    if(_trace_display_first LESS 0 OR NOT _trace_display_first EQUAL _trace_display_last)
        message(FATAL_ERROR "Trace cache control requires one actual admission guard")
    endif()
    string(REPLACE "${_trace_display_cap}" "if (false) { /* old unbounded admission */"
        _trace_display_model_old_unbounded "${_trace_display_model_old_unbounded}")
endforeach()
unset(_trace_display_cap)
set(_trace_display_dir "${CMAKE_CURRENT_BINARY_DIR}/trace-display")
file(MAKE_DIRECTORY "${_trace_display_dir}")
foreach(_trace_display_variant IN ITEMS actual old_state old_gap old_memory old_recording old_clear_unsigned_wrap old_unbounded
    old_covered_gate old_missing_poll)
    set(_trace_display_model "${_trace_display_model_actual}")
    set(_trace_display_render "${_trace_display_render_actual}")
    set(_trace_display_toolbar "${_trace_display_toolbar_actual}")
    set(_trace_display_entry "${_trace_display_entry_actual}")
    if(_trace_display_variant STREQUAL "old_state" OR _trace_display_variant STREQUAL "old_gap" OR _trace_display_variant STREQUAL "old_clear_unsigned_wrap" OR _trace_display_variant STREQUAL "old_unbounded")
        set(_trace_display_model "${_trace_display_model_${_trace_display_variant}}")
    elseif(_trace_display_variant STREQUAL "old_memory")
        set(_trace_display_render "${_trace_display_render_old_memory}")
    elseif(_trace_display_variant STREQUAL "old_recording")
        set(_trace_display_toolbar "${_trace_display_toolbar_old_recording}")
    elseif(_trace_display_variant STREQUAL "old_covered_gate")
        set(_trace_display_entry_wire "    if (jce_editor_game_view_maximized() &&\n        ImGui::GetWindowViewport() == ImGui::GetMainViewport()) return;\n")
    elseif(_trace_display_variant STREQUAL "old_missing_poll")
        set(_trace_display_entry_wire "    poll_trace();\n")
    endif()
    if(_trace_display_variant STREQUAL "old_covered_gate" OR _trace_display_variant STREQUAL "old_missing_poll")
        string(FIND "${_trace_display_entry}" "${_trace_display_entry_wire}" _trace_display_first)
        string(FIND "${_trace_display_entry}" "${_trace_display_entry_wire}" _trace_display_last REVERSE)
        if(_trace_display_first LESS 0 OR NOT _trace_display_first EQUAL _trace_display_last)
            message(FATAL_ERROR "Trace content ${_trace_display_variant} actual wire must be unique")
        endif()
        string(REPLACE "${_trace_display_entry_wire}" "" _trace_display_entry "${_trace_display_entry}")
    endif()
    foreach(_trace_display_part IN ITEMS model render toolbar entry)
        set(_trace_display_content "${_trace_display_${_trace_display_part}}")
        set(_trace_display_path "${_trace_display_dir}/${_trace_display_variant}_${_trace_display_part}.inc.h")
        unset(_trace_display_previous)
        if(EXISTS "${_trace_display_path}")
            file(READ "${_trace_display_path}" _trace_display_previous)
        endif()
        if(NOT _trace_display_previous STREQUAL _trace_display_content)
            file(WRITE "${_trace_display_path}" "${_trace_display_content}")
        endif()
    endforeach()
    if(_trace_display_variant STREQUAL "actual")
        set(_trace_display_target test_jce_profiler_trace_display)
        set(_trace_display_kind integration)
    else()
        set(_trace_display_target test_jce_profiler_trace_display_${_trace_display_variant})
        set(_trace_display_kind contract)
    endif()
    jce_add_cxx_test(${_trace_display_target} KIND ${_trace_display_kind}
        SOURCES test_jce_profiler_trace_display.cpp LINK jce_core)
    target_compile_features(${_trace_display_target} PRIVATE cxx_std_20)
    target_compile_definitions(${_trace_display_target} PRIVATE
        JCE_TRACE_DISPLAY_MODEL_OWNER="${_trace_display_dir}/${_trace_display_variant}_model.inc.h"
        JCE_TRACE_DISPLAY_RENDER_OWNER="${_trace_display_dir}/${_trace_display_variant}_render.inc.h"
        JCE_TRACE_DISPLAY_TOOLBAR_OWNER="${_trace_display_dir}/${_trace_display_variant}_toolbar.inc.h"
        JCE_TRACE_DISPLAY_ENTRY_OWNER="${_trace_display_dir}/${_trace_display_variant}_entry.inc.h")
    if(_trace_display_kind STREQUAL "contract")
        set_tests_properties(${_trace_display_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
foreach(_trace_display_var IN ITEMS owner source model_start_marker model_end_marker render_start_marker render_end_marker toolbar_start_marker toolbar_end_marker part boundary marker first last model_start model_end render_start render_end toolbar_start toolbar_end length model_actual render_actual toolbar_actual model_old_state model_old_gap model_old_clear_unsigned_wrap model_old_unbounded render_old_memory toolbar_old_recording state_unknown state_unregistered gap memory recording clear wire content dir variant model render toolbar path previous target kind entry_marker entry_actual entry entry_wire)
    unset(_trace_display_${_trace_display_var})
endforeach()
unset(_trace_display_var)

# Actual GameRender camera state, lifecycle and render consumer bodies.
# Cameras/matrices are real public CPU owners; GPU/runtime/history ports are counted.
set(_game_camera_owner "${CMAKE_SOURCE_DIR}/editor/src/scene/jce_editor_game_render.cpp")
set(_game_camera_play_owner "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_play.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_game_camera_owner}" "${_game_camera_play_owner}")
file(READ "${_game_camera_owner}" _game_camera_source)
file(READ "${_game_camera_play_owner}" _game_camera_play_source)
function(_jce_game_camera_unique source needle output)
    string(FIND "${source}" "${needle}" first)
    string(FIND "${source}" "${needle}" last REVERSE)
    if(first LESS 0 OR NOT first EQUAL last)
        message(FATAL_ERROR "Game camera actual wire must be unique: ${needle}")
    endif()
    set(${output} ${first} PARENT_SCOPE)
endfunction()
function(_jce_game_camera_extract source start_marker end_marker output)
    _jce_game_camera_unique("${source}" "${start_marker}" first)
    _jce_game_camera_unique("${source}" "${end_marker}" end)
    if(end LESS_EQUAL first)
        message(FATAL_ERROR "Game camera actual-body bounds are invalid")
    endif()
    math(EXPR length "${end} - ${first}")
    string(SUBSTRING "${source}" ${first} ${length} content)
    set(${output} "${content}" PARENT_SCOPE)
endfunction()
_jce_game_camera_extract("${_game_camera_source}" "struct GameRenderState {"
    "/* Use a view ID well above" _game_camera_state)
set(_game_camera_base "constexpr uint16_t GAME_VIEW_BASE = JCE_EDITOR_GAME_VIEW_BASE;\n")
_jce_game_camera_unique("${_game_camera_source}" "${_game_camera_base}" _game_camera_base_pos)
string(APPEND _game_camera_state "${_game_camera_base}")
_jce_game_camera_extract("${_game_camera_source}" "JceCameraDesc game_default_camera_desc_internal(void)"
    "} /* anon namespace */" _game_camera_helpers)
_jce_game_camera_extract("${_game_camera_source}" "bool jce_editor_game_render_init("
    "    LOG_INFO(LOG_TAG, \"[init] game view renderer ready" _game_camera_init)
string(REPLACE "bool jce_editor_game_render_init(" "bool test_actual_game_camera_init("
    _game_camera_init "${_game_camera_init}")
string(APPEND _game_camera_init "    return true;\n}\n")
_jce_game_camera_extract("${_game_camera_source}" "void jce_editor_game_render_shutdown(void)"
    "/* Captured mouse-button bitmask" _game_camera_shutdown)
_jce_game_camera_extract("${_game_camera_source}" "bool jce_editor_game_render_begin_play_camera(void)"
    "/* Hand the game renderer the live Play-mode runtime" _game_camera_lifecycle)
_jce_game_camera_extract("${_game_camera_source}" "void jce_editor_game_render_set_play_runtime(JceRuntime *rt)"
    "bool jce_editor_game_render_apply_cursor(void)" _game_camera_attach)
_jce_game_camera_extract("${_game_camera_source}" "JceCamera *jce_editor_game_render_get_camera(void)"
    "uint16_t jce_editor_game_render_get_texture(void)" _game_camera_get)
_jce_game_camera_extract("${_game_camera_source}" "    JceSceneRenderer *engine_sr = jce_editor_get_scene_renderer();"
    "    /* Camera.clearFlags, resolved ONCE here" _game_camera_frame)
_jce_game_camera_extract("${_game_camera_source}"
    "    JceEditorViewportRenderTrace render_trace(\"editor.game.render\", \"editor.game.render.reason\","
    "    if (!g.initialized || !g.renderer)" _game_camera_frame_trace)
set(_game_camera_actual "${_game_camera_state}\n${_game_camera_helpers}\n${_game_camera_lifecycle}\n${_game_camera_attach}\n${_game_camera_get}\n${_game_camera_init}\n${_game_camera_shutdown}\nvoid test_actual_game_camera_frame(uint32_t width,uint32_t height)\n{\n${_game_camera_frame_trace}${_game_camera_frame}\n}\n")
_jce_game_camera_extract("${_game_camera_play_source}" "    const bool created = jce_editor_play_create(s_play_snapshot_valid,"
    "    jce_runtime_set_platform_services(s_play_host.runtime_stats.runtime," _game_camera_create)
_jce_game_camera_extract("${_game_camera_play_source}" "        jce_editor_game_render_set_play_runtime(NULL);"
    "        /* Unload streamed chunks" _game_camera_stop)
set(_game_camera_retry "    if (s.play_state == JCE_PLAY_STOPPED) {\n        jce_editor_game_render_end_play_camera();\n")
_jce_game_camera_unique("${_game_camera_play_source}" "${_game_camera_retry}" _game_camera_retry_pos)
string(APPEND _game_camera_retry "    }\n")
set(_game_camera_taa "        jce_postfx_set_taa(g.postfx, true,\n            g.camera_cut_pending ? 0.0f : taa_fb, taa_lc, taa_mc);\n")
set(_game_camera_apply "                    jce_postfx_apply(g.postfx, game_color, game_depth);\n                    g.camera_cut_pending = false;\n")
_jce_game_camera_unique("${_game_camera_source}" "${_game_camera_taa}" _game_camera_taa_pos)
_jce_game_camera_unique("${_game_camera_source}" "${_game_camera_apply}" _game_camera_apply_pos)
set(_game_camera_dir "${CMAKE_CURRENT_BINARY_DIR}/game-camera")
file(MAKE_DIRECTORY "${_game_camera_dir}")
foreach(_game_camera_variant IN ITEMS current old_selection missing_release missing_admission missing_cut old_default_seed)
    set(_game_camera_content "${_game_camera_actual}")
    set(_game_camera_create_content "${_game_camera_create}")
    set(_game_camera_taa_content "${_game_camera_taa}")
    if(_game_camera_variant STREQUAL "old_selection")
        set(_game_camera_needle "    return g.play_camera ? g.play_camera : g.camera;")
        _jce_game_camera_unique("${_game_camera_content}" "${_game_camera_needle}" _game_camera_first)
        string(REPLACE "${_game_camera_needle}" "    return g.camera;"
            _game_camera_content "${_game_camera_content}")
    elseif(_game_camera_variant STREQUAL "missing_release")
        set(_game_camera_needle "    if (camera) jce_camera_destroy(camera);")
        _jce_game_camera_unique("${_game_camera_content}" "${_game_camera_needle}" _game_camera_first)
        string(REPLACE "${_game_camera_needle}" "    (void)camera;"
            _game_camera_content "${_game_camera_content}")
    elseif(_game_camera_variant STREQUAL "missing_admission")
        set(_game_camera_needle "            if (!jce_editor_game_render_begin_play_camera()) return false;\n")
        _jce_game_camera_unique("${_game_camera_create_content}" "${_game_camera_needle}" _game_camera_first)
        string(REPLACE "${_game_camera_needle}" "" _game_camera_create_content "${_game_camera_create_content}")
    elseif(_game_camera_variant STREQUAL "missing_cut")
        set(_game_camera_needle "g.camera_cut_pending ? 0.0f : taa_fb")
        _jce_game_camera_unique("${_game_camera_taa_content}" "${_game_camera_needle}" _game_camera_first)
        string(REPLACE "${_game_camera_needle}" "taa_fb" _game_camera_taa_content "${_game_camera_taa_content}")
    elseif(_game_camera_variant STREQUAL "old_default_seed")
        set(_game_camera_needle "    JceCamera *camera = g.camera ? jce_camera_clone(g.camera)\n                                 : jce_camera_create(&desc);")
        _jce_game_camera_unique("${_game_camera_content}" "${_game_camera_needle}" _game_camera_first)
        string(REPLACE "${_game_camera_needle}" "    JceCamera *camera = jce_camera_create(&desc);"
            _game_camera_content "${_game_camera_content}")
    endif()
    foreach(_game_camera_part IN ITEMS content create_content stop retry taa_content apply)
        set(_game_camera_path "${_game_camera_dir}/${_game_camera_variant}_${_game_camera_part}.inc.h")
        unset(_game_camera_previous)
        if(EXISTS "${_game_camera_path}")
            file(READ "${_game_camera_path}" _game_camera_previous)
        endif()
        if(NOT _game_camera_previous STREQUAL _game_camera_${_game_camera_part})
            file(WRITE "${_game_camera_path}" "${_game_camera_${_game_camera_part}}")
        endif()
    endforeach()
    if(_game_camera_variant STREQUAL "current")
        set(_game_camera_target test_jce_editor_game_camera)
        set(_game_camera_kind integration)
    else()
        set(_game_camera_target "test_jce_editor_game_camera_${_game_camera_variant}")
        set(_game_camera_kind contract)
    endif()
    jce_add_cxx_test(${_game_camera_target} KIND ${_game_camera_kind}
        SOURCES test_jce_editor_game_camera.cpp LINK jce_core jce_renderer)
    target_compile_features(${_game_camera_target} PRIVATE cxx_std_20)
    target_compile_definitions(${_game_camera_target} PRIVATE
        JCE_EDITOR_GAME_CAMERA_OWNER="${_game_camera_dir}/${_game_camera_variant}_content.inc.h"
        JCE_EDITOR_GAME_CAMERA_CREATE_WIRE="${_game_camera_dir}/${_game_camera_variant}_create_content.inc.h"
        JCE_EDITOR_GAME_CAMERA_STOP_WIRE="${_game_camera_dir}/${_game_camera_variant}_stop.inc.h"
        JCE_EDITOR_GAME_CAMERA_RETRY_WIRE="${_game_camera_dir}/${_game_camera_variant}_retry.inc.h"
        JCE_EDITOR_GAME_CAMERA_TAA_WIRE="${_game_camera_dir}/${_game_camera_variant}_taa_content.inc.h"
        JCE_EDITOR_GAME_CAMERA_APPLY_WIRE="${_game_camera_dir}/${_game_camera_variant}_apply.inc.h")
    if(_game_camera_kind STREQUAL "contract")
        set_tests_properties(${_game_camera_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
foreach(_game_camera_var IN ITEMS owner play_owner source play_source state base base_pos helpers init shutdown lifecycle attach get frame frame_trace actual create stop retry retry_pos taa apply taa_pos apply_pos dir variant content create_content taa_content needle first part path previous target kind)
    unset(_game_camera_${_game_camera_var})
endforeach()
unset(_game_camera_var)

# Explicit FrameLog natural selection uses the existing real ImGui work fixture.
# Remove only its actual selector-to-capture wire for an ordinary contract fail.
set(_viewport_frame_log_owner "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_viewport_work.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_viewport_frame_log_owner}")
file(READ "${_viewport_frame_log_owner}" _viewport_frame_log_source)
set(_viewport_frame_log_needle
    "jce_editor_frame_log_viewport_policy(std::getenv(\"JCE_KPI_FRAME_LOG_VIEWPORT\")) !=\n            JceEditorFrameLogViewportPolicy::Natural")
string(FIND "${_viewport_frame_log_source}" "${_viewport_frame_log_needle}" _viewport_frame_log_first)
string(FIND "${_viewport_frame_log_source}" "${_viewport_frame_log_needle}" _viewport_frame_log_last REVERSE)
if(_viewport_frame_log_first LESS 0 OR NOT _viewport_frame_log_first EQUAL _viewport_frame_log_last)
    message(FATAL_ERROR "FrameLog natural mutation requires its one actual policy wire")
endif()
string(REPLACE "${_viewport_frame_log_needle}" "true"
    _viewport_frame_log_mutant "${_viewport_frame_log_source}")
set(_viewport_frame_log_dir "${CMAKE_CURRENT_BINARY_DIR}/viewport-frame-log")
file(MAKE_DIRECTORY "${_viewport_frame_log_dir}")
set(_viewport_frame_log_header "${_viewport_frame_log_dir}/jce_editor_viewport_work_missing_frame_log_policy.h")
unset(_viewport_frame_log_previous)
if(EXISTS "${_viewport_frame_log_header}")
    file(READ "${_viewport_frame_log_header}" _viewport_frame_log_previous)
endif()
if(NOT _viewport_frame_log_previous STREQUAL _viewport_frame_log_mutant)
    file(WRITE "${_viewport_frame_log_header}" "${_viewport_frame_log_mutant}")
endif()
_jce_editor_viewport_work_fixture(test_jce_editor_viewport_work_missing_frame_log_policy contract)
target_compile_definitions(test_jce_editor_viewport_work_missing_frame_log_policy PRIVATE
    JCE_TEST_EDITOR_VIEWPORT_WORK_HEADER="${_viewport_frame_log_header}")
set_tests_properties(test_jce_editor_viewport_work_missing_frame_log_policy PROPERTIES WILL_FAIL TRUE)
foreach(_viewport_frame_log_var IN ITEMS owner source needle first last mutant dir header previous)
    unset(_viewport_frame_log_${_viewport_frame_log_var})
endforeach()
unset(_viewport_frame_log_var)

include(native_module_images.cmake)

include(render_trace_controls.cmake)

include(welcome_startup_controls.cmake)

include(pipeline_panel_controls.cmake)

include(tsr_history_controls.cmake)

include(scene_overview_controls.cmake)

include(console_viewport_controls.cmake)

include(loose_scene_content_controls.cmake)

include(frame_observation_controls.cmake)

include(lan_discovery_controls.cmake)

include(mouse_input_controls.cmake)

include(launch_diagnostics_controls.cmake)

include(scene_content_clock_controls.cmake)

include(draw_log_controls.cmake)

# Actual shared Play overlay with public SDK types and counted CPU endpoints.
jce_add_cxx_test(test_jce_physics_overlay
    SOURCES test_jce_physics_overlay.cpp)

include(asset_caption_controls.cmake)

include(asset_directory_controls.cmake)
