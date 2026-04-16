# engine/CMakeLists.txt
#
# JCE engine — per-layer static libraries with dependency isolation.
#
# Public include root: engine/include/  →  <jce/core/jce_log.h>
# Consumers link via:  target_link_libraries(MyGame PRIVATE JCE)
#
# JCE is an INTERFACE library that transitively links all sub-layers,
# so downstream targets (caged_kingdom, editor) need no changes.

set(_ENGINE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(_ENGINE_INC "${CMAKE_CURRENT_SOURCE_DIR}/include")

# ================================================================== #
# Helper: create one engine sub-layer                                 #
# ================================================================== #
#
# _jce_add_layer(<name>
#     DIRS      dir1 [dir2 ...]       # subdirectories under engine/src/
#     [LINK_PUBLIC  lib ...]           # PUBLIC  link dependencies
#     [LINK_PRIVATE lib ...]           # PRIVATE link dependencies
#     [CXX]                            # set LINKER_LANGUAGE CXX
# )
function(_jce_add_layer NAME)
	cmake_parse_arguments(ARG "CXX" "" "DIRS;LINK_PUBLIC;LINK_PRIVATE" ${ARGN})

	# Collect sources from the listed directories.
	set(_srcs "")
	foreach(_dir IN LISTS ARG_DIRS)
		file(GLOB _dir_srcs CONFIGURE_DEPENDS
			"${_ENGINE_SRC}/${_dir}/*.c"
			"${_ENGINE_SRC}/${_dir}/*.cpp")
		list(APPEND _srcs ${_dir_srcs})
	endforeach()

	add_library(${NAME} STATIC ${_srcs})

	if(ARG_CXX)
		set_target_properties(${NAME} PROPERTIES LINKER_LANGUAGE CXX)
	endif()

	# Public headers — every layer exposes the same public include root
	# so that #include <jce/core/jce_log.h> works from any layer.
	target_include_directories(${NAME} PUBLIC "${_ENGINE_INC}")

	# Private: only the layer's own source directories.
	foreach(_dir IN LISTS ARG_DIRS)
		target_include_directories(${NAME} PRIVATE "${_ENGINE_SRC}/${_dir}")
	endforeach()

	# Cross-layer internal headers: a layer that links another layer
	# inherits the public include dir (engine/include), but sometimes
	# needs access to peer src/ directories (e.g., resource loading
	# graphics types from engine/src/graphics/).  The internal src root
	# is exposed PRIVATE to each layer for this purpose.
	target_include_directories(${NAME} PRIVATE "${_ENGINE_SRC}")

	# Link dependencies.
	if(ARG_LINK_PUBLIC)
		target_link_libraries(${NAME} PUBLIC ${ARG_LINK_PUBLIC})
	endif()
	if(ARG_LINK_PRIVATE)
		target_link_libraries(${NAME} PRIVATE ${ARG_LINK_PRIVATE})
	endif()

	jce_target_warnings(${NAME} STRICT)
endfunction()

# ================================================================== #
# L0 — Foundation (core)                                              #
# ================================================================== #

_jce_add_layer(jce_core
	DIRS      core
	LINK_PUBLIC
		sdl::sdl
		cglm::cglm
		mimalloc-static
		zstd::libzstd_static
		xxHash::xxhash
		physfs-static
		cjson::cjson
		enkits::enkits)

# Tracy: true compile-time gate.
if(JCE_TRACY_ENABLED)
	target_link_libraries(jce_core PUBLIC Tracy::TracyClient)
	target_compile_definitions(jce_core PUBLIC JCE_TRACY_ENABLED=1)
else()
	target_compile_definitions(jce_core PUBLIC JCE_TRACY_ENABLED=0)
endif()

# ================================================================== #
# L1 — Platform                                                       #
# ================================================================== #

_jce_add_layer(jce_platform
	DIRS      platform
	LINK_PUBLIC
		jce_core
	LINK_PRIVATE
		SDL3_image::SDL3_image)

# ================================================================== #
# L3 — Independent subsystems (depend only on L0)                     #
# ================================================================== #

_jce_add_layer(jce_audio
	DIRS      audio
	LINK_PUBLIC  jce_core
	LINK_PRIVATE miniaudio::miniaudio)

# On Apple platforms, miniaudio includes AVFoundation (Objective-C).
if(APPLE)
	set_source_files_properties(
		"${_ENGINE_SRC}/audio/miniaudio_impl.c"
		PROPERTIES LANGUAGE OBJC)
	enable_language(OBJC)
endif()

# ------------------------------------------------------------------ #
# jce_video — MPEG-1 video via vendored pl_mpeg (single-header,      #
# MIT).  No external codec dependency.                               #
# ------------------------------------------------------------------ #
_jce_add_layer(jce_video
	DIRS      video
	LINK_PUBLIC  jce_core)

# pl_mpeg triggers a wide range of conversion/shadow warnings that
# the engine's STRICT profile would otherwise flag.  Relax them for
# the vendored translation units only — the wrapper jce_video.c still
# builds under the strict profile.
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
	set_source_files_properties(
		"${_ENGINE_SRC}/video/pl_mpeg_impl.c"
		"${_ENGINE_SRC}/video/jce_video.c"
		PROPERTIES COMPILE_OPTIONS
			"-Wno-conversion;-Wno-sign-conversion;-Wno-shadow;-Wno-double-promotion;-Wno-pedantic;-Wno-unused-function")
elseif(MSVC)
	set_source_files_properties(
		"${_ENGINE_SRC}/video/pl_mpeg_impl.c"
		"${_ENGINE_SRC}/video/jce_video.c"
		PROPERTIES COMPILE_OPTIONS
			"/wd4244;/wd4245;/wd4267;/wd4456;/wd4457;/wd4701;/wd4189;/wd4242")
endif()

_jce_add_layer(jce_physics
	DIRS      physics
	LINK_PUBLIC  jce_core
	LINK_PRIVATE box2d::box2d Bullet::Bullet
	CXX)

_jce_add_layer(jce_animation
	DIRS      animation
	LINK_PUBLIC  jce_core
	LINK_PRIVATE ozz-animation::ozz-animation bgfx::bgfx
	CXX)

_jce_add_layer(jce_ai
	DIRS      ai
	LINK_PUBLIC  jce_core
	LINK_PRIVATE BT::behaviortree_cpp
	CXX)

_jce_add_layer(jce_net
	DIRS      net
	LINK_PUBLIC  jce_core
	LINK_PRIVATE enet::enet protobuf::protobuf
	CXX)

# Protobuf code generation (net layer owns the proto files).
set(JCE_PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/proto")
set(JCE_PROTO_OUT "${CMAKE_CURRENT_BINARY_DIR}/proto_gen")
file(MAKE_DIRECTORY "${JCE_PROTO_OUT}")
protobuf_generate(
	TARGET jce_net
	LANGUAGE cpp
	IMPORT_DIRS "${JCE_PROTO_DIR}"
	PROTOS "${JCE_PROTO_DIR}/jce_net_messages.proto"
	PROTOC_OUT_DIR "${JCE_PROTO_OUT}")
target_include_directories(jce_net PRIVATE "${JCE_PROTO_OUT}")

_jce_add_layer(jce_scene
	DIRS      scene
	LINK_PUBLIC  jce_core
	LINK_PRIVATE flecs::flecs_static)

# ================================================================== #
# L3 — Graphics + Render (depends on L0, L1, animation)              #
# ================================================================== #

_jce_add_layer(jce_graphics
	DIRS      graphics render
	LINK_PUBLIC
		jce_core
		jce_platform
		jce_animation
	LINK_PUBLIC
		bgfx::bgfx
	LINK_PRIVATE
		SDL3_image::SDL3_image
		Freetype::Freetype
		harfbuzz::harfbuzz
	CXX)

# ================================================================== #
# L3 — UI (depends on core, graphics, platform, audio, app)          #
# ================================================================== #

# NOTE: jce_ui links jce_app (forward-declared below) for config access,
#       and jce_audio for volume settings.  These are intentional design
#       choices to keep settings UI self-contained.
_jce_add_layer(jce_ui
	DIRS      ui
	LINK_PUBLIC
		jce_core
		jce_graphics
		jce_platform
		jce_audio
	LINK_PRIVATE
		rmlui::rmlui
	CXX)

# ================================================================== #
# L2 — Resource (bridge layer — loads assets for all subsystems)      #
# ================================================================== #

_jce_add_layer(jce_resource
	DIRS      resource
	LINK_PUBLIC
		jce_core
		jce_graphics
		jce_audio
		jce_animation
		jce_scene
	LINK_PRIVATE
		SDL3_image::SDL3_image
		cgltf::cgltf
		miniaudio::miniaudio
	CXX)

# ================================================================== #
# L5 — Application orchestrator                                       #
# ================================================================== #

set(_APP_DIRS app streaming)
if(JCE_PLATFORM_ANDROID OR JCE_BUILD_JNI)
	list(APPEND _APP_DIRS bridge)
endif()

_jce_add_layer(jce_app
	DIRS      ${_APP_DIRS}
	LINK_PUBLIC
		jce_core
		jce_platform
		jce_graphics
		jce_audio
		jce_resource)

if(JCE_BUILD_JNI)
	target_include_directories(jce_app PRIVATE ${JNI_INCLUDE_DIRS})
	target_compile_definitions(jce_app PRIVATE JCE_BUILD_JNI=1)
endif()

# jce_ui needs jce_app for config access (circular at layer level,
# but not at symbol level — ui reads config, app orchestrates ui).
target_link_libraries(jce_ui PUBLIC jce_app)

# ================================================================== #
# JCE — INTERFACE facade (backward-compatible link target)            #
# ================================================================== #

add_library(JCE INTERFACE)

target_link_libraries(JCE INTERFACE
	jce_core
	jce_platform
	jce_audio
	jce_video
	jce_physics
	jce_animation
	jce_ai
	jce_net
	jce_scene
	jce_graphics
	jce_ui
	jce_resource
	jce_app)

# ── Platform-specific link options (on the facade) ──
if(JCE_PLATFORM_WEB)
	# Web: C-array embedding handled by caged_kingdom module
elseif(JCE_PLATFORM_ANDROID)
	target_link_libraries(JCE INTERFACE
		EGL GLESv3 android log)
elseif(JCE_PLATFORM_IOS)
	target_link_libraries(JCE INTERFACE
		"-framework Metal"
		"-framework QuartzCore"
		"-framework UIKit"
		"-framework Foundation"
		"-framework CoreGraphics"
		"-framework CoreText"
		"-framework AVFoundation"
		"-framework AudioToolbox"
		"-framework CoreAudio"
		"-framework GameController"
		"-framework CoreHaptics"
		"-framework CoreMotion"
		"-framework CoreServices")
elseif(JCE_PLATFORM_MACOS)
	target_link_libraries(JCE INTERFACE
		"-framework Metal"
		"-framework QuartzCore"
		"-framework Cocoa"
		"-framework IOKit"
		"-framework CoreVideo"
		"-framework CoreAudio"
		"-framework AudioToolbox"
		"-framework Carbon"
		"-framework ForceFeedback"
		"-framework GameController"
		"-framework CoreHaptics")
elseif(WIN32)
	target_link_libraries(JCE INTERFACE psapi)
endif()

# ------------------------------------------------------------------ #
# Dependencies                                                        #
# ------------------------------------------------------------------ #

# ------------------------------------------------------------------ #
# Linux static C++ linking helper                                     #
# ------------------------------------------------------------------ #

if(JCE_PLATFORM_LINUX)
	execute_process(
		COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=libstdc++.a
		OUTPUT_VARIABLE _STATIC_STDCXX OUTPUT_STRIP_TRAILING_WHITESPACE)
	if(NOT EXISTS "${_STATIC_STDCXX}")
		message(FATAL_ERROR
			"libstdc++.a not found (looked for: ${_STATIC_STDCXX}).\n"
			"Install: sudo apt install libstdc++-11-dev")
	endif()

	execute_process(
		COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=libgcc.a
		OUTPUT_VARIABLE _STATIC_LIBGCC OUTPUT_STRIP_TRAILING_WHITESPACE)
	execute_process(
		COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=libgcc_eh.a
		OUTPUT_VARIABLE _STATIC_LIBGCC_EH OUTPUT_STRIP_TRAILING_WHITESPACE)

	set(_STATIC_SHIM_DIR "${CMAKE_BINARY_DIR}/_static_shims")
	file(MAKE_DIRECTORY "${_STATIC_SHIM_DIR}")
	file(WRITE "${_STATIC_SHIM_DIR}/libstdc++.so"
		"/* Linker script — redirect to static archive */\n"
		"GROUP ( ${_STATIC_STDCXX} )\n")
	if(EXISTS "${_STATIC_LIBGCC}" AND EXISTS "${_STATIC_LIBGCC_EH}")
		file(WRITE "${_STATIC_SHIM_DIR}/libgcc_s.so"
			"/* Linker script */\n"
			"GROUP ( ${_STATIC_LIBGCC} ${_STATIC_LIBGCC_EH} )\n")
		file(WRITE "${_STATIC_SHIM_DIR}/libgcc_s.so.1"
			"/* Linker script */\n"
			"GROUP ( ${_STATIC_LIBGCC} ${_STATIC_LIBGCC_EH} )\n")
	endif()

	# Export shim dir for executables that link JCE.
	set(_STATIC_SHIM_DIR "${_STATIC_SHIM_DIR}" PARENT_SCOPE)
endif()
