# editor/CMakeLists.txt
#
# JCE_Editor executable (desktop-only).
# Depends on JCE static library for core engine functionality.

# Guard: editor is desktop-only — skip for mobile/web/JNI builds.
if(JCE_PLATFORM_ANDROID OR JCE_PLATFORM_IOS
   OR JCE_PLATFORM_WEB OR JCE_BUILD_JNI)
	return()
endif()

# ── Editor sources (C++) ──
file(GLOB_RECURSE EDITOR_SOURCES CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_executable(JCE_Editor ${EDITOR_SOURCES})

# Optional linker map for the binary-size report (tools/gen_binsize_report.py).
if(COMMAND jce_emit_link_map)
	jce_emit_link_map(JCE_Editor)
endif()
# Verify the symbols selected by the linker: bimg embeds a slower dav1d with
# the same names. Keep the editor a JCE-only consumer and fail wrong bindings.
if(MSVC)
    set(_codec_map "${CMAKE_BINARY_DIR}/reports/JCE_Editor.map")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/reports")
    target_link_options(JCE_Editor PRIVATE "/MAP:${_codec_map}")
    add_custom_command(TARGET JCE_Editor POST_BUILD
        COMMAND "${CMAKE_COMMAND}" "-DMAP_FILE=${_codec_map}"
            -P "${PROJECT_SOURCE_DIR}/engine/cmake/JCEVerifyCodecLink.cmake"
        VERBATIM)
endif()

# Auto-normalize i18n JSON before each editor build (no-op when files
# are already normalized).  See top-level CMakeLists.txt -> jce_i18n_auto.
if(TARGET jce_i18n_auto)
	add_dependencies(JCE_Editor jce_i18n_auto)
endif()

# Editor is C++20 (per project policy: engine = C99, editor = C++20).
set_target_properties(JCE_Editor PROPERTIES
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED YES
	CXX_EXTENSIONS NO)

# The editor is a pure L7 engine consumer: it reaches the engine only through
# the JCE facade + the sanctioned jce_tools_imgui (ImGui) target. It links no
# third-party engine dependency directly — cjson/SDL3_image/bgfx were dead
# direct links (editor sources reference them only in comments; verified by
# tools/lint/check_editor_consumer_purity.py). Removed per architecture
# audit R-D41 (link-boundary: JCE_Editor). If a real need for a backend
# symbol appears, expose it through a JCE public API instead of relinking.
target_link_libraries(JCE_Editor PRIVATE
	JCE jce_tools_imgui)

target_include_directories(JCE_Editor PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}/src")

# ------------------------------------------------------------------ #
#  Script backends in editor Play                                     #
#                                                                     #
#  MEASURED BEFORE THIS EXISTED (2026-08-16, the Elemental Serenity    #
#  scene with four scripted entities): editor Play ran ONE of four     #
#  languages.  The engine registers only the built-in Lua VM, every    #
#  other backend registers itself from a call the application makes,   #
#  and the editor made none — so a .py or .java attached to an entity  #
#  was refused by name in Play and ran perfectly in the game build.    #
#  The editor being WEAKER than the shipped runtime is the packaging   #
#  asymmetry this repo's rules exist to prevent, in reverse.           #
#                                                                     #
#  These are FIRST-PARTY sibling targets, not third-party backends:    #
#  scripting/ is a peer of engine/ and editor/, and                    #
#  check_editor_consumer_purity.py bans engine-internal THIRD-PARTY    #
#  headers (SDL, bgfx, flecs, ...) so the editor cannot paper over a   #
#  missing public API.  Linking a language runtime is not that; there  #
#  is no engine API that could stand in for "this process contains a   #
#  CPython", and the alternative is an editor that cannot author the   #
#  projects the engine can run.                                        #
#                                                                     #
#  All are OPTIONAL: absent targets simply leave the editor on Lua,    #
#  which is exactly where it was.                                      #
#                                                                      #
#  THE CPP BACKEND IS HERE TOO, AND THIS COMMENT USED TO SAY IT COULD  #
#  NOT BE.  The reasoning was that a cpp script is a class compiled    #
#  into the GAME executable, which the editor does not link — true,    #
#  and it is an argument about where the CLASSES come from, not about  #
#  where the VM comes from.  The VM belongs in this process; the       #
#  classes arrive at run time from the project's own module, named in  #
#  jce_project.json and loaded with jce_script_vm_cpp_load_library().  #
#  See editor/src/core/jce_editor_script_backends.cpp.                 #
# ------------------------------------------------------------------ #
# ------------------------------------------------------------------ #
#  SCRIPTING IN EDITOR PLAY — the same call a shipped game makes.     #
#                                                                     #
#  This was four hand-written if(TARGET jce_script_vm_<lang>) blocks   #
#  plus four register() blocks in jce_editor_script_backends.cpp, and  #
#  they sat on the editor-versus-shipped parity boundary: a language    #
#  added to scripting/ worked in a shipped executable and was silently  #
#  refused in Play, visible only by pressing Play.  Two mechanisms for  #
#  one contract is how that divergence stays possible.                  #
#                                                                       #
#  jce_script_enable() links whatever this build has, bakes each         #
#  backend's runtime inputs, and generates the registration shim.  The   #
#  editor now registers through the SAME GENERATED CODE as a game, so    #
#  the parity is structural instead of maintained.                       #
#                                                                        #
#  The editor's two better behaviours were not dropped, they were moved  #
#  INTO the shim and are now everyone's: each runtime input is resolved  #
#  from the environment first, and is checked to exist BEFORE the        #
#  backend registers, with wording that says the backend is linked and   #
#  the path is wrong rather than "this language does not work".          #
# ------------------------------------------------------------------ #
option(JCE_EDITOR_STANDALONE
    "Build a native single editor executable with embedded scripting runtimes" OFF)
if(JCE_EDITOR_STANDALONE)
    if(NOT JCE_PLATFORM_WINDOWS OR NOT MSVC)
        message(FATAL_ERROR "JCE_EDITOR_STANDALONE currently requires Windows/MSVC")
    endif()
    if(NOT CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "MultiThreaded" OR
       CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "DLL")
        message(FATAL_ERROR
            "Standalone editor requires static CRT for the entire dependency graph; "
            "use scripts/jce.py editor --standalone --variant dist")
    endif()
    # Explicit shipping contract: Lua is built into the engine; these three
    # adapters link native libraries. Managed runtimes are separate builds.
    # The ScriptApi import library is unused by this executable; a native
    # project module supplies its own call-down ABI. Verify actual PE imports.
    jce_script_enable(JCE_Editor LANGUAGES c cpp js REQUIRED NO_STAGE)
    # Carry distribution notices in the native file, without sidecar dependencies.
    enable_language(RC)
    set(_jce_standalone_notices "${CMAKE_CURRENT_BINARY_DIR}/standalone_notices.rc")
    file(GENERATE OUTPUT "${_jce_standalone_notices}" CONTENT
        "#pragma code_page(65001)\nJCE_LICENSE RCDATA \"${PROJECT_SOURCE_DIR}/LICENSE\"\nJCE_THIRD_PARTY_LICENSES RCDATA \"${PROJECT_SOURCE_DIR}/THIRD_PARTY_LICENSES.md\"\n")
    target_sources(JCE_Editor PRIVATE "${_jce_standalone_notices}")
    set_source_files_properties("${_jce_standalone_notices}" PROPERTIES
        OBJECT_DEPENDS "${PROJECT_SOURCE_DIR}/LICENSE;${PROJECT_SOURCE_DIR}/THIRD_PARTY_LICENSES.md")

else()
    jce_script_enable(JCE_Editor)
endif()

# The compiled com.jce.script classes are built by scripting/java and NOTHING
# LINKS THEM, so the build edge has to be explicit here.  Without it they are
# produced only when somebody happens to build "all", and the symptom is a
# green editor build followed by "no script VM claims .java" in Play.
if(TARGET jce_script_java_classes)
	add_dependencies(JCE_Editor jce_script_java_classes)
endif()
if(TARGET jce_script_java)
	# Loaded BY the JVM, not linked: build-order only. jce_script_enable stages
	# the file itself.
	add_dependencies(JCE_Editor jce_script_java)
endif()

# Resolve a human-readable OS + arch string for the About dialog.
if(JCE_PLATFORM_WINDOWS)
    set(_jce_os_str "Windows")
elseif(JCE_PLATFORM_MACOS)
    set(_jce_os_str "macOS")
elseif(JCE_PLATFORM_LINUX)
    set(_jce_os_str "Linux")
else()
    set(_jce_os_str "${CMAKE_SYSTEM_NAME}")
endif()

# Target architecture.  On MSVC prefer the compiler's TARGET arch
# (CMAKE_CXX_COMPILER_ARCHITECTURE_ID = X86/x64/ARM64) over
# CMAKE_SYSTEM_PROCESSOR: Conan leaves the latter at the host value "AMD64" for
# an x86 build, which would mislabel a 32-bit editor as x64 in the About dialog.
set(_jce_arch_id "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}")
if(NOT _jce_arch_id)
    set(_jce_arch_id "${CMAKE_SYSTEM_PROCESSOR}")
endif()
string(TOLOWER "${_jce_arch_id}" _jce_arch_lc)
if(_jce_arch_lc MATCHES "arm64|aarch64")
    set(_jce_arch_str "ARM64")
elseif(_jce_arch_lc MATCHES "x64|amd64|x86_64")
    set(_jce_arch_str "x64")
elseif(_jce_arch_lc MATCHES "^(x86|i[3-6]86)$")
    set(_jce_arch_str "x86")
elseif(_jce_arch_lc MATCHES "^arm")
    set(_jce_arch_str "ARM")
else()
    set(_jce_arch_str "${_jce_arch_id}")
endif()

# Bitness from pointer width (32-bit vs 64-bit).
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_jce_bits_str "64-bit")
else()
    set(_jce_bits_str "32-bit")
