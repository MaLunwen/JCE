# caged_kingdom/CMakeLists.txt
#
# "Caged Kingdom" — sandbox game project built on top of JCE engine.
#
# Two build modes:
#
#   1. In-tree:  added via add_subdirectory() from the JCE root build.
#                Inherits JCE_BUILD_VARIANT, GAME_PAK_FILE, etc.; supports
#                Android / JNI / Web / iOS variants and embedded PAK blobs.
#
#   2. Standalone (SDK consumer mode):  configured directly via
#                `cmake -S caged_kingdom -B build -G Ninja
#                  -DJCE_DIR=<sdk>/lib/cmake/JCE`.
#                Uses find_package(JCE).  When the SDK shipped the host
#                tools (jce_cook/jce_pak, via JCE_ENABLE_SDK_INSTALL),
#                jce_add_pak() cooks+packs the game's raw assets into a
#                REAL embedded PAK — matching the in-tree pipeline.  If no
#                packer shipped, it falls back to a stub empty-PAK TU so
#                the engine still links (link-surface-only dogfood).

# ------------------------------------------------------------------ #
# Standalone branch                                                   #
# ------------------------------------------------------------------ #

if(NOT DEFINED JCE_BUILD_VARIANT)
	cmake_minimum_required(VERSION 3.20)
	project(CagedKingdom C)

	find_package(JCE REQUIRED)

	file(GLOB_RECURSE CK_SOURCES CONFIGURE_DEPENDS
		"${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")

	add_executable(CagedKingdom ${CK_SOURCES})
	set_target_properties(CagedKingdom PROPERTIES OUTPUT_NAME caged_kingdom)

	target_compile_definitions(CagedKingdom PRIVATE SDL_MAIN_USE_CALLBACKS=1)
	target_include_directories(CagedKingdom PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/src")
	target_link_libraries(CagedKingdom PRIVATE JCE::JCE)

	# ---- Game asset PAK ------------------------------------------- #
	# Prefer the SDK's turnkey jce_add_pak(): it cooks raw assets
	# (PNG→RGBA8, WAV/OGG→PCM) via jce_cook when the SDK shipped it,
	# then packs+embeds the cooked tree — matching the in-tree
	# cook→pack→embed pipeline so a no-editor consumer gets a REAL PAK,
	# not a raw/stub one.  jce_add_pak() resolves internally:
	#   editor-prebuilt assets → cook (if jce_cook) → raw pack (+warn).
	# Fall back to a stub blob only when no host packer shipped at all
	# (link-surface-only dogfood), so the engine still links.
	set(_ck_res "${CMAKE_CURRENT_SOURCE_DIR}/resources/assets")
	set(_ck_ui  "${CMAKE_CURRENT_SOURCE_DIR}/ui")
	set(_ck_dirs)
	if(IS_DIRECTORY "${_ck_res}")
		list(APPEND _ck_dirs "${_ck_res}")
	endif()
	if(IS_DIRECTORY "${_ck_ui}")
		list(APPEND _ck_dirs "${_ck_ui}")
	endif()

	# Editor-driven builds feed pre-cooked assets via
	# JCE_PROJECT_PREBUILT_ASSETS_*; jce_add_pak() consumes those without
	# needing a host packer on PATH.
	set(_ck_have_prebuilt OFF)
	if((DEFINED JCE_PROJECT_PREBUILT_ASSETS_OBJ AND EXISTS "${JCE_PROJECT_PREBUILT_ASSETS_OBJ}") OR
	   (DEFINED JCE_PROJECT_PREBUILT_ASSETS_ASM AND EXISTS "${JCE_PROJECT_PREBUILT_ASSETS_ASM}") OR
	   (DEFINED JCE_PROJECT_PREBUILT_ASSETS_C   AND EXISTS "${JCE_PROJECT_PREBUILT_ASSETS_C}"))
		set(_ck_have_prebuilt ON)
	endif()

	if(COMMAND jce_add_pak AND
	   (_ck_have_prebuilt OR (JCE_PAK_EXECUTABLE AND _ck_dirs)))
		jce_add_pak(CagedKingdom
			RESOURCE_DIRS ${_ck_dirs})
	else()
		set(_stub "${CMAKE_CURRENT_BINARY_DIR}/jce_stub_pak.c")
		file(WRITE "${_stub}"
"#include <stddef.h>\n"
"const unsigned char assets_pak_data[1] = {0};\n"
"const size_t        assets_pak_data_size = 0;\n")
		target_sources(CagedKingdom PRIVATE "${_stub}")
	endif()

	if(WIN32 AND CMAKE_BUILD_TYPE STREQUAL "Dist")
		set_target_properties(CagedKingdom PROPERTIES WIN32_EXECUTABLE TRUE)
	endif()

	install(TARGETS CagedKingdom DESTINATION bin)
	return()
endif()

# ------------------------------------------------------------------ #
# In-tree branch (original behaviour)                                 #
# ------------------------------------------------------------------ #

# Source files                                                        #
# ------------------------------------------------------------------ #

file(GLOB_RECURSE CK_SOURCES CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")

# ── Target type ──
if(JCE_PLATFORM_ANDROID OR JCE_BUILD_JNI)
	# JNI bridge lives in engine; we just build a shared lib.
	add_library(CagedKingdom SHARED ${CK_SOURCES})
	if(JCE_PLATFORM_ANDROID)
		# Android Java side loads System.loadLibrary("JCE") → libJCE.so
		set_target_properties(CagedKingdom PROPERTIES OUTPUT_NAME JCE)
	elseif(JCE_BUILD_JNI)
		# Host JNI: JceRuntime loads "jce" → jce.dll / libjce.so
		set_target_properties(CagedKingdom PROPERTIES
			OUTPUT_NAME jce
			RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${JCE_BUILD_VARIANT}"
			LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${JCE_BUILD_VARIANT}"
			ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${JCE_BUILD_VARIANT}")
	endif()
else()
	add_executable(CagedKingdom ${CK_SOURCES})

	set_target_properties(CagedKingdom PROPERTIES
		OUTPUT_NAME caged_kingdom
		RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${JCE_BUILD_VARIANT}"
		ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${JCE_BUILD_VARIANT}")

	# Dist builds on Windows: GUI subsystem (no console window).
	if(WIN32 AND JCE_BUILD_VARIANT STREQUAL "dist")
		set_target_properties(CagedKingdom PROPERTIES WIN32_EXECUTABLE TRUE)
	endif()

	# ── iOS bundle properties ──
	if(JCE_PLATFORM_IOS)
		set(IOS_INFO_PLIST "${CMAKE_SOURCE_DIR}/scripts/macos/ios/Info.plist")
		set(IOS_APP_ICON   "${CMAKE_SOURCE_DIR}/scripts/macos/ios/ck_i1024.png")
		if(NOT EXISTS "${IOS_INFO_PLIST}")
			message(FATAL_ERROR "Missing iOS Info.plist: ${IOS_INFO_PLIST}")
		endif()

		set_target_properties(CagedKingdom PROPERTIES
			MACOSX_BUNDLE TRUE
			MACOSX_BUNDLE_INFO_PLIST "${IOS_INFO_PLIST}"
			MACOSX_BUNDLE_BUNDLE_NAME "CagedKingdom"
			MACOSX_BUNDLE_GUI_IDENTIFIER "com.cagedKingdom"
			MACOSX_BUNDLE_BUNDLE_VERSION "1.0"
			MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
			XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.cagedKingdom"
			XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
			XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "4C98CBUY4F"
			XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2")

		if(EXISTS "${IOS_APP_ICON}")
			target_sources(CagedKingdom PRIVATE "${IOS_APP_ICON}")
			set_source_files_properties("${IOS_APP_ICON}" PROPERTIES
				MACOSX_PACKAGE_LOCATION "Resources")
		endif()
	endif()
endif()

# ------------------------------------------------------------------ #
# Compile definitions                                                 #
# ------------------------------------------------------------------ #

# SDL_MAIN_USE_CALLBACKS only for standalone executables.
if(NOT JCE_BUILD_JNI)
	target_compile_definitions(CagedKingdom PRIVATE SDL_MAIN_USE_CALLBACKS=1)
endif()

# ------------------------------------------------------------------ #
# Include directories & linking                                       #
# ------------------------------------------------------------------ #

target_include_directories(CagedKingdom PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}/src")

# When building a shared library for host JNI, we must force the linker to
# include ALL symbols from the JCE static archives — otherwise unreferenced JNI
# entry points (Java_com_jce_JceRuntime_*) are silently discarded and Java gets
# UnsatisfiedLinkError at runtime.
# Android does not need WHOLE_ARCHIVE for most symbols, but SDL_main must be
# force-included via -u because Java calls it via dlsym, not C code.
#
# We use platform-specific linker flags instead of CMake's LINK_LIBRARY feature
# because transitive PUBLIC dependencies cause WHOLE_ARCHIVE/DEFAULT conflicts.
if(JCE_BUILD_JNI)
	target_link_libraries(CagedKingdom PRIVATE JCE)
	if(MSVC)
		# MSVC: /WHOLEARCHIVE forces all .obj from each .lib to be included.
		# jce_platform must be included: jce_jni_bridge.c lives there.
		foreach(_lib jce_application jce_platform jce_core)
			target_link_options(CagedKingdom PRIVATE
				"/WHOLEARCHIVE:$<TARGET_FILE:${_lib}>")
		endforeach()
	elseif(APPLE)
		# macOS ld64 errors on duplicate symbols, unlike GNU ld which silently picks one.
		# jce_core and jce_application share jce_config.c, so force-loading both causes
		# duplicate '_jce_config_*' errors. Only jce_platform needs -force_load because
		# jce_jni_bridge.c (Java_com_jce_JceRuntime_*) is unreferenced by C code and
		# would otherwise be silently dropped. The other libs are pulled in via the
		# normal target_link_libraries(CagedKingdom PRIVATE JCE) link above.
		target_link_options(CagedKingdom PRIVATE
			"-Wl,-force_load,$<TARGET_FILE:jce_platform>")
	else()
		# GCC (Linux): --whole-archive / --no-whole-archive bracket.
		# jce_platform must be included: jce_jni_bridge.c lives there.
		target_link_options(CagedKingdom PRIVATE
			"LINKER:--whole-archive"
			"$<TARGET_FILE:jce_application>"
			"$<TARGET_FILE:jce_platform>"
			"$<TARGET_FILE:jce_core>"
			"LINKER:--no-whole-archive")
	endif()
else()
	target_link_libraries(CagedKingdom PRIVATE JCE)
	# Android: SDL_main lives in jce_application but nothing in C references it
	# (Java calls it via dlsym).  Force the linker to include the symbol so
	# nativeRunMain() can find "SDL_main" in libJCE.so.
	if(JCE_PLATFORM_ANDROID)
		target_link_options(CagedKingdom PRIVATE "-Wl,-u,SDL_main")
	endif()
endif()

# ------------------------------------------------------------------ #
# Web — Emscripten link options                                       #
# ------------------------------------------------------------------ #

if(JCE_PLATFORM_WEB)
	set(_SHELL "${CMAKE_BINARY_DIR}/shell.html")
	file(WRITE "${_SHELL}" [=[<!doctype html>
<html><head><meta charset="utf-8"><title>Caged Kingdom</title>
<style>
html,body{margin:0;padding:0;overflow:hidden;width:100%;height:100%;background:#000}
canvas{display:block;width:100%;height:100%;border:0;padding:0}
</style></head><body>
<canvas id="canvas" oncontextmenu="event.preventDefault()" tabindex="-1"></canvas>
<script>
var Module={canvas:document.getElementById('canvas')};
document.addEventListener('click',function r(){
  var ctx=typeof _ma_device__AudioContext!=='undefined'?_ma_device__AudioContext
         :typeof AL!=='undefined'&&AL.currentCtx?AL.currentCtx.audioCtx:null;
  if(ctx)ctx.resume();
  document.removeEventListener('click',r);
},true);
</script>
{{{ SCRIPT }}}
</body></html>
]=])

	set_target_properties(CagedKingdom PROPERTIES SUFFIX ".html")
	target_link_options(CagedKingdom PRIVATE
		-sWASM=1
		-sALLOW_MEMORY_GROWTH=1
		-sMAX_WEBGL_VERSION=2
		-sMIN_WEBGL_VERSION=2
		-sFULL_ES3=1
		-sSTACK_SIZE=1048576
		-sMALLOC=emmalloc
		-sASSERTIONS=1
		"SHELL:--shell-file ${_SHELL}")
endif()

# ------------------------------------------------------------------ #
# Linux — static C++ runtime                                          #
# ------------------------------------------------------------------ #

if(JCE_PLATFORM_LINUX)
	set_target_properties(CagedKingdom PROPERTIES LINKER_LANGUAGE CXX)
	if(DEFINED _STATIC_SHIM_DIR)
		target_link_options(CagedKingdom PRIVATE
			"-L${_STATIC_SHIM_DIR}"
			-static-libstdc++ -static-libgcc)
	endif()
endif()

# ------------------------------------------------------------------ #
# Embedded PAK blob (game_assets.pak)                                 #
# ------------------------------------------------------------------ #

if(JCE_PLATFORM_WEB)
	# Web: PAK is preloaded into the Emscripten virtual FS at /game_assets.pak.
	# The runtime opens it with pak_open_file("/game_assets.pak").
	target_link_options(CagedKingdom PRIVATE
		"SHELL:--preload-file ${GAME_PAK_FILE}@/game_assets.pak")
	set_property(TARGET CagedKingdom APPEND PROPERTY
		LINK_DEPENDS "${GAME_PAK_FILE}")
elseif(JCE_PLATFORM_ANDROID OR JCE_BUILD_JNI)
	# Android: PAK shipped as APK asset, loaded at runtime via SDL_IOFromFile.
	# JNI:     PAK shipped alongside the native lib in the fat JAR, extracted
	#          and loaded at runtime via pak_open_file().  This avoids embedding
	#          a ~290 MB blob inside every per-platform shared library, keeping
	#          a single shared copy of game_assets.pak across all architectures.
elseif(MSVC)
	# MSVC: link the COFF .obj produced by jce_pak.
	target_link_libraries(CagedKingdom PRIVATE "${GAME_PAK_OBJ_FILE}")
	set_source_files_properties("${GAME_PAK_OBJ_FILE}"
		PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
else()
	# GCC / Clang: .incbin ASM wrapper.
	set(_GAME_ASM_EMBED "${CMAKE_BINARY_DIR}/game_assets_embed.S")
	set(PAK_FILE_PATH "${GAME_PAK_FILE}")
	configure_file("${CMAKE_SOURCE_DIR}/tools/assets_embed.S.in"
	               "${_GAME_ASM_EMBED}" @ONLY)
	enable_language(ASM)
	set_source_files_properties("${_GAME_ASM_EMBED}"
		PROPERTIES LANGUAGE ASM GENERATED TRUE)
	target_sources(CagedKingdom PRIVATE "${_GAME_ASM_EMBED}")
endif()

add_dependencies(CagedKingdom JCE PackGameAssets)

# ------------------------------------------------------------------ #
# Remove .exp/.lib artifacts (MSVC)                                   #
# ------------------------------------------------------------------ #

if(MSVC AND NOT JCE_BUILD_JNI)
	add_custom_command(TARGET CagedKingdom POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E rm -f
		        "$<TARGET_FILE_DIR:CagedKingdom>/$<TARGET_FILE_BASE_NAME:CagedKingdom>.exp"
		        "$<TARGET_FILE_DIR:CagedKingdom>/$<TARGET_FILE_BASE_NAME:CagedKingdom>.lib"
		COMMENT "Removing CagedKingdom .exp/.lib"
		VERBATIM)
endif()

# ------------------------------------------------------------------ #
# Build size report                                                   #
# ------------------------------------------------------------------ #

add_custom_command(TARGET CagedKingdom POST_BUILD
	COMMAND "${CMAKE_COMMAND}"
	        -DEXE_FILE=$<TARGET_FILE:CagedKingdom>
	        -DPAK_FILE=${GAME_PAK_FILE}
	        -DMANIFEST_FILE=${GAME_MANIFEST_FILE}
	        -DDEP_LOCS_FILE=${CMAKE_BINARY_DIR}/_dep_locs_$<CONFIG>.cmake
	        -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
	        -DCONFIG=$<CONFIG>
	        -P "${REPORT_SCRIPT}"
	COMMENT "Build size report"
	VERBATIM)

# ------------------------------------------------------------------ #
# Dist SHA-256 hash                                                   #
# ------------------------------------------------------------------ #

if(JCE_BUILD_VARIANT STREQUAL "dist")
	add_custom_command(TARGET CagedKingdom POST_BUILD
		COMMAND ${CMAKE_COMMAND}
			-DFILE=$<TARGET_FILE:CagedKingdom>
			-DOUT=$<TARGET_FILE_DIR:CagedKingdom>/caged_kingdom_sha256.txt
			-P "${CMAKE_SOURCE_DIR}/tools/write_sha256.cmake"
		COMMENT "Computing SHA-256 hash for caged_kingdom"
		VERBATIM)
endif()

# ------------------------------------------------------------------ #
# Install                                                             #
# ------------------------------------------------------------------ #

# The in-tree caged_kingdom binary is a sample — it must NOT end up in
# the JCE SDK install tree (that would bloat <sdk>/bin/ with a 100 MB+
# executable that's not part of the SDK contract).  Standalone-mode
# builds (the SDK-consumer dogfood) still install via the early return
# branch above.
if(NOT JCE_ENABLE_SDK_INSTALL)
	install(TARGETS CagedKingdom DESTINATION bin)
endif()
