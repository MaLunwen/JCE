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

jce_add_unit_test(test_jce_bundle_pack
    SOURCES test_jce_bundle_pack.c
    LINK    jce_core jce_resource)
if(TARGET cjson::cjson)
    target_link_libraries(test_jce_bundle_pack PRIVATE cjson::cjson)
endif()

jce_add_unit_test(test_jce_bundle_loader
    SOURCES test_jce_bundle_loader.c
    LINK    jce_core jce_resource)
if(TARGET xxHash::xxhash)
    target_link_libraries(test_jce_bundle_loader PRIVATE xxHash::xxhash)
endif()

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
        LABELS         "unit"
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
            add_dependencies(jce_tests ${_csn_tool})
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
