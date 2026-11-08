cmake_minimum_required(VERSION 3.20)
project(SnakeSeven C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
# jce_script_cpp.hpp #errors below C++17, and the native C++ script module
# includes it.  The generated template sets only the C standard.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Locate the JCE SDK.
#   Override on the command line with:
#     cmake -DJCE_DIR=<sdk>/lib/cmake/JCE ..
#   or set the JCE_SDK_DIR environment variable (preferred).
#   The editor forwards this automatically for project builds.
if(NOT DEFINED JCE_DIR AND DEFINED ENV{JCE_SDK_DIR})
    if(EXISTS "$ENV{JCE_SDK_DIR}/lib/cmake/JCE/JCEConfig.cmake")
        set(JCE_DIR "$ENV{JCE_SDK_DIR}/lib/cmake/JCE")
    elseif(EXISTS "$ENV{JCE_SDK_DIR}/cmake/JCEConfig.cmake")
        set(JCE_DIR "$ENV{JCE_SDK_DIR}/cmake")
    endif()
endif()
find_package(JCE REQUIRED)

# Authored source requires seven languages. Portable Lua is a distinct Bundle
# consumer; a build flag cannot remove required scene modules.
option(SNAKE_SINGLE_FILE "Request a single-file product without dropping source-required VMs" OFF)
set(SNAKE_SINGLE_FILE_LANGUAGES js CACHE STRING "Legacy requested language closure; must cover the authored scene")
set(_snake_required lua js python java c cpp csharp)
if(SNAKE_SINGLE_FILE)
    foreach(_required js python java c cpp csharp)
        if(NOT "${_required}" IN_LIST SNAKE_SINGLE_FILE_LANGUAGES)
            message(FATAL_ERROR "SNAKE_SINGLE_FILE_LANGUAGES omits required authored backend '${_required}'. Use the existing portable Bundle for the separate Lua variant; no source Script is silently removed.")
        endif()
    endforeach()
endif()
set(_snake_native native/snake_body.cpp native/snake_manager.c)

add_executable(SnakeSeven src/main.c ${_snake_native})
if(NOT COMMAND jce_script_enable)
    message(FATAL_ERROR "SnakeSeven needs the installed SDK scripting helper and compiled backend roster")
endif()
jce_script_enable(SnakeSeven LANGUAGES ${_snake_required} REQUIRED
    NATIVE_FACTORIES snake_c_module snake_cpp_module)

if(_snake_native)
    # ON THE TARGET, not on the source files.
    #
    # set_source_files_properties() attaches to the SOURCE, so once the same
    # two sources are also built as shared modules below, the property
    # reaches those too -- and JCE_SCRIPT_MODULE_NO_ENTRY is exactly the
    # define that suppresses the module entry symbol.  The modules then
    # built, linked and loaded, and the engine refused them with "exports no
    # jce_cpp_script_module -- it is not a JCE script module".  Correct
    # message, and the cause was one word about scope.
    #
    # Harmless on main.c: the define only suppresses an entry a translation
    # unit would otherwise emit, and main.c declares no module.
    target_compile_definitions(SnakeSeven PRIVATE
        JCE_SCRIPT_MODULE_NO_ENTRY
        )

    # THE SAME TWO SOURCES, BUILT A SECOND TIME AS SHARED MODULES.
    #
    # Not redundant: the two builds are for two different processes.  The exe
    # above LINKS the modules (JCE_SCRIPT_MODULE_NO_ENTRY, so the two do not
    # both define the entry symbol).  The EDITOR is a different process that
    # does not link this game, so the only way a .jcec or .jcecpp class
    # reaches Play is a shared object it dlopens -- the one named in
    # jce_project.json's "script_modules".
    #
    # Built here rather than by a separate cmake invocation in native/ so
    # that one `jce.py build-project` produces both products.  It was a
    # manual step, and a manual step that only affects the EDITOR is the kind
    # nobody discovers they skipped: the shipped game runs all seven
    # languages either way, and Play quietly runs five.
    #
    # PREFIX "" because jce_project.json names the files literally and
    # CMake's default prefix is toolchain-dependent -- MSVC produces
    # snake_c.dll and MinGW libsnake_c.dll from this same line, and a
    # manifest correct under one compiler names a missing file under the
    # other.
    foreach(_mod cpp c)
        if(_mod STREQUAL "cpp")
            set(_src native/snake_body.cpp)
            set(_lib JCE::ScriptCppModule)
        else()
            set(_src native/snake_manager.c)
            set(_lib JCE::ScriptCModule)
        endif()
        if(TARGET ${_lib})
            add_library(snake_${_mod} MODULE ${_src})
            target_link_libraries(snake_${_mod} PRIVATE ${_lib})
            set_target_properties(snake_${_mod} PROPERTIES
                PREFIX ""
                LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/native/build"
                RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/native/build")
            foreach(_cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
                set_target_properties(snake_${_mod} PROPERTIES
                    LIBRARY_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_SOURCE_DIR}/native/build"
                    RUNTIME_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_SOURCE_DIR}/native/build")
            endforeach()
            add_dependencies(SnakeSeven snake_${_mod})
        else()
            message(FATAL_ERROR "Required editor native provider target ${_lib} is absent in this compiled SDK")
        endif()
    endforeach()
    # Asserted, not assumed: scripting/c's claim is that a PURE C translation
    # unit can be an entity script, and linking it as C++ would still run.
    if(TARGET snake_c)
        set_target_properties(snake_c PROPERTIES LINKER_LANGUAGE C)
    endif()
endif()
target_compile_definitions(SnakeSeven PRIVATE SDL_MAIN_USE_CALLBACKS=1)

# THIS IS A FIXED-CAMERA GAME.  The board is seen straight down and the view
# is part of the design, so the default main must not fly the camera around
# with WASD/Space -- which it otherwise does, and which also steals the very
# keys input.jcejs reads.
target_compile_definitions(SnakeSeven PRIVATE JCE_APP_AUTHORED_CAMERA_ONLY=1)
target_link_libraries(SnakeSeven PRIVATE JCE::JCE)

# THE PARITY LINE.  Without it this executable links the built-in Lua VM and
# NOTHING else: the scene loads, the Lua module runs, and every Python / Java
# / JavaScript entity is refused by name in the log.  Editor Play would run
# four languages and the shipped game one, from the same scene file -- a
# difference that shows up as "the game is missing half its behaviour" rather
# than as an error.
#
# The generated template does not call this (project.create writes a
# CMakeLists with no scripting at all), which is why it is added here by hand
# rather than inherited.
# Only the C# assembly still needs a path: a .cs script resolves to a type in
# a managed assembly, and managed code cannot be linked into a native binary.
# ONE path, named once, used by both the builder and the loader.
#
# It is pinned rather than defaulted because MSBuild's default output path
# depends on the Platform property, and CMake's environment sets it: the
# first build of this target put the assembly in bin/x64/Release/net8.0
# while this define still pointed at bin/Release/net8.0.  The build
# succeeded, the package was staged, the game ran -- and loaded the STALE
# assembly from the path nobody had written to.  A built artefact and a
# consumer that looks somewhere else is the same defect as not building it
# at all, and it is harder to see because the build log says success.
set(SNAKE_CS_OUT "${CMAKE_SOURCE_DIR}/csharp/build")
target_compile_definitions(SnakeSeven PRIVATE
    SNAKE_CS_ASSEMBLY="${SNAKE_CS_OUT}/SnakeScripts.dll")

# ...and building it is part of THIS build, not a step someone remembers.
#
# The engine keeps `dotnet build` out of CMake on purpose, so that a machine
# with a C toolchain and no .NET SDK can still build every native target it
# declares.  That reasoning does not carry over to a project whose gameplay
# is partly written in C#: here the SDK is a real dependency, and leaving the
# assembly to a manual step produces the worst possible failure -- the game
# builds, links, packages and RUNS, with one module silently absent.
#
# Measured, before this existed: renaming the C# class left a stale
# SnakeScripts.dll in the package.  The engine reported
# "no type named 'scripts/Hud.cs' in 2 loaded assemblies", the other six
# languages ran, and the HUD simply never updated.  Nothing failed.
#
# JceScriptDll is passed because the .csproj references the SDK's bridge by
# that property; an unset HintPath resolves to nothing and the compile fails
# with CS0246 on a type the file plainly uses.  The SDK publishes no variable
# for it, so it is located the same way any other SDK payload would be --
# from JCE_DIR, which find_package(JCE) has already resolved.
get_filename_component(_jce_sdk_root "${JCE_DIR}/../../.." ABSOLUTE)
find_file(SNAKE_JCESCRIPT_DLL
    NAMES JceScript.dll
    PATHS "${_jce_sdk_root}/share/jce/scripting/csharp"
    NO_DEFAULT_PATH)

find_program(SNAKE_DOTNET dotnet)
if(SNAKE_DOTNET AND SNAKE_JCESCRIPT_DLL)
    add_custom_target(SnakeScripts ALL
        COMMAND "${SNAKE_DOTNET}" build
                "${CMAKE_SOURCE_DIR}/csharp/SnakeScripts.csproj"
                -c Release
                "-p:JceScriptDll=${SNAKE_JCESCRIPT_DLL}"
                "-p:OutDir=${SNAKE_CS_OUT}/"
        BYPRODUCTS "${SNAKE_CS_OUT}/SnakeScripts.dll"
        COMMENT "Building the C# gameplay assembly (SnakeScripts)"
        VERBATIM)
    add_dependencies(SnakeSeven SnakeScripts)

    # ...and put it BESIDE THE EXE, because that is where main.c looks first
    # and where the packager stages from.  Building it to a private output
    # directory and stopping there produced the exact failure this whole
    # block exists to prevent: a fresh assembly in build/csharp/ holding the
    # new class, a STALE one beside the exe holding the old class, and a
    # game that loaded the stale one and reported
    # "no type named 'scripts/Hud.cs' in 2 loaded assemblies".  Both files
    # were real, both were named SnakeScripts.dll, and the build was green.
    add_custom_command(TARGET SnakeSeven POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${SNAKE_CS_OUT}/SnakeScripts.dll"
                "$<TARGET_FILE_DIR:SnakeSeven>"
        COMMENT "Staging the C# gameplay assembly beside the exe"
        VERBATIM)
else()
    message(FATAL_ERROR
        "Required C# gameplay assembly cannot be rebuilt (dotnet=${SNAKE_DOTNET}, bridge=${SNAKE_JCESCRIPT_DLL}); seven-language source has no complete producer. Installed backend catalog alone is not runtime/build availability.")
endif()

# A compiled roster is not a self-contained external VM payload. The public
# delivery admission/audit must separately qualify every required runtime;
# this authoring producer never changes the selected seven-language scene.

# Cooked-asset tree to pack into the embedded PAK.  build-project.bat
# scrapes jce_project.json's cooked_assets field and forwards it via
# -DJCE_PROJECT_COOKED_ASSETS=<rel>; the default mirrors the schema
# default so plain `cmake -S . -B build` works too.
if(NOT DEFINED JCE_PROJECT_COOKED_ASSETS)
    set(JCE_PROJECT_COOKED_ASSETS "resources/_cooked")
endif()
set(_cooked "${CMAKE_SOURCE_DIR}/${JCE_PROJECT_COOKED_ASSETS}")

# Raw (uncooked) asset tree, for the manual path below.  Resolved from the
# MANIFEST, not hardcoded: jce_project.json is where a project DECLARES this
# (schema field source_assets, read by the engine at jce_project.c:212 and by
# scripts/jce.py:2557), and a template that answered it independently would
# silently miss the assets of any project declaring another directory --
# which project.create does, "resources".  Note the cooked tree above relies
# on build-project forwarding -DJCE_PROJECT_COOKED_ASSETS; there is no such
# forwarder for the raw tree, so reading the manifest is the only way a plain
# `cmake -S . -B build` can honour the declaration.
#
# Order: an explicit -D wins, then the manifest, then "assets" -- the engine
# model's own default (jce_project.c:270, :281) and the directory this
# template ships.
if(NOT DEFINED JCE_PROJECT_RAW_ASSETS)
    set(JCE_PROJECT_RAW_ASSETS "assets")
    if(EXISTS "${CMAKE_SOURCE_DIR}/jce_project.json")
        file(READ "${CMAKE_SOURCE_DIR}/jce_project.json" _jce_manifest_json)
        string(JSON _jce_src_assets ERROR_VARIABLE _jce_src_err
               GET "${_jce_manifest_json}" source_assets)
        if(NOT _jce_src_err AND _jce_src_assets)
            set(JCE_PROJECT_RAW_ASSETS "${_jce_src_assets}")
        endif()
    endif()
endif()
set(_raw "${CMAKE_SOURCE_DIR}/${JCE_PROJECT_RAW_ASSETS}")

# In editor-driven builds, jce_target_embed_pak consumes the
# editor-prebuilt asset source passed through JCE_PROJECT_PREBUILT_ASSETS_C.
# Manual SDK consumers may still provide a host packer, but the packaged
# editor path does not require first-party external tools.
set(_has_prebuilt_assets OFF)
if(DEFINED JCE_PROJECT_PREBUILT_ASSETS_C AND
   EXISTS "${JCE_PROJECT_PREBUILT_ASSETS_C}")
    set(_has_prebuilt_assets ON)
endif()
if(COMMAND jce_target_embed_pak)
    if(_has_prebuilt_assets OR IS_DIRECTORY "${_cooked}")
        jce_target_embed_pak(SnakeSeven RESOURCE_DIRS "${_cooked}")
    elseif(COMMAND jce_add_pak AND IS_DIRECTORY "${_raw}")
        # No cooked tree, but the project HAS raw assets.  jce_add_pak is a
        # superset of jce_target_embed_pak: it cooks the raw tree (or packs it
        # raw with a WARNING when no cooker is installed) and delegates to
        # jce_target_embed_pak for the prebuilt case, so the editor-driven
        # paths above are unaffected.
        #
        # Without this branch, a project built outside the editor -- which the
        # comment at the top of this block says is supported -- linked an
        # empty PAK and its assets silently did not ship, behind a
        # message(STATUS) rather than a warning.  MEASURED 2026-09-21: a
        # marker string in a raw asset was ABSENT from the built exe before
        # and PRESENT after, same asset bytes and same path in both arms.
        jce_add_pak(SnakeSeven RESOURCE_DIRS "${_raw}")
    else()
        # Still reachable, and deliberately: no cooked tree AND no raw tree
        # (a freshly created project has neither), or no jce_add_pak.  The
        # engine references assets_pak_data unconditionally, so something must
        # define it.
        set(_stub "${CMAKE_CURRENT_BINARY_DIR}/jce_stub_pak.c")
        file(WRITE "${_stub}"
"#include <stddef.h>\n"
"const unsigned char assets_pak_data[1] = {0};\n"
"const size_t        assets_pak_data_size = 0;\n")
        target_sources(SnakeSeven PRIVATE "${_stub}")
        message(STATUS "SnakeSeven: no cooked assets at ${_cooked} and no raw assets at ${_raw} - linking empty PAK stub")
    endif()
else()
    set(_stub "${CMAKE_CURRENT_BINARY_DIR}/jce_stub_pak.c")
    file(WRITE "${_stub}"
"#include <stddef.h>\n"
"const unsigned char assets_pak_data[1] = {0};\n"
"const size_t        assets_pak_data_size = 0;\n")
    target_sources(SnakeSeven PRIVATE "${_stub}")
    message(STATUS "SnakeSeven: JCE asset helper unavailable - linking empty PAK stub")
endif()

# Stage runtime data alongside the exe so the template's main.c can
# read jce_project.json (for startup_scene) and load scene/material
# files from the cooked tree via host-FS APIs.
add_custom_command(TARGET SnakeSeven POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/jce_project.json"
            "$<TARGET_FILE_DIR:SnakeSeven>/jce_project.json"
    VERBATIM)

# Loose cooked-asset staging is optional: when the project ships its assets
# encrypted inside the embedded PAK, copying the plaintext cooked tree next
# to the exe would defeat the encryption (the editor passes OFF for
# encrypting builds).  Default ON preserves the historical layout.
option(JCE_PROJECT_STAGE_LOOSE_ASSETS
    "Copy the loose cooked asset tree next to the built executable" ON)
if(JCE_PROJECT_STAGE_LOOSE_ASSETS AND IS_DIRECTORY "${_cooked}")
    add_custom_command(TARGET SnakeSeven POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${_cooked}"
                "$<TARGET_FILE_DIR:SnakeSeven>/${JCE_PROJECT_COOKED_ASSETS}"
        VERBATIM)
elseif(NOT JCE_PROJECT_STAGE_LOOSE_ASSETS)
    message(STATUS "SnakeSeven: loose cooked-asset staging disabled (encrypted assets ship inside the PAK)")
endif()

# ── Embed project bundles directly into the exe ─────────────────
# JCE_PROJECT_BUNDLES is a CMake list (semicolon-separated) of source paths
# to .jbundle files.  Editor-driven builds pre-generate linkable objects or
# sources under JCE_PROJECT_PREBUILT_BUNDLE_DIR so no SDK-side wrapper tool is
# needed.  Each bundle gets baked with two externs:
#   const unsigned char bundle_<name>[];   // payload
#   const size_t        bundle_<name>_size;
# main.c then declares matching externs and mounts via
# jce_bundle_file_open_memory() — no sidecar files, true single-exe.
#
# We also write `_embedded_bundles.h` next to main.c so the template
# can `#include` the extern declarations and the loop body without
# having to know the bundle list at template-codegen time.
set(_emb_inc "${CMAKE_CURRENT_BINARY_DIR}/_embedded_bundles.h")
set(_emb_lines "/* Auto-generated by CMake -- DO NOT EDIT */\n#ifndef JCE_PROJECT_EMBEDDED_BUNDLES_H\n#define JCE_PROJECT_EMBEDDED_BUNDLES_H\n#include <stddef.h>\n")
set(_emb_extern "")
set(_emb_table  "static const struct { const char *name; const unsigned char *data; const size_t *size; } jce_project_embedded_bundles[] = {\n")
set(_emb_count 0)
if(JCE_PROJECT_BUNDLES AND COMMAND jce_target_embed_bundle)
    foreach(_bundle IN LISTS JCE_PROJECT_BUNDLES)
        if(NOT IS_ABSOLUTE "${_bundle}")
            set(_bundle "${CMAKE_SOURCE_DIR}/${_bundle}")
        endif()
        if(EXISTS "${_bundle}")
            get_filename_component(_bname "${_bundle}" NAME_WE)
            string(REGEX REPLACE "[^A-Za-z0-9_]" "_" _sym "${_bname}")
            set(_sym "bundle_${_sym}")
            jce_target_embed_bundle(SnakeSeven BUNDLE "${_bundle}" SYMBOL "${_sym}")
            string(APPEND _emb_extern "extern const unsigned char ${_sym}[];\nextern const size_t ${_sym}_size;\n")
            string(APPEND _emb_table  "    { \"${_bname}\", ${_sym}, &${_sym}_size },\n")
            math(EXPR _emb_count "${_emb_count}+1")
        else()
            message(WARNING "SnakeSeven: bundle not found, skipping embed: ${_bundle}")
        endif()
    endforeach()
endif()
string(APPEND _emb_table "    { 0, 0, 0 }\n};\n")
string(APPEND _emb_lines "${_emb_extern}${_emb_table}#define JCE_PROJECT_EMBEDDED_BUNDLE_COUNT ${_emb_count}\n#endif\n")
file(WRITE "${_emb_inc}" "${_emb_lines}")
target_include_directories(SnakeSeven PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