endif()

# Platform row in the About dialog, e.g. "Windows x86 (32-bit)".
set(_jce_platform_str "${_jce_os_str} ${_jce_arch_str} (${_jce_bits_str})")

set(_jce_config_str "Unknown")
if(CMAKE_BUILD_TYPE)
    set(_jce_config_str "${CMAKE_BUILD_TYPE}")
endif()

target_compile_definitions(JCE_Editor PRIVATE
    JCE_BUILD_VARIANT_STR="${JCE_BUILD_VARIANT}"
    JCE_PLATFORM_STR="${_jce_platform_str}")

# ------------------------------------------------------------------ #
# Embedded PAK blob (editor_assets.pak)                               #
# ------------------------------------------------------------------ #

if(MSVC)
	# MSVC: link the COFF .obj produced by jce_pak.
	target_link_libraries(JCE_Editor PRIVATE "${EDITOR_PAK_OBJ_FILE}")
	set_source_files_properties("${EDITOR_PAK_OBJ_FILE}"
		PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
else()
	# GCC / Clang: .incbin ASM wrapper.
	set(_EDITOR_ASM_EMBED "${CMAKE_BINARY_DIR}/editor_assets_embed.S")
	set(PAK_FILE_PATH "${EDITOR_PAK_FILE}")
	configure_file("${CMAKE_SOURCE_DIR}/tools/assets_embed.S.in"
	               "${_EDITOR_ASM_EMBED}" @ONLY)
	enable_language(ASM)
	set_source_files_properties("${_EDITOR_ASM_EMBED}"
		PROPERTIES LANGUAGE ASM GENERATED TRUE)
	target_sources(JCE_Editor PRIVATE "${_EDITOR_ASM_EMBED}")
endif()

add_dependencies(JCE_Editor PackEditorAssets)

# ── Compiler warnings ──
jce_target_warnings(JCE_Editor)

# Keep frame pointers so the crash handler can walk the editor call chain.
if(NOT MSVC)
	target_compile_options(JCE_Editor PRIVATE -fno-omit-frame-pointer)
endif()

set_target_properties(JCE_Editor PROPERTIES
	OUTPUT_NAME jce_editor
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${JCE_BUILD_VARIANT}"
	ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${JCE_BUILD_VARIANT}")

# Dist builds on Windows: GUI subsystem (no console window).
if(WIN32 AND JCE_BUILD_VARIANT STREQUAL "dist")
	set_target_properties(JCE_Editor PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

# NOTE: 32-bit Windows builds are linked /LARGEADDRESSAWARE via a project-wide
# rule in the top-level CMakeLists.txt (see "large-address-aware by default").

# ── Remove MSVC-generated .exp/.lib ──
if(MSVC)
	add_custom_command(TARGET JCE_Editor POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E rm -f
		        "$<TARGET_FILE_DIR:JCE_Editor>/$<TARGET_FILE_BASE_NAME:JCE_Editor>.exp"
		        "$<TARGET_FILE_DIR:JCE_Editor>/$<TARGET_FILE_BASE_NAME:JCE_Editor>.lib"
		COMMENT "Removing JCE_Editor .exp/.lib"
		VERBATIM)
endif()

# ── Linux static C++ linking ──
if(JCE_PLATFORM_LINUX)
	set_target_properties(JCE_Editor PROPERTIES LINKER_LANGUAGE CXX)
	if(DEFINED _STATIC_SHIM_DIR)
		target_link_options(JCE_Editor PRIVATE
			"-L${_STATIC_SHIM_DIR}"
			-static-libstdc++ -static-libgcc)
	endif()
endif()

# ── Dist SHA-256 hash ──
# The dist binary's SHA-256 is the per-artifact integrity/identity record
# (shown in the About dialog and copied by package-editor.bat).  Different
# architectures have different binaries (different SHAs) by design.
if(JCE_BUILD_VARIANT STREQUAL "dist")
	add_custom_command(TARGET JCE_Editor POST_BUILD
		COMMAND ${CMAKE_COMMAND}
			-DFILE=$<TARGET_FILE:JCE_Editor>
			-DOUT=$<TARGET_FILE_DIR:JCE_Editor>/jce_editor_sha256.txt
			-P "${CMAKE_SOURCE_DIR}/tools/write_sha256.cmake"
		COMMENT "Computing SHA-256 hash for jce_editor"
		VERBATIM)
endif()
