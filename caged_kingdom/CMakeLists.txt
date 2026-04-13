# caged_kingdom/CMakeLists.txt
#
# "Caged Kingdom" — sandbox game project built on top of JCE engine.
#
# Standalone mode:  SDL3 callback executable  (desktop / iOS / Web)
# JNI mode:         shared library            (Android / host-JNI)

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
		set(IOS_APP_ICON   "${CMAKE_SOURCE_DIR}/scripts/macos/ios/jce_i1024.png")
		if(NOT EXISTS "${IOS_INFO_PLIST}")
			message(FATAL_ERROR "Missing iOS Info.plist: ${IOS_INFO_PLIST}")
		endif()

		set_target_properties(CagedKingdom PROPERTIES
			MACOSX_BUNDLE TRUE
			MACOSX_BUNDLE_INFO_PLIST "${IOS_INFO_PLIST}"
			MACOSX_BUNDLE_BUNDLE_NAME "CagedKingdom"
			MACOSX_BUNDLE_GUI_IDENTIFIER "com.caged_kingdom"
			MACOSX_BUNDLE_BUNDLE_VERSION "1.0"
			MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
			XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.caged_kingdom"
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
# Android does NOT need WHOLE_ARCHIVE: SDL callbacks are referenced via main.c.
#
# We use platform-specific linker flags instead of CMake's LINK_LIBRARY feature
# because transitive PUBLIC dependencies cause WHOLE_ARCHIVE/DEFAULT conflicts.
if(JCE_BUILD_JNI)
	target_link_libraries(CagedKingdom PRIVATE JCE)
	if(MSVC)
		# MSVC: /WHOLEARCHIVE forces all .obj from each .lib to be included.
		foreach(_lib jce_app jce_core)
			target_link_options(CagedKingdom PRIVATE
				"/WHOLEARCHIVE:$<TARGET_FILE:${_lib}>")
		endforeach()
	else()
		# GCC / Clang: --whole-archive / --no-whole-archive bracket.
		target_link_options(CagedKingdom PRIVATE
			"LINKER:--whole-archive"
			"$<TARGET_FILE:jce_app>"
			"$<TARGET_FILE:jce_core>"
			"LINKER:--no-whole-archive")
	endif()
else()
	target_link_libraries(CagedKingdom PRIVATE JCE)
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

install(TARGETS CagedKingdom DESTINATION bin)
