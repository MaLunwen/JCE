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
set(_ENGINE_TOOLS_INC "${CMAKE_CURRENT_SOURCE_DIR}/tools_include")

# ================================================================== #
# jce_tools_imgui — Tools-layer ImGui wrapper.                       #
# Headers live under engine/tools_include/ (NOT engine/include/),    #
# so game SDK consumers (caged_kingdom, third-party games) cannot    #
# accidentally reach Dear ImGui via <jce/...>. Only editor and the   #
# engine UI middleware opt-in by linking this INTERFACE target.      #
# ================================================================== #
add_library(jce_tools_imgui INTERFACE)
target_include_directories(jce_tools_imgui INTERFACE "${_ENGINE_TOOLS_INC}")
target_link_libraries(jce_tools_imgui INTERFACE imgui::imgui)

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

	# Collect sources from the listed directories (recursive — picks up
	# OS-specific subdirectories like platform/macos/, platform/android/).
	set(_srcs "")
	foreach(_dir IN LISTS ARG_DIRS)
		file(GLOB_RECURSE _dir_srcs CONFIGURE_DEPENDS
			"${_ENGINE_SRC}/${_dir}/*.c"
			"${_ENGINE_SRC}/${_dir}/*.cpp")
		# Skip vendored third_party trees (e.g., libhevc has its own build).
		list(FILTER _dir_srcs EXCLUDE REGEX "/third_party/")
		# Skip host-tool-only TUs that get explicitly compiled into the
		# tools/ targets (jce_cook, jce_pak, …) and pull in symbols
		# (jce_tex_compress, …) that the runtime engine does not link.
		list(FILTER _dir_srcs EXCLUDE REGEX "/jce_asset_cooker\\.c$")
		list(APPEND _srcs ${_dir_srcs})
	endforeach()

	add_library(${NAME} STATIC ${_srcs})

	if(ARG_CXX)
		set_target_properties(${NAME} PROPERTIES LINKER_LANGUAGE CXX)
	endif()

	# Public headers — every layer exposes the same public include root
	# so that #include <jce/core/jce_log.h> works from any layer.
	target_include_directories(${NAME} PUBLIC "${_ENGINE_INC}")
	# Generated headers (e.g. jce_version.h) live under the build tree.
	target_include_directories(${NAME} PUBLIC "${CMAKE_BINARY_DIR}/include")
	target_compile_definitions(${NAME} PRIVATE JCE_BUILDING_ENGINE=1)

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

	# Keep frame pointers so the crash handler can walk the call chain
	# via ucontext when a signal fires.  Overhead is negligible.
	if(NOT MSVC)
		target_compile_options(${NAME} PRIVATE -fno-omit-frame-pointer)
	endif()
endfunction()

# ================================================================== #
# L0 — Foundation (core)                                              #
# ================================================================== #

_jce_add_layer(jce_core
	DIRS      os/core
	LINK_PUBLIC
		sdl::sdl
	LINK_PRIVATE
		cjson::cjson
		mimalloc-static
		enkits::enkits
		zstd::libzstd_static
		xxHash::xxhash
		physfs-static)

# Tracy: true compile-time gate.
if(JCE_TRACY_ENABLED)
	target_link_libraries(jce_core PRIVATE Tracy::TracyClient)
	target_compile_definitions(jce_core PUBLIC JCE_TRACY_ENABLED=1)
else()
	target_compile_definitions(jce_core PUBLIC JCE_TRACY_ENABLED=0)
endif()

# ================================================================== #
# L1 — Platform                                                       #
# ================================================================== #

_jce_add_layer(jce_platform
	DIRS      os/platform
	LINK_PUBLIC
		jce_core
	LINK_PRIVATE
		SDL3_image::SDL3_image)

# ================================================================== #
# L3 — Independent subsystems (depend only on L0)                     #
# ================================================================== #

# Patented codec adapters (AAC / H.264 / H.265) — user preference.
# Default ON for release (editor needs them to import legacy assets).
# The dist variant always overrides this OFF via the JCE_PATENTED_CODECS_ENABLED
# intermediate (see root CMakeLists.txt), without mutating this preference —
# so flipping back to release naturally restores the user's chosen value.
option(JCE_ENABLE_PATENTED_CODECS
	"Compile vendored fdk-aac / OpenH264 / libhevc adapters (AAC, H.264, H.265). \
Patent-encumbered in many jurisdictions; you are responsible for licensing. \
Default ON for release; the dist variant always forces OFF for royalty-free \
redistribution." ON)

