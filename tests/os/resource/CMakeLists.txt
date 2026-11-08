cmake_minimum_required(VERSION 3.20)

# ----------------------------------------------------------------------- #
#  L2 (resource) unit tests.                                              #
#                                                                         #
#  Tests at this layer cook synthetic PAK / bundle blobs in-process and   #
#  exercise the runtime readers.  They link the `jce_resource` layer      #
#  library; xxHash is required for path-hash computation in cooked PAKs   #
#  so it is added explicitly here (the engine keeps xxhash PRIVATE).      #
# ----------------------------------------------------------------------- #

jce_add_unit_test(test_jce_pak_loader
    SOURCES test_jce_pak_loader.c
    LINK    jce_core jce_resource)

jce_add_unit_test(test_jce_bundle_deps
    SOURCES test_jce_bundle_deps.c
    LINK    jce_core jce_resource)
target_include_directories(test_jce_bundle_deps PRIVATE "${CMAKE_SOURCE_DIR}/engine/src")

jce_add_unit_test(test_jce_bundle_pack
    SOURCES test_jce_bundle_pack.c
    LINK    jce_core jce_resource)
target_include_directories(test_jce_bundle_pack PRIVATE "${CMAKE_SOURCE_DIR}/engine/src")
include(bundle_partition_controls.cmake)
if(TARGET cjson::cjson)
    target_link_libraries(test_jce_bundle_pack PRIVATE cjson::cjson)
endif()

jce_add_unit_test(test_jce_bundle_loader
    SOURCES test_jce_bundle_loader.c
    LINK    jce_core jce_resource)
if(TARGET xxHash::xxhash)
    target_link_libraries(test_jce_bundle_loader PRIVATE xxHash::xxhash)
endif()

# Actual public Bundle/Pak/VFS same-handle admission, including legacy provider
# restoration and opaque custom providers. This is not a custom-decoder sandbox.
jce_add_unit_test(test_jce_bundle_inspection_mount
    SOURCES test_jce_bundle_inspection_mount.c
    LINK jce_core jce_resource)
target_compile_definitions(test_jce_bundle_inspection_mount PRIVATE
    JCE_BUNDLE_INSPECTION_FIXTURE_NAMESPACE="test_jce_bundle_inspection_mount")

