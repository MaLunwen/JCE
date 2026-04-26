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

find_package(assimp REQUIRED)
target_link_libraries(JCE_Editor PRIVATE
	JCE assimp::assimp imgui::imgui
	cglm::cglm cjson::cjson SDL3_image::SDL3_image bgfx::bgfx)

target_include_directories(JCE_Editor PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}/src"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/core"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/ui"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/io")

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

if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|arm64|aarch64")
    set(_jce_arch_str "ARM64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64|x64")
    set(_jce_arch_str "x64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86|i[3-6]86")
    set(_jce_arch_str "x86")
else()
    set(_jce_arch_str "${CMAKE_SYSTEM_PROCESSOR}")
endif()

set(_jce_platform_str "${_jce_os_str} ${_jce_arch_str}")

set(_jce_config_str "Unknown")
if(CMAKE_BUILD_TYPE)
    set(_jce_config_str "${CMAKE_BUILD_TYPE}")
endif()

target_compile_definitions(JCE_Editor PRIVATE
    SDL_MAIN_USE_CALLBACKS=1
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

set_target_properties(JCE_Editor PROPERTIES
	OUTPUT_NAME jce_editor
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${JCE_BUILD_VARIANT}"
	ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${JCE_BUILD_VARIANT}")

# Dist builds on Windows: GUI subsystem (no console window).
if(WIN32 AND JCE_BUILD_VARIANT STREQUAL "dist")
	set_target_properties(JCE_Editor PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

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
if(JCE_BUILD_VARIANT STREQUAL "dist")
	add_custom_command(TARGET JCE_Editor POST_BUILD
		COMMAND ${CMAKE_COMMAND}
			-DFILE=$<TARGET_FILE:JCE_Editor>
			-DOUT=$<TARGET_FILE_DIR:JCE_Editor>/jce_editor_sha256.txt
			-P "${CMAKE_SOURCE_DIR}/tools/write_sha256.cmake"
		COMMENT "Computing SHA-256 hash for jce_editor"
		VERBATIM)
endif()
