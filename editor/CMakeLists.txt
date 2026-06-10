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

# Auto-normalize i18n JSON before each editor build (no-op when files
# are already normalized).  See top-level CMakeLists.txt -> jce_i18n_auto.
if(TARGET jce_i18n_auto)
	add_dependencies(JCE_Editor jce_i18n_auto)
endif()

# Editor is C++17 (per project policy: engine = C99, editor = C++17).
set_target_properties(JCE_Editor PROPERTIES
	CXX_STANDARD 17
	CXX_STANDARD_REQUIRED YES
	CXX_EXTENSIONS NO)

target_link_libraries(JCE_Editor PRIVATE
	JCE jce_tools_imgui
	cjson::cjson SDL3_image::SDL3_image bgfx::bgfx)

target_include_directories(JCE_Editor PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}/src")

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