# Single-owner mutations retain the exact unchanged fixture and actual library
# dependencies. A configure-time byte delta checks the exact number of wires;
# CTest WILL_FAIL still requires inspection of normal Unity assertion exits.
function(_jce_bundle_inspection_negative NAME OWNER WIRE REPLACEMENT COUNT)
    set(_owner "${CMAKE_SOURCE_DIR}/${OWNER}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_owner}")
    file(READ "${_owner}" _source)
    string(REPLACE "${WIRE}" "${REPLACEMENT}" _mutant "${_source}")
    string(LENGTH "${_source}" _before)
    string(LENGTH "${_mutant}" _after)
    string(LENGTH "${WIRE}" _wire_size)
    string(LENGTH "${REPLACEMENT}" _replacement_size)
    math(EXPR _actual "${_before} - ${_after}")
    math(EXPR _expected "(${_wire_size} - ${_replacement_size}) * ${COUNT}")
    if(_expected EQUAL 0 OR NOT _actual EQUAL _expected)
        message(FATAL_ERROR "Expected exactly ${COUNT} actual admission wire(s) for ${NAME}")
    endif()
    set(_file "${CMAKE_CURRENT_BINARY_DIR}/${NAME}_owner.c")
    jce_test_write_if_changed("${_file}" "${_mutant}")
    get_filename_component(_owner_dir "${_owner}" DIRECTORY)
    jce_add_unit_test(${NAME}
        KIND contract
        SOURCES test_jce_bundle_inspection_mount.c "${_file}"
        LINK jce_core jce_resource sdl::sdl physfs-static cjson::cjson xxHash::xxhash
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${_owner_dir}")
    target_compile_definitions(${NAME} PRIVATE JCE_BUNDLE_INSPECTION_FIXTURE_NAMESPACE="${NAME}")
    set_tests_properties(${NAME} PROPERTIES WILL_FAIL TRUE)
endfunction()
_jce_bundle_inspection_negative(test_jce_bundle_inspection_missing_mark
    "engine/src/resource/jce_bundle_loader.c"
    "if (inspection_only) jce_pak_mark_inspection_only_internal(pak);"
    "(void)inspection_only;" 1)
_jce_bundle_inspection_negative(test_jce_bundle_inspection_missing_mount_guard
    "engine/src/os/core/jce_filesystem.c"
    "if (s_pak_service.current_mount_allowed && !s_pak_service.current_mount_allowed(pak)) return false;"
    "/* old mount admission */" 2)
_jce_bundle_inspection_negative(test_jce_bundle_inspection_missing_fallback_guard
    "engine/src/resource/jce_pak_loader.c"
    "if (!pak_mount_allowed(base) || !pak_mount_allowed(layer)) return;"
    "/* old fallback admission */" 1)
_jce_bundle_inspection_negative(test_jce_bundle_inspection_missing_no_provider_guard
    "engine/src/os/core/jce_filesystem.c"
    "if (!s_pak_service.current_provider) return false;"
    "/* old unregistered provider admission */" 2)
_jce_bundle_inspection_negative(test_jce_bundle_inspection_missing_shared_tail_guard
    "engine/src/resource/jce_pak_loader.c"
    "if (tail == layer_tail) return;"
    "/* old tail join */" 1)

jce_add_unit_test(test_jce_archive
    SOURCES test_jce_archive.c
    LINK    jce_core jce_resource)
target_include_directories(test_jce_archive PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")

jce_add_unit_test(test_jce_mod_loader
    SOURCES test_jce_mod_loader.c
    LINK    jce_core jce_resource)

# FBX skinned-animation extraction (assimp path).  Links jce_resource (which
# transitively pulls jce_renderer + jce_animation for the skeleton/clip queries
# and the JceModelCpu inspectors).  Needs engine/src on the include path to
# reach the src-internal headers renderer/jce_gltf_loader.h and
# middleware/animation/jce_animation.h.  CI-safe: the rigged FBX fixtures are
# gitignored, so the test skips+passes when absent, plus a fixture-independent
# synthetic-OBJ assertion that always runs.
jce_add_unit_test(test_jce_fbx_skinned
    SOURCES test_jce_fbx_skinned.c
    LINK    jce_core jce_resource)
target_include_directories(test_jce_fbx_skinned PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")

# Pure overflow-safe bounds primitives (header-only).  Needs engine/src on the
# include path to reach the resource-internal header jce_read_bounds.h.
jce_add_unit_test(test_jce_read_bounds
    SOURCES test_jce_read_bounds.c
    LINK    jce_core)
target_include_directories(test_jce_read_bounds PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")

# Tests that hand-cook PAK blobs need XXH3 to compute path_hash values.
if(TARGET xxHash::xxhash)
    target_link_libraries(test_jce_pak_loader PRIVATE xxHash::xxhash)
    target_link_libraries(test_jce_archive PRIVATE xxHash::xxhash)
    target_link_libraries(test_jce_mod_loader PRIVATE xxHash::xxhash)
    target_link_libraries(test_jce_bundle_pack PRIVATE xxHash::xxhash)
endif()

# Per-asset incremental cook cache (Feature 0.4).  Compiles the host cooker's
# catalog helper (tools/jce_cook_catalog.c) directly so the test exercises the
# REAL skip-decision code path the --batch cooker uses (no mock).  Needs the
# tools/ dir on the include path for jce_cook_catalog.h and xxHash for XXH3.
jce_add_unit_test(test_jce_cook_incremental
    SOURCES test_jce_cook_incremental.c
            "${CMAKE_SOURCE_DIR}/tools/jce_cook_catalog.c"
    LINK    jce_core jce_resource)
target_include_directories(test_jce_cook_incremental PRIVATE
    "${CMAKE_SOURCE_DIR}/tools")
if(TARGET xxHash::xxhash)
    target_link_libraries(test_jce_cook_incremental PRIVATE xxHash::xxhash)
endif()

# Actual Web/mobile texture outputs and legacy-cache migration. Compile each
# complete legacy/CLI owner once and reuse it across the unchanged assertions.
# This is CPU decode/cook/archive evidence, never renderer or browser acceptance.
if(TARGET jce_tool_cli_io)
    set(_cook_fixture_dir "${CMAKE_CURRENT_BINARY_DIR}/cook-platform-owners")
    set(_cook_resource_dir "${CMAKE_SOURCE_DIR}/engine/src/resource")
    file(MAKE_DIRECTORY "${_cook_fixture_dir}/legacy" "${_cook_fixture_dir}/old-web")
    foreach(_cook_owner jce_asset_cooker.c jce_bundle_pack_cook.c jce_bundle_pack.c jce_cook_policy.h)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${_cook_resource_dir}/${_cook_owner}")
    endforeach()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/tools/jce_cook.c")
    file(READ "${_cook_resource_dir}/jce_cook_policy.h" _cook_policy)
    file(READ "${_cook_resource_dir}/jce_asset_cooker.c" _cook_asset)
    file(READ "${_cook_resource_dir}/jce_bundle_pack_cook.c" _cook_bundle_asset)
    file(READ "${_cook_resource_dir}/jce_bundle_pack.c" _cook_bundle)
    file(READ "${CMAKE_SOURCE_DIR}/tools/jce_cook.c" _cook_cli)
    # FIND/REVERSE count the single full wire, even for same-byte-length salts;
    # REGEX MATCHALL/list counting would split C statements on semicolons.
    function(_jce_cook_exact_mutation INPUT WIRE REPLACEMENT OUT)
        string(FIND "${INPUT}" "${WIRE}" _first)
        string(FIND "${INPUT}" "${WIRE}" _last REVERSE)
        if(_first LESS 0 OR NOT _first EQUAL _last)
            message(FATAL_ERROR "Expected one actual cook wire: ${WIRE}")
        endif()
        string(REPLACE "${WIRE}" "${REPLACEMENT}" _changed "${INPUT}")
        if("${_changed}" STREQUAL "${INPUT}")
            message(FATAL_ERROR "Actual cook wire mutation was not applied")
        endif()
        set(${OUT} "${_changed}" PARENT_SCOPE)
    endfunction()
    _jce_cook_exact_mutation("${_cook_policy}"
        "case JCE_COOK_PLATFORM_WEB:\n        return JCEASSET_TEXFMT_RGBA8;"
        "case JCE_COOK_PLATFORM_WEB:\n        return JCEASSET_TEXFMT_ASTC_4x4;" _cook_old_policy)
    _jce_cook_exact_mutation("${_cook_bundle}" "uint32_t writer_revision = 4u;"
        "uint32_t writer_revision = 3u;" _cook_old_bundle_salt)
    _jce_cook_exact_mutation("${_cook_cli}" "uint8_t sv = 3u;"
        "uint8_t sv = 2u;" _cook_old_cli_salt)
    # Copied whole CLI owners need the same private include expressed relative
    # to the inherited src root; this rebases only its source-location spelling.
    string(REPLACE "../engine/src/resource/jce_audio_import_settings.h"
        "resource/jce_audio_import_settings.h" _cook_old_cli_salt "${_cook_old_cli_salt}")
    jce_test_write_if_changed("${_cook_fixture_dir}/legacy/jce_cook_policy.h" "${_cook_old_policy}")
    jce_test_write_if_changed("${_cook_fixture_dir}/old-web/jce_cook_policy.h" "${_cook_old_policy}")
    set(_cook_legacy_names [=[#define jce_cook_texture jce_legacy_cook_texture
#define jce_cook_audio jce_legacy_cook_audio
#define jce_cook_raw jce_legacy_cook_raw
#define jce_cook_file jce_legacy_cook_file
#define jce_cook_result_free jce_legacy_cook_result_free
#define jce_cook_write jce_legacy_cook_write
#define jce_cook_detect_type jce_legacy_cook_detect_type
]=])
    set(_cook_legacy_bundle_names [=[#define jce_bundle_pack_run jce_legacy_bundle_pack_run
#define jce_bundle_pack_diff jce_legacy_bundle_pack_diff
#define jce_bundle_cook_asset jce_legacy_bundle_cook_asset
#define jce_bundle_cook_jobs_range jce_legacy_bundle_cook_jobs_range
#define pack_log_info jce_legacy_pack_log_info
#define read_asset jce_legacy_read_asset
#define ends_with_ci jce_legacy_ends_with_ci
]=])
    # MSVC stdio supplies inline printf definitions: process its actual header
    # before redirecting call sites, never rename the CRT declaration/body.
    set(_cook_stdout_port [=[#include <stdio.h>
int jce_test_cook_platform_printf(const char *format, ...);
#define printf jce_test_cook_platform_printf
]=])
    jce_test_write_if_changed("${_cook_fixture_dir}/cli.c"
        "${_cook_stdout_port}#include \"${CMAKE_SOURCE_DIR}/tools/jce_cook.c\"\n")
    jce_test_write_if_changed("${_cook_fixture_dir}/legacy/asset.c" "${_cook_legacy_names}${_cook_asset}")
    jce_test_write_if_changed("${_cook_fixture_dir}/legacy/bundle_asset.c"
        "${_cook_legacy_names}${_cook_legacy_bundle_names}${_cook_bundle_asset}")
    jce_test_write_if_changed("${_cook_fixture_dir}/legacy/bundle.c"
        "${_cook_legacy_bundle_names}${_cook_old_bundle_salt}")
    jce_test_write_if_changed("${_cook_fixture_dir}/legacy/cli.c"
        "${_cook_stdout_port}#define jce_tool_main jce_legacy_cook_tool_main\n${_cook_legacy_names}${_cook_old_cli_salt}")
    jce_test_write_if_changed("${_cook_fixture_dir}/old-web/asset.c" "${_cook_asset}")
    jce_test_write_if_changed("${_cook_fixture_dir}/old-web/bundle_asset.c" "${_cook_bundle_asset}")
    jce_test_write_if_changed("${_cook_fixture_dir}/old_cli_salt.c" "${_cook_stdout_port}${_cook_old_cli_salt}")
    jce_test_write_if_changed("${_cook_fixture_dir}/old_bundle_salt.c" "${_cook_old_bundle_salt}")
    function(_jce_cook_cpu_owner NAME)
        add_library(${NAME} OBJECT ${ARGN})
        set_target_properties(${NAME} PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES C_EXTENSIONS OFF)
        target_include_directories(${NAME} PRIVATE
            "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/resource"
            "${CMAKE_SOURCE_DIR}/tools")
        target_link_libraries(${NAME} PRIVATE jce_resource sdl::sdl miniaudio::miniaudio
            cgltf::cgltf cjson::cjson xxHash::xxhash)
        target_compile_definitions(${NAME} PRIVATE JCE_BUILDING_ENGINE=1)
    endfunction()
    _jce_cook_cpu_owner(jce_test_cook_platform_helpers
        "${CMAKE_SOURCE_DIR}/tools/jce_cook_atlas.c" "${CMAKE_SOURCE_DIR}/tools/jce_cook_catalog.c")
    _jce_cook_cpu_owner(jce_test_cook_platform_cli "${_cook_fixture_dir}/cli.c")
    _jce_cook_cpu_owner(jce_test_cook_platform_legacy
        "${_cook_fixture_dir}/legacy/asset.c" "${_cook_fixture_dir}/legacy/bundle_asset.c"
        "${_cook_fixture_dir}/legacy/bundle.c" "${_cook_fixture_dir}/legacy/cli.c")
    _jce_cook_cpu_owner(jce_test_cook_platform_old_web
        "${_cook_fixture_dir}/old-web/asset.c" "${_cook_fixture_dir}/old-web/bundle_asset.c")
    _jce_cook_cpu_owner(jce_test_cook_platform_old_cli_salt "${_cook_fixture_dir}/old_cli_salt.c")
    _jce_cook_cpu_owner(jce_test_cook_platform_old_bundle_salt "${_cook_fixture_dir}/old_bundle_salt.c")
    function(_jce_cook_output_fixture NAME KIND)
        jce_add_unit_test(${NAME} KIND ${KIND}
            SOURCES test_jce_cook_platform_outputs.c
                $<TARGET_OBJECTS:jce_test_cook_platform_helpers>
                $<TARGET_OBJECTS:jce_test_cook_platform_legacy> ${ARGN}
            LINK jce_core jce_resource jce_physics jce_platform jce_tool_cli_io
            INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/tools")
        target_compile_definitions(${NAME} PRIVATE JCE_COOK_PLATFORM_FIXTURE_NAMESPACE="${NAME}")
    endfunction()
    _jce_cook_output_fixture(test_jce_cook_platform_outputs integration
        $<TARGET_OBJECTS:jce_test_cook_platform_cli>)
    _jce_cook_output_fixture(test_jce_cook_platform_old_web_default contract
        $<TARGET_OBJECTS:jce_test_cook_platform_cli> $<TARGET_OBJECTS:jce_test_cook_platform_old_web>)
    _jce_cook_output_fixture(test_jce_cook_platform_old_cli_salt contract
        $<TARGET_OBJECTS:jce_test_cook_platform_old_cli_salt>)
    _jce_cook_output_fixture(test_jce_cook_platform_old_bundle_salt contract
        $<TARGET_OBJECTS:jce_test_cook_platform_cli> $<TARGET_OBJECTS:jce_test_cook_platform_old_bundle_salt>)
    set_tests_properties(test_jce_cook_platform_old_web_default
        test_jce_cook_platform_old_cli_salt test_jce_cook_platform_old_bundle_salt
        PROPERTIES WILL_FAIL TRUE)
    unset(_cook_fixture_dir)
    unset(_cook_resource_dir)
    unset(_cook_owner)
    unset(_cook_policy)
    unset(_cook_asset)
    unset(_cook_bundle_asset)
    unset(_cook_bundle)
    unset(_cook_cli)
    unset(_cook_old_policy)
    unset(_cook_old_bundle_salt)
    unset(_cook_old_cli_salt)
    unset(_cook_stdout_port)
    unset(_cook_legacy_names)
    unset(_cook_legacy_bundle_names)
else()
    jce_test_suite_skipped("engine/cook-platform-outputs"
        "actual host CLI I/O target unavailable; CPU product controls are not registered")
endif()

# REF-007 (dedup audit) — the asset manager's registry key must share the
# archive layer's resource identity (jce_archive_hash_path).  Before the fix
# it hashed the RAW path string, so two spellings of one asset occupied two
# slots and were decoded/uploaded twice while resolving to the same entry.
jce_add_unit_test(test_jce_asset_path_identity
    SOURCES test_jce_asset_path_identity.c
    LINK    jce_core jce_resource)
if(TARGET xxHash::xxhash)
    target_link_libraries(test_jce_asset_path_identity PRIVATE xxHash::xxhash)
endif()

# REF-008 (dedup audit) — one canonical extension -> asset-kind table
# (jce_asset_type_from_ext).  Five tables previously disagreed about which
# extensions are textures/models, so the asset browser could label a file
# "Texture" while the cooker packed it as an opaque RAW blob.  Also pins the
# deliberate split between classification and cooker capability.
jce_add_unit_test(test_jce_asset_ext
    SOURCES test_jce_asset_ext.c
    LINK    jce_core jce_resource)

jce_add_unit_test(test_jce_streaming_async
    SOURCES test_jce_streaming_async.c
    LINK    jce_core jce_resource)

jce_add_unit_test(test_jce_numeric_texture
    SOURCES test_jce_numeric_texture.c
    LINK    jce_core jce_resource
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")
if(TARGET xxHash::xxhash)
    target_link_libraries(test_jce_numeric_texture PRIVATE xxHash::xxhash)
endif()

# Exception firewall on the model importer's extern "C" surface.  That TU is
# C++ behind a flat C ABI and it parses UNTRUSTED model bytes, so an escaping
# exception is undefined behaviour.  These feed it garbage and assert it
# RETURNS FALSE rather than unwinding past the C boundary.
jce_add_unit_test(test_jce_model_importer_firewall
    SOURCES test_jce_model_importer_firewall.c
    LINK    jce_core jce_resource)

# Cooked terrain v3 codec.  The corruption cases matter more than the happy
# path: a codec that decodes valid files is easy, one that REJECTS invalid
# ones is what makes streaming and byte-identical double-cook safe.
jce_add_unit_test(test_jce_terrain_format
    SOURCES test_jce_terrain_format.c
            "${CMAKE_SOURCE_DIR}/engine/src/resource/jce_terrain_format.c"
    LINK    jce_core xxHash::xxhash)
target_include_directories(test_jce_terrain_format PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src/resource"
    "${CMAKE_SOURCE_DIR}/engine/src")

# Terrain must have ONE owner.  The ownership tests are the ones that matter:
# a pinned tile must never be evicted, a stale handle must fail closed, and
# destroy must refuse while a view is borrowed.
jce_add_unit_test(test_jce_terrain_store
    SOURCES test_jce_terrain_store.c
            "${CMAKE_SOURCE_DIR}/engine/src/resource/jce_terrain_store.c"
    LINK    jce_core)
target_include_directories(test_jce_terrain_store PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src/resource"
    "${CMAKE_SOURCE_DIR}/engine/src")

# End-to-end: cooked v3 bytes -> source -> store -> pinned tile view.  The
# codec and the store are each tested in isolation; this proves they compose.
jce_add_unit_test(test_jce_terrain_source
    SOURCES test_jce_terrain_source.c
            "${CMAKE_SOURCE_DIR}/engine/src/resource/jce_terrain_source.c"
            "${CMAKE_SOURCE_DIR}/engine/src/resource/jce_terrain_store.c"
            "${CMAKE_SOURCE_DIR}/engine/src/resource/jce_terrain_format.c"
    LINK    jce_core xxHash::xxhash)
target_include_directories(test_jce_terrain_source PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src/resource"
    "${CMAKE_SOURCE_DIR}/engine/src")

# Multi-tile cooking: what makes the v3 directory worth having.  Asserts that
# each tile decodes independently, that ADJACENT tiles agree on their shared
# edge (the one-sample overlap the paging depends on), that corruption stays
# local to one tile, and that cooked bytes reach a consumer through the real
# source + store with a budget too small to hold them all.
jce_add_unit_test(test_jce_terrain_cook_tiled
    SOURCES test_jce_terrain_cook_tiled.c
            "${CMAKE_SOURCE_DIR}/engine/src/resource/jce_terrain_source.c"
            "${CMAKE_SOURCE_DIR}/engine/src/resource/jce_terrain_store.c"
            "${CMAKE_SOURCE_DIR}/engine/src/resource/jce_terrain_format.c"
    LINK    jce_core xxHash::xxhash)
target_include_directories(test_jce_terrain_cook_tiled PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src/resource"
    "${CMAKE_SOURCE_DIR}/engine/src")

# Mipmaps of sRGB colour must be averaged in linear light.
#
# The defect is invisible on flat content -- a box filter over raw sRGB bytes
# is exactly right wherever the source is uniform -- so it survived until a
# measurement over real textures showed worst per-texel errors of 73 levels and
# a signed -14.5 luma / +16 R-B shift by mip 7 on a leaf atlas.  The cases here
# use contrast on purpose, and assert the linear-data path is untouched,
# because decoding a normal map would corrupt values that are currently right.
# Links jce_resource rather than compiling tools/jce_tex_compress.c directly:
# that TU calls jce_tex_encoded_size, which lives in the bimg-backed
# jce_tex_encode.cpp, and jce_resource already carries both.
jce_add_unit_test(test_jce_tex_mip_srgb
    SOURCES test_jce_tex_mip_srgb.c
    LINK    jce_core jce_resource)
target_include_directories(test_jce_tex_mip_srgb PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src/resource"
    "${CMAKE_SOURCE_DIR}/engine/src")

# ----------------------------------------------------------------------- #
#  THE COOK TRAP GATE — a script must never reach a PAK under a name no    #
#  language claims.                                                        #
#                                                                          #
#  `jce_cook --batch` without `--preserve-names` renamed every asset to    #
#  <stem>.jceasset.  A cooked asset survives that (its container carries a #
#  type tag); a SCRIPT does not — it ships as its own bytes and its        #
#  extension is the only record of which language it is.  The renamed      #
#  script resolved to no language and died IN A PACKAGED BUILD ONLY, since #
#  the editor keeps reading loose files.  See the header of                #
#  test_jce_cook_script_names.c for the measurement, including the         #
#  .java/.class pair that collapsed onto ONE output name.                  #
#                                                                          #
#  Two registrations, on purpose.  The setup step runs the REAL jce_cook   #
#  and jce_pak (deliberately WITHOUT --preserve-names) as a CTest fixture, #
#  so a failure in either tool is a red test naming its stage rather than  #
#  a build error that stops the checker from ever running.  The checker is #
#  a plain Unity test that reads the artefact those tools produced.        #
# ----------------------------------------------------------------------- #

set(_csn_have_cook NO)
set(_csn_have_pak  NO)
if(TARGET jce_cook)
    set(_csn_have_cook YES)
endif()
if(TARGET jce_pak)
    set(_csn_have_pak YES)
endif()

if(_csn_have_cook AND _csn_have_pak)
    message(STATUS
        "cook trap gate: WIRED — jce_cook and jce_pak will cook and pack the "
        "script-name fixture")

    set(_csn_work "${CMAKE_BINARY_DIR}/tests/cook_script_names")

    add_test(NAME cook_script_names_setup
        COMMAND "${CMAKE_COMMAND}"
                "-DJCE_COOK_EXE=$<TARGET_FILE:jce_cook>"
                "-DJCE_PAK_EXE=$<TARGET_FILE:jce_pak>"
                "-DWORK_DIR=${_csn_work}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cook_script_names_setup.cmake")
    # A bare add_test(): tests/CMakeLists.txt refuses one that is not armed
    # with JCE_TEST_ENVIRONMENT, so both halves of that registration are done
    # by hand here (same as tests/scripting/java's differential runner).
    set_tests_properties(cook_script_names_setup PROPERTIES
        TIMEOUT        300
        LABELS         "unit" JCE_TEST_KIND integration
        FIXTURES_SETUP cook_script_names
        ENVIRONMENT    "${JCE_TEST_ENVIRONMENT}")
    set_property(GLOBAL APPEND PROPERTY JCE_TESTS_WITH_ENV
        cook_script_names_setup)

    jce_add_unit_test(test_jce_cook_script_names
        SOURCES test_jce_cook_script_names.c
        LINK    jce_core jce_resource)
    target_compile_definitions(test_jce_cook_script_names PRIVATE
        "JCE_COOK_SCRIPT_NAMES_PAK=\"${_csn_work}/cook_script_names.pak\"")
    set_tests_properties(test_jce_cook_script_names PROPERTIES
        FIXTURES_REQUIRED cook_script_names)
    if(TARGET xxHash::xxhash)
        target_link_libraries(test_jce_cook_script_names PRIVATE xxHash::xxhash)
    endif()

    # The suite's GENUINE build inputs, named as such.  The setup step's
    # COMMAND is "${CMAKE_COMMAND}" — a variable reference, which
    # tools/lint/check_jce_tests_closure.py deliberately excludes from its
    # rule because no add_dependencies can name a target no build produces.
    # The two tools ARE targets this build produces, and without these lines
    # `cmake --build --target jce_tests` (what CI builds) leaves them absent
    # and the fixture dies on a missing executable.  Guarded on IMPORTED: a
    # cross-compiling configure declares jce_pak/jce_cook as imported
    # locations of a host build, and add_dependencies on an imported target
    # is a hard CMake error.
    foreach(_csn_tool jce_cook jce_pak)
        get_target_property(_csn_imported ${_csn_tool} IMPORTED)
        if(_csn_imported)
            message(STATUS
                "cook trap gate: ${_csn_tool} is IMPORTED (host tool from "
                "another build) — not added to the jce_tests closure")
        else()
            jce_test_build_dependencies(${_csn_tool})
        endif()
    endforeach()
    unset(_csn_imported)
    unset(_csn_tool)
    unset(_csn_work)
else()
    # Printed by name, never silent.  A gate that skips itself when the thing
    # it needs is missing is indistinguishable from a gate that passed.
    message(WARNING
        "cook trap gate: NOT WIRED — jce_cook target: ${_csn_have_cook}, "
        "jce_pak target: ${_csn_have_pak}. Nothing checks that a script "
        "reaches a PAK under a name a language claims in this configuration.")
endif()
unset(_csn_have_cook)
unset(_csn_have_pak)

# Model import options: the editor writes <asset>.import.json for every model a
# preset touches, and the importer's assimp flag word was a constant -- so
# flip_uv / gen_normals / gen_tangents / merge_meshes / scale all reached
# nothing.  The observable is the imported VERTEX DATA, not the settings struct.
jce_add_unit_test(test_jce_model_import_settings
    SOURCES test_jce_model_import_settings.c
    LINK    JCE)
target_include_directories(test_jce_model_import_settings PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src/resource")

# Sprite atlas packing.  Every property here fails INVISIBLY in a picture -- a
# 1px overlap looks like an artist's edge, a missing padding pixel looks like a
# sprite with a line on it, and an atlas that reshuffles on a directory-order
# change rewrites every UV on a cook that changed nothing.  The packer is pure
# arithmetic precisely so these can be asserted instead of inspected.
jce_add_unit_test(test_jce_atlas_pack
    SOURCES test_jce_atlas_pack.c
    LINK    jce_core jce_resource)

# Per-clip audio import options.  The entire cook-time policy for sound was
# one hardcoded size heuristic with no per-asset override in either
# direction, so a 2.1 MB footstep set paid a decode on every trigger and a
# 1.9 MB ambience loop was expanded to raw PCM in the PAK.  Every failure
# this module can have is SILENT -- a key read under the wrong name, a
# refusal that lands on zeros instead of defaults, an unknown mode falling
# through to "cook" -- and all of them just make the PAK the wrong size.
# Needs engine/src/resource on the include path for the internal header.
jce_add_unit_test(test_jce_audio_import_settings
    SOURCES test_jce_audio_import_settings.c
    LINK    jce_core jce_resource)
target_include_directories(test_jce_audio_import_settings PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src/resource"
    "${CMAKE_SOURCE_DIR}/engine/src"
    # jce_test_file_util.h is a header-only LF-writing helper that happens to
    # live under tests/application.  Reached rather than copied: a second
    # copy would be a second answer to "how does a test write a fixture",
    # and this one already knows that write_text() emits CRLF on Windows.
    "${CMAKE_SOURCE_DIR}/tests/application")

# Authored value curves (the Curve Editor's documents, read back).  Every
# defect this reader can have LOOKS LIKE A WORKING CURVE -- an unscaled
# Hermite tangent is still smooth, a CONSTANT segment read as LINEAR is still
# in range, an unsorted key array evaluates the wrong segment without
# crashing -- so none of them is visible in a picture of the curve, which is
# the only place anyone would otherwise look.  The controls are half the file.
jce_add_unit_test(test_jce_curve
    SOURCES test_jce_curve.c
    LINK    jce_core jce_resource)

# Real asset manager/loaders with counted font/audio/allocation ports; no GPU/device.
jce_add_unit_test(test_jce_asset_font_params
    SOURCES test_jce_asset_font_params.c
        "${CMAKE_SOURCE_DIR}/engine/src/resource/jce_asset_loaders.c"
    LINK jce_core jce_resource
    INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
        "${CMAKE_SOURCE_DIR}/engine/src")
# Scope counted audio ports to the fixture and its exact loader TU. The linked
# real audio archive retains its public symbols and all manager/VFS owners.
target_compile_definitions(test_jce_asset_font_params PRIVATE
    jce_audio_load=jce_test_resource_audio_load
    jce_audio_load_pcm=jce_test_resource_audio_load_pcm
    jce_audio_unload=jce_test_resource_audio_unload)
if(TARGET xxHash::xxhash)
    target_link_libraries(test_jce_asset_font_params PRIVATE xxHash::xxhash)
endif()
# The exact private loader header uses SDL atomic declarations. Keep these
# compile includes local to the fixture; core/SDK public includes remain clean.
if(TARGET SDL3::Headers)
    target_link_libraries(test_jce_asset_font_params PRIVATE SDL3::Headers)
else()
    set(_asset_font_sdl_headers "")
    foreach(_asset_font_sdl_target SDL3::SDL3 sdl::sdl)
        if(TARGET ${_asset_font_sdl_target})
            get_target_property(_asset_font_sdl_headers ${_asset_font_sdl_target}
                INTERFACE_INCLUDE_DIRECTORIES)
            if(_asset_font_sdl_headers)
                break()
            endif()
        endif()
    endforeach()
    if(NOT _asset_font_sdl_headers)
        message(FATAL_ERROR "asset font fixture requires the configured SDL header interface")
    endif()
    target_include_directories(test_jce_asset_font_params SYSTEM PRIVATE
        ${_asset_font_sdl_headers})
    unset(_asset_font_sdl_headers)
    unset(_asset_font_sdl_target)
endif()

# Exact host pack-tool TU with real public Bundle/Archive owners, never bootstrap.
# CPU closure/provenance/integrity controls do not certify a relocated application.
if(TARGET jce_tool_cli_io)
    jce_add_unit_test(test_jce_pak_selected_archive
        KIND integration
        SOURCES test_jce_pak_selected_archive.c
            "${CMAKE_SOURCE_DIR}/tools/jce_pak.c"
        LINK jce_core jce_resource jce_platform jce_tool_cli_io
            cjson::cjson xxHash::xxhash zstd::libzstd_static
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src"
            "${CMAKE_SOURCE_DIR}/tools")
    target_compile_definitions(test_jce_pak_selected_archive PRIVATE
        JCE_PAK_SELECTED_FIXTURE_BASE="${CMAKE_CURRENT_BINARY_DIR}/selected-archive-fixtures")

    # Compile the actual tool with only the verified report-address wire removed.
    # Count its semicolon-free signature; CMake lists split C statements at ';'.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/tools/jce_pak.c")
    file(READ "${CMAKE_SOURCE_DIR}/tools/jce_pak.c" _pak_bom_source)
    string(REGEX MATCHALL [[p = sources\[s\]\.rel_path]]
        _pak_bom_name_wires "${_pak_bom_source}")
    list(LENGTH _pak_bom_name_wires _pak_bom_name_wire_count)
    if(NOT _pak_bom_name_wire_count EQUAL 1)
        message(FATAL_ERROR "Expected one actual verified pack BOM address wire")
    endif()
    string(REPLACE "p = sources[s].rel_path;" "p = NULL;"
        _pak_bom_mutant "${_pak_bom_source}")
    if("${_pak_bom_mutant}" STREQUAL "${_pak_bom_source}")
        message(FATAL_ERROR "Actual pack BOM address mutation was not applied")
    endif()
    set(_pak_bom_mutant_file "${CMAKE_CURRENT_BINARY_DIR}/jce_pak_missing_bom_source_names.c")
    jce_test_write_if_changed("${_pak_bom_mutant_file}" "${_pak_bom_mutant}")
    jce_add_unit_test(test_jce_pak_selected_archive_missing_bom_source_names
        KIND integration
        SOURCES test_jce_pak_selected_archive.c "${_pak_bom_mutant_file}"
        LINK jce_core jce_resource jce_platform jce_tool_cli_io
            cjson::cjson xxHash::xxhash zstd::libzstd_static
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/tools")
    target_compile_definitions(test_jce_pak_selected_archive_missing_bom_source_names PRIVATE
        JCE_PAK_SELECTED_FIXTURE_BASE="${CMAKE_CURRENT_BINARY_DIR}/missing-bom-source-names-fixtures")
    set_tests_properties(test_jce_pak_selected_archive_missing_bom_source_names
        PROPERTIES WILL_FAIL TRUE)
    # Restore only the tool's former host-as-target dispatch. Runtime policy
    # and unchanged real Bundle/Archive assertions remain independent.
    set(_pak_target_wire "selected->target = expected_target;")
    string(REPLACE "${_pak_target_wire}" "selected->target = NULL;"
        _pak_host_target_mutant "${_pak_bom_source}")
    string(LENGTH "${_pak_bom_source}" _pak_target_before)
    string(LENGTH "${_pak_host_target_mutant}" _pak_target_after)
    string(LENGTH "${_pak_target_wire}" _pak_target_wire_size)
    string(LENGTH "selected->target = NULL;" _pak_target_old_size)
    math(EXPR _pak_target_delta "${_pak_target_before} - ${_pak_target_after}")
    math(EXPR _pak_target_expected "${_pak_target_wire_size} - ${_pak_target_old_size}")
    if(NOT _pak_target_delta EQUAL _pak_target_expected)
        message(FATAL_ERROR "Expected exactly one selected target dispatch wire")
    endif()
    set(_pak_host_target_file "${CMAKE_CURRENT_BINARY_DIR}/jce_pak_old_host_target.c")
    jce_test_write_if_changed("${_pak_host_target_file}" "${_pak_host_target_mutant}")
    jce_add_unit_test(test_jce_pak_selected_archive_old_host_target
        KIND integration
        SOURCES test_jce_pak_selected_archive.c "${_pak_host_target_file}"
        LINK jce_core jce_resource jce_platform jce_tool_cli_io
            cjson::cjson xxHash::xxhash zstd::libzstd_static
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/tools")
    target_compile_definitions(test_jce_pak_selected_archive_old_host_target PRIVATE
        JCE_PAK_SELECTED_FIXTURE_BASE="${CMAKE_CURRENT_BINARY_DIR}/old-host-target-fixtures")
    set_tests_properties(test_jce_pak_selected_archive_old_host_target PROPERTIES WILL_FAIL TRUE)
    unset(_pak_target_wire)
    unset(_pak_host_target_mutant)
    unset(_pak_target_before)
    unset(_pak_target_after)
    unset(_pak_target_wire_size)
    unset(_pak_target_old_size)
    unset(_pak_target_delta)
    unset(_pak_target_expected)
    unset(_pak_host_target_file)
    # Each control changes one guard in the complete actual single-entry owner.
    foreach(_read_variant IN ITEMS missing_read_limit missing_read_crc overwrite_read_output)
        if(_read_variant STREQUAL "missing_read_limit")
            set(_read_wire "entry->original_size > 16u * 1024u * 1024u")
            set(_read_old "0")
        elseif(_read_variant STREQUAL "missing_read_crc")
            set(_read_wire "jce_archive_verify_entry(entry, bytes, size);")
            set(_read_old "1;")
        else()
            set(_read_wire "jce_fs_host_write_new(out, bytes, size)")
            set(_read_old "jce_fs_host_write_all(out, bytes, size)")
        endif()
        string(REPLACE "${_read_wire}" "${_read_old}" _read_mutant "${_pak_bom_source}")
        string(LENGTH "${_pak_bom_source}" _read_before)
        string(REPLACE "${_read_wire}" "" _read_without "${_pak_bom_source}")
        string(LENGTH "${_read_without}" _read_after)
        string(LENGTH "${_read_wire}" _read_wire_size)
        string(LENGTH "${_read_old}" _read_old_size)
        math(EXPR _read_delta "${_read_before} - ${_read_after}")
        if(NOT _read_delta EQUAL _read_wire_size)
            message(FATAL_ERROR "Expected exactly one actual single-entry ${_read_variant} wire")
        endif()
        set(_read_file "${CMAKE_CURRENT_BINARY_DIR}/jce_pak_${_read_variant}.c")
        jce_test_write_if_changed("${_read_file}" "${_read_mutant}")
        jce_add_unit_test(test_jce_pak_selected_archive_${_read_variant}
            KIND integration
            SOURCES test_jce_pak_selected_archive.c "${_read_file}"
            LINK jce_core jce_resource jce_platform jce_tool_cli_io
                cjson::cjson xxHash::xxhash zstd::libzstd_static
            INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/tools")
        target_compile_definitions(test_jce_pak_selected_archive_${_read_variant} PRIVATE
            JCE_PAK_SELECTED_FIXTURE_BASE="${CMAKE_CURRENT_BINARY_DIR}/${_read_variant}-fixtures")
        set_tests_properties(test_jce_pak_selected_archive_${_read_variant} PROPERTIES WILL_FAIL TRUE)
    endforeach()
    unset(_read_variant)
    unset(_read_wire)
    unset(_read_old)
    unset(_read_mutant)
    unset(_read_before)
    unset(_read_after)
    unset(_read_wire_size)
    unset(_read_old_size)
    unset(_read_delta)
    unset(_read_without)
    unset(_read_file)
    unset(_pak_bom_source)
    unset(_pak_bom_name_wires)
    unset(_pak_bom_name_wire_count)
    unset(_pak_bom_mutant)
    unset(_pak_bom_mutant_file)
else()
    jce_test_suite_skipped("engine/pak-selected-archive"
        "real host CLI I/O object target is unavailable in this configuration")
endif()

# Explicit native CLI negative for the suffix limit controls in the existing
# host-tool unittest. Reuse the actual packer's complete source/link recipe.
# Only the two new refusal guards are removed, leaving the old bounded copy
# and ignored-overflow behavior intact. Never install this test-only tool.
if(TARGET jce_pak AND NOT CMAKE_CROSSCOMPILING)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/tools/jce_pak.c")
    file(READ "${CMAKE_SOURCE_DIR}/tools/jce_pak.c" _pak_suffix_source)
    set(_pak_suffix_refusal [=[            if (a.exclude_suffix_count >= 16) {
                fprintf(stderr, "[jce_pak] --exclude-suffix accepts at most 16 values\n");
                exit(1);
            }
            if (strlen(val) >= sizeof(a.exclude_suffixes[0])) {
                fprintf(stderr, "[jce_pak] --exclude-suffix value is %zu bytes; maximum %zu bytes\n",
                        strlen(val), sizeof(a.exclude_suffixes[0]) - 1);
                exit(1);
            }
]=])
    string(REPLACE "${_pak_suffix_refusal}" "" _pak_suffix_mutant "${_pak_suffix_source}")
    if("${_pak_suffix_mutant}" STREQUAL "${_pak_suffix_source}")
        message(FATAL_ERROR "Expected actual --exclude-suffix refusal guards")
    endif()
    string(LENGTH "${_pak_suffix_source}" _pak_suffix_source_length)
    string(LENGTH "${_pak_suffix_mutant}" _pak_suffix_mutant_length)
    string(LENGTH "${_pak_suffix_refusal}" _pak_suffix_refusal_length)
    math(EXPR _pak_suffix_removed_length "${_pak_suffix_source_length} - ${_pak_suffix_mutant_length}")
    if(NOT _pak_suffix_removed_length EQUAL _pak_suffix_refusal_length)
        message(FATAL_ERROR "Expected exactly one actual --exclude-suffix refusal block")
    endif()
    set(_pak_suffix_mutant_file "${CMAKE_CURRENT_BINARY_DIR}/jce_pak_old_exclude_suffix_wire.c")
    file(WRITE "${_pak_suffix_mutant_file}" "${_pak_suffix_mutant}")
    get_target_property(_pak_suffix_sources jce_pak SOURCES)
    set(_pak_suffix_owners "${_pak_suffix_sources}")
    list(FILTER _pak_suffix_owners INCLUDE REGEX "/tools/jce_pak\\.c$")
    list(LENGTH _pak_suffix_owners _pak_suffix_owner_count)
    if(NOT _pak_suffix_owner_count EQUAL 1)
        message(FATAL_ERROR "Expected one actual jce_pak translation unit")
    endif()
    list(REMOVE_ITEM _pak_suffix_sources ${_pak_suffix_owners})
    list(APPEND _pak_suffix_sources "${_pak_suffix_mutant_file}")
    add_executable(test_jce_pak_old_exclude_suffix_wire EXCLUDE_FROM_ALL ${_pak_suffix_sources})
    set_property(TARGET test_jce_pak_old_exclude_suffix_wire PROPERTY
        JCE_PAK_SUFFIX_MUTANT_SOURCE "${_pak_suffix_mutant_file}")
    # The root admits pristine host-tool headers/dependencies after tests have
    # configured. Copy the final actual recipe only after that late admission.
    function(_jce_pak_suffix_inherit_final_recipe)
        foreach(_property IN ITEMS
                C_STANDARD C_STANDARD_REQUIRED C_EXTENSIONS MSVC_RUNTIME_LIBRARY
                INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS
                LINK_LIBRARIES LINK_OPTIONS LINK_FLAGS)
            get_target_property(_value jce_pak "${_property}")
            if(NOT _value STREQUAL "_value-NOTFOUND")
                set_property(TARGET test_jce_pak_old_exclude_suffix_wire
                    PROPERTY "${_property}" "${_value}")
            endif()
        endforeach()
        get_target_property(_prerequisites jce_pak MANUALLY_ADDED_DEPENDENCIES)
        if(_prerequisites)
            add_dependencies(test_jce_pak_old_exclude_suffix_wire ${_prerequisites})
        endif()
        # Source properties are directory-scoped. Preserve their visibility in
        # the test target's directory, including the relocated complete owner.
        get_target_property(_sources jce_pak SOURCES)
        get_target_property(_mutant test_jce_pak_old_exclude_suffix_wire JCE_PAK_SUFFIX_MUTANT_SOURCE)
        foreach(_source IN LISTS _sources)
            set(_test_source "${_source}")
            if(_source STREQUAL "${CMAKE_SOURCE_DIR}/tools/jce_pak.c")
                set(_test_source "${_mutant}")
            endif()
            foreach(_property IN ITEMS
                    COMPILE_DEFINITIONS INCLUDE_DIRECTORIES COMPILE_OPTIONS
                    COMPILE_FLAGS LANGUAGE OBJECT_DEPENDS)
                get_source_file_property(_value "${_source}" TARGET_DIRECTORY jce_pak "${_property}")
                if(NOT _value STREQUAL "NOTFOUND")
                    set_source_files_properties("${_test_source}"
                        TARGET_DIRECTORY test_jce_pak_old_exclude_suffix_wire
                        PROPERTIES "${_property}" "${_value}")
                endif()
            endforeach()
        endforeach()
        # Quoted CLI headers normally resolve beside the production source.
        target_include_directories(test_jce_pak_old_exclude_suffix_wire PRIVATE "${JCE_TOOLS_DIR}")
    endfunction()
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _jce_pak_suffix_inherit_final_recipe)
    set_target_properties(test_jce_pak_old_exclude_suffix_wire PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/host-pak-suffix-controls")
    jce_configure_test_link_pool(test_jce_pak_old_exclude_suffix_wire)
    unset(_pak_suffix_source)
    unset(_pak_suffix_refusal)
    unset(_pak_suffix_mutant)
    unset(_pak_suffix_source_length)
    unset(_pak_suffix_mutant_length)
    unset(_pak_suffix_refusal_length)
    unset(_pak_suffix_removed_length)
    unset(_pak_suffix_mutant_file)
    unset(_pak_suffix_sources)
    unset(_pak_suffix_owners)
    unset(_pak_suffix_owner_count)
endif()