# Effective switch — the rest of this file reads only this. Set by the root
# CMakeLists.txt before this file runs (default: == JCE_ENABLE_PATENTED_CODECS;
# dist variant: forced OFF).
if(NOT DEFINED JCE_PATENTED_CODECS_ENABLED)
	set(JCE_PATENTED_CODECS_ENABLED ${JCE_ENABLE_PATENTED_CODECS})
endif()

# If the vendor sources are absent, force-disable patented codecs to keep builds working.
if(JCE_PATENTED_CODECS_ENABLED)
	set(_JCE_LIBHEVC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/middleware/video/third_party/libhevc/jce_build")
	set(_JCE_OPENH264_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/middleware/video/third_party/openh264/jce_build")
	set(_JCE_FDKAAC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/middleware/audio/third_party/fdk-aac")
	if(NOT EXISTS "${_JCE_LIBHEVC_DIR}" OR NOT EXISTS "${_JCE_OPENH264_DIR}" OR NOT EXISTS "${_JCE_FDKAAC_DIR}")
		message(WARNING "JCE: patented codecs requested, but vendor sources are missing. Disabling.")
		set(JCE_PATENTED_CODECS_ENABLED OFF)
	endif()
endif()

if(JCE_PATENTED_CODECS_ENABLED)
	message(STATUS "JCE: patented codecs ENABLED (AAC/H.264/H.265 via fdk-aac/OpenH264/libhevc)")

	# AOSP libhevc decoder (Apache-2.0, static library)
	add_subdirectory(
		"${_JCE_LIBHEVC_DIR}"
		"${CMAKE_CURRENT_BINARY_DIR}/libhevcdec")

	# Cisco openh264 decoder (BSD-2, static library — pure-C path)
	add_subdirectory(
		"${_JCE_OPENH264_DIR}"
		"${CMAKE_CURRENT_BINARY_DIR}/openh264dec")

	# Fraunhofer FDK-AAC decoder/encoder (vendored — see header note in
	# the directory). Built as static lib with default options.
	set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
	set(BUILD_PROGRAMS    OFF CACHE BOOL "" FORCE)
	set(FDK_AAC_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
	set(FDK_AAC_INSTALL_PKGCONFIG_MODULE    OFF CACHE BOOL "" FORCE)
	add_subdirectory(
		"${_JCE_FDKAAC_DIR}"
		"${CMAKE_CURRENT_BINARY_DIR}/fdk-aac")
	# Suppress strict-warnings for vendored 3rd-party.
	if(TARGET fdk-aac)
		target_compile_options(fdk-aac PRIVATE
			$<$<C_COMPILER_ID:MSVC>:/w>
			$<$<NOT:$<C_COMPILER_ID:MSVC>>:-w>)
	endif()
else()
	message(STATUS "JCE: patented codecs DISABLED (royalty-free path: Opus + AV1)")
endif()

_jce_add_layer(jce_video
	DIRS      middleware/video
	LINK_PUBLIC jce_core
	LINK_PRIVATE dav1d::dav1d libvpx::libvpx webm::webm Opus::opus)

# S3: compile the AVX2 YUV conversion TU with SIMD flags on x86 only.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i686|i386")
    set_source_files_properties(
        "${_ENGINE_SRC}/middleware/video/jce_yuv_convert_avx2.c"
        PROPERTIES COMPILE_OPTIONS
            "$<IF:$<C_COMPILER_ID:MSVC>,/arch:AVX2,-mavx2>")
else()
    set_source_files_properties(
        "${_ENGINE_SRC}/middleware/video/jce_yuv_convert_avx2.c"
        PROPERTIES HEADER_FILE_ONLY TRUE)
endif()

if(JCE_PATENTED_CODECS_ENABLED)
	target_link_libraries(jce_video PRIVATE fdk-aac openh264dec libhevcdec)
	target_compile_definitions(jce_video PUBLIC JCE_ENABLE_PATENTED_CODECS=1)
endif()

_jce_add_layer(jce_audio
	DIRS      middleware/audio
	LINK_PUBLIC  jce_core
	LINK_PRIVATE miniaudio::miniaudio jce_video flecs::flecs_static
	            Opus::opus Ogg::ogg)

if(JCE_PATENTED_CODECS_ENABLED)
	target_link_libraries(jce_audio PRIVATE fdk-aac)
	target_compile_definitions(jce_audio PUBLIC JCE_ENABLE_PATENTED_CODECS=1)
endif()

# On Apple platforms, miniaudio includes AVFoundation (Objective-C).
if(APPLE)
	set_source_files_properties(
		"${_ENGINE_SRC}/middleware/audio/jce_miniaudio_impl.c"
		PROPERTIES LANGUAGE OBJC)
	enable_language(OBJC)
endif()

_jce_add_layer(jce_physics
	DIRS      middleware/physics
	LINK_PUBLIC  jce_core
	LINK_PRIVATE box2d::box2d Bullet::Bullet v-hacd::v-hacd
	CXX)

# VHACD is a heavy single-header library; silence its warnings on the one
# TU that pulls in its implementation.
set_source_files_properties(
	"${CMAKE_CURRENT_SOURCE_DIR}/src/middleware/physics/jce_collider_cook.cpp"
	PROPERTIES COMPILE_OPTIONS
	"$<$<CXX_COMPILER_ID:MSVC>:/w>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-w>")

_jce_add_layer(jce_animation
	DIRS      middleware/animation
	LINK_PUBLIC  jce_core
	LINK_PRIVATE ozz-animation::ozz-animation bgfx::bgfx
	CXX)

_jce_add_layer(jce_ai
	DIRS      middleware/ai
	LINK_PUBLIC  jce_core
	LINK_PRIVATE BT::behaviortree_cpp recastnavigation::recastnavigation
	            flecs::flecs_static
	CXX)

# Gameplay scripting VM (Phase 0 keystone). Generic Lua host: depends ONLY on
# lua + core. Scene/ECS access is provided by the runtime via a JceScriptHost
# callback bridge (mirrors how jce_ai's BT actions are registered by the
# runtime) so this layer never depends upward on scene.
_jce_add_layer(jce_script
	DIRS      middleware/script
	LINK_PUBLIC  jce_core
	LINK_PRIVATE lua::lua)

_jce_add_layer(jce_net
	DIRS      middleware/net
	LINK_PUBLIC  jce_core
	LINK_PRIVATE enet::enet protobuf::protobuf flecs::flecs_static
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
	DIRS      middleware/scene
	LINK_PUBLIC  jce_core
	LINK_PRIVATE
		jce_animation
		jce_video
		flecs::flecs_static
		cjson::cjson
		bgfx::bgfx
		cgltf::cgltf
	CXX)

_jce_add_layer(jce_world
	DIRS      middleware/world
	LINK_PUBLIC  jce_core
	# GAS attribute replication (jce_gas_replication.c) registers a packed
	# replica component with the net snapshot substrate.  jce_net links only
	# jce_core (no jce_world), so this private edge introduces no cycle.
	LINK_PRIVATE flecs::flecs_static bgfx::bgfx jce_net)

_jce_add_layer(jce_save
	DIRS      middleware/save
	LINK_PUBLIC  jce_core
	# jce_save_providers.c serializes/rehydrates the scene through the shared
	# component-JSON schema (jce_scene_save_json / jce_scene_load_json), so the
	# save layer links the scene layer.  jce_scene is declared above and does
	# not link jce_save, so there is no dependency cycle.
	LINK_PRIVATE jce_scene)

# ================================================================== #
# L3 — Graphics + Render (depends on L0, L1, animation)              #
# ================================================================== #

_jce_add_layer(jce_renderer
	DIRS      renderer
	LINK_PUBLIC
		jce_core
		jce_platform
		jce_animation
	LINK_PRIVATE
		bgfx::bgfx
		SDL3_image::SDL3_image
		Freetype::Freetype
		harfbuzz::harfbuzz
		flecs::flecs_static
		cgltf::cgltf
	CXX)

# RenderDoc capture: compile-time gate (PRIVATE — only jce_gpu_capture.c reads
# it). No link: the in-app API is resolved at runtime from the injected module
# via the jce_library shared-object wrapper. Forced OFF for dist (root
# CMakeLists), so shipped games carry zero RenderDoc code.
if(JCE_RENDERDOC_ENABLED)
	target_compile_definitions(jce_renderer PRIVATE JCE_RENDERDOC_ENABLED=1)
else()
	target_compile_definitions(jce_renderer PRIVATE JCE_RENDERDOC_ENABLED=0)
endif()

# Late-bound dependency: jce_scene_renderer.c lives in middleware/scene
# but consumes the renderer low-level API.  Declared here (after both
# targets exist) to keep the link order valid.
target_link_libraries(jce_scene PRIVATE jce_renderer)

# P2-weather-decals-tod: jce_scene_renderer.c drives the weather overlay and
# the time-of-day curve (both in middleware/world / jce_world) from authored
# scene settings.  Late-bound link because jce_world is declared after the
# jce_scene layer above.
target_link_libraries(jce_scene PRIVATE jce_world)

# P3-C.4 follow-up: jce_scene_update reconciles JceClothComponent against
# the cloth runtime (create/destroy handles, update wind).  Late-bound
# because jce_physics is declared before jce_scene above.
target_link_libraries(jce_scene PRIVATE jce_physics)

# P3-C.4: jce_physics installs an observer on the Render Pipeline Asset
# (renderer layer) to wire the cloth/soft-body HW gate.  Late-bound link
# because jce_physics is declared before jce_renderer.
target_link_libraries(jce_physics PRIVATE jce_renderer)

# ================================================================== #
# L3 — UI (depends on core, graphics, platform, audio, app)          #
# ================================================================== #

# NOTE: jce_ui links jce_application (forward-declared below) for config access,
#       and jce_audio for volume settings.  These are intentional design
#       choices to keep settings UI self-contained.
_jce_add_layer(jce_ui
	DIRS      middleware/ui
	LINK_PUBLIC
		jce_core
		jce_renderer
		jce_platform
		jce_audio
	LINK_PRIVATE
		rmlui::rmlui
		bgfx::bgfx
		jce_tools_imgui
	CXX)

# ================================================================== #
# L2 — Resource (bridge layer — loads assets for all subsystems)      #
# ================================================================== #

_jce_add_layer(jce_resource
	DIRS      resource
	LINK_PUBLIC
		jce_core
		jce_renderer
		jce_audio
		jce_animation
		jce_scene
	LINK_PRIVATE
		SDL3_image::SDL3_image
		cgltf::cgltf
		miniaudio::miniaudio
		zstd::libzstd_static
		xxHash::xxhash
		cjson::cjson
		bgfx::bgfx
		assimp::assimp
		meshoptimizer::meshoptimizer
	CXX)

# P0-build-bundles-cook: the in-process bundle packer (jce_bundle_pack.c,
# already in this layer) cooks textures/audio at build time by calling
# jce_cook_texture / jce_cook_audio. Those live in jce_asset_cooker.c —
# excluded from the layer glob by _jce_add_layer (it's a host-tool TU that
# pulls in jce_tex_compress / jce_tex_encode). Add them back here, together
# with the bimg block encoder, so "Build Bundles" produces BC/ASTC textures
# in-process (no jce_cook subprocess). This mirrors the existing precedent of
# assimp/meshoptimizer being linked here purely for the build-time mesh→GLB
# converter (jce_bundle_mesh_convert.cpp).
#
# NOTE (audit Round-3 P3-E): the shipping runtime ALSO reaches a subset of
# these symbols — jce_texture_from_cooked() / jce_texture_decode_cooked_rgba8()
# in jce_renderer call jce_tex_cooked_pixel_size() (size validation) and
# jce_tex_decode_to_rgba8() to load cooked textures at runtime. They resolve in
# the monolithic editor/SDK link; a *standalone* jce_renderer link would need
# these symbols too. A future cleanup may relocate the pure-C sizing helper into
# the renderer layer to restore strict layering.
target_sources(jce_resource PRIVATE
	"${JCE_ENGINE_SRC_DIR}/resource/jce_asset_cooker.c"
	"${JCE_TOOLS_DIR}/jce_tex_compress.c"
	"${JCE_TOOLS_DIR}/jce_tex_encode.cpp")
target_link_libraries(jce_resource PRIVATE
	bgfx::bimg_encode
	bgfx::bimg
	bgfx::bx)

# ================================================================== #
# L5 — Application orchestrator                                       #
# ================================================================== #

set(_APP_DIRS application runtime)

_jce_add_layer(jce_application
	DIRS      ${_APP_DIRS}
	LINK_PUBLIC
		jce_core
		jce_platform
		jce_renderer
		jce_audio
		jce_physics
		jce_resource
		jce_scene
		jce_script
	LINK_PRIVATE
		bgfx::bgfx)

if(JCE_BUILD_JNI)
	# jce_jni_bridge.c lives in jce_platform (os/platform/) and is guarded by
	# JCE_BUILD_JNI — both the definition and JNI headers must be on that target.
	target_include_directories(jce_platform PRIVATE ${JNI_INCLUDE_DIRS})
	target_compile_definitions(jce_platform PRIVATE JCE_BUILD_JNI=1)
	target_include_directories(jce_application PRIVATE ${JNI_INCLUDE_DIRS})
	target_compile_definitions(jce_application PRIVATE JCE_BUILD_JNI=1)
endif()

# jce_ui needs jce_application for config access (circular at layer level,
# but not at symbol level — ui reads config, app orchestrates ui).
target_link_libraries(jce_ui PUBLIC jce_application)

# ================================================================== #
# JCE — INTERFACE facade (backward-compatible link target)            #
# ================================================================== #

add_library(JCE INTERFACE)

# Expose the CMake-generated version header to all JCE consumers.
target_include_directories(JCE INTERFACE "${CMAKE_BINARY_DIR}/include")

target_link_libraries(JCE INTERFACE
	jce_core
	jce_platform
	jce_audio
	jce_video
	jce_physics
	jce_animation
	jce_ai
	jce_net
	jce_world
	jce_save
	jce_scene
	jce_script
	jce_renderer
	jce_ui
	jce_resource
	jce_application)

# ── Platform-specific link options (on the facade) ──
if(JCE_PLATFORM_WEB)
	# Web: C-array embedding handled by caged_kingdom module.
	# Emscripten ABI link flags on the facade so in-tree apps (and the
	# find_package SDK, which mirrors these) get a working WebGL2/GLES3 link.
	target_link_options(JCE INTERFACE
		-sWASM=1 -sALLOW_MEMORY_GROWTH=1 -sMAX_WEBGL_VERSION=2 -sMIN_WEBGL_VERSION=2
		-sFULL_ES3=1 -sSTACK_SIZE=1048576 -sMALLOC=emmalloc "$<$<CONFIG:Debug>:-sASSERTIONS=1>")
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

# Linux + macOS: dladdr() is in libdl on glibc; on macOS it's in libSystem
# but linking ${CMAKE_DL_LIBS} is the portable spelling.
if(JCE_PLATFORM_LINUX OR JCE_PLATFORM_MACOS)
	target_link_libraries(JCE INTERFACE ${CMAKE_DL_LIBS})
	# For backtrace_symbols() to produce useful output the executable
	# must export its dynamic symbol table.
	target_link_options(JCE INTERFACE
		"$<$<PLATFORM_ID:Linux>:-rdynamic>")
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

# ================================================================== #
# SDK link-time shims (MSVC only).                                    #
#                                                                      #
# A tiny static lib that back-ports MSVC STL vectorised helpers       #
# missing from older toolchain CRTs.  Kept OUT of layer GLOBs and     #
# fat libs; the installed JCEConfig.cmake links it only for consumer  #
# MSVC versions that need the compatibility symbol.                   #
# ================================================================== #
if(WIN32 AND MSVC)
	add_library(jce_msvc_stl_shims STATIC
		"${_ENGINE_SRC}/sdk_shims/jce_msvc_stl_shims.cpp")
	set_target_properties(jce_msvc_stl_shims PROPERTIES LINKER_LANGUAGE CXX)
	target_compile_definitions(jce_msvc_stl_shims PRIVATE JCE_BUILDING_ENGINE=1)
endif()

# ================================================================== #
# SDK packaging (Phase 3) — produce a redistributable JCE SDK.        #
# Opt-in via -DJCE_ENABLE_SDK_INSTALL=ON. End-users then consume the  #
# install tree with find_package(JCE REQUIRED) — no Conan required.   #
# ================================================================== #
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/JCESDKInstall.cmake")
jce_register_sdk_install()
