# engine/CMakeLists.txt
#
# JCE engine static library.
#
# Public include root: engine/src/
# Consumers link via: target_link_libraries(MyGame PRIVATE JCE)

# ------------------------------------------------------------------ #
# Source files                                                        #
# ------------------------------------------------------------------ #

file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")

# Exclude JNI bridge from non-JNI/non-Android builds.
if(NOT JCE_PLATFORM_ANDROID AND NOT JCE_BUILD_JNI)
	list(FILTER ENGINE_SOURCES EXCLUDE REGEX ".*/bridge/.*")
endif()

# On Apple platforms, miniaudio includes AVFoundation (Objective-C).
if(APPLE)
	set_source_files_properties(
		"${CMAKE_CURRENT_SOURCE_DIR}/src/audio/miniaudio_impl.c"
		PROPERTIES LANGUAGE OBJC)
	enable_language(OBJC)
endif()

# ── JCE — Engine static library ──
add_library(JCE STATIC ${ENGINE_SOURCES})
set_target_properties(JCE PROPERTIES LINKER_LANGUAGE CXX)

# ------------------------------------------------------------------ #
# Compiler warnings                                                   #
# ------------------------------------------------------------------ #

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
	target_compile_options(JCE PRIVATE
		-Wall -Wextra -Wpedantic
		-Wconversion -Wshadow -Wdouble-promotion
		-Wno-unused-parameter)
elseif(MSVC)
	target_compile_options(JCE PRIVATE
		/W4
		/wd4100    # unreferenced formal parameter
		/wd4127    # conditional expression is constant
		/utf-8
		/Zc:preprocessor)
	target_compile_definitions(JCE PRIVATE
		_CRT_SECURE_NO_WARNINGS
		NOMINMAX
		WIN32_LEAN_AND_MEAN)
endif()

# ------------------------------------------------------------------ #
# Include directories                                                 #
# ------------------------------------------------------------------ #

target_include_directories(JCE PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/include")  # public API: <jce/core/jce_log.h>

target_include_directories(JCE PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}/src")      # internal headers

if(JCE_BUILD_JNI)
	target_include_directories(JCE PRIVATE ${JNI_INCLUDE_DIRS})
endif()

# ------------------------------------------------------------------ #
# Third-party libraries (Conan targets found by root)                #
# ------------------------------------------------------------------ #

target_link_libraries(JCE PUBLIC
	SDL3_image::SDL3_image
	sdl::sdl
	bgfx::bgfx
	flecs::flecs_static
	miniaudio::miniaudio
	Freetype::Freetype
	harfbuzz::harfbuzz
	cgltf::cgltf
	zstd::libzstd_static
	xxHash::xxhash
	mimalloc-static
	cglm::cglm
	enkits::enkits
	physfs-static
	cjson::cjson
	box2d::box2d
	Bullet::Bullet
	enet::enet
	protobuf::protobuf
	BehaviorTree::behaviortree_cpp
	rmlui::rmlui)

# Tracy: true compile-time gate.
# When OFF, we do not link Tracy at all, and headers compile to no-ops.
if(JCE_TRACY_ENABLED)
	target_link_libraries(JCE PUBLIC Tracy::TracyClient)
	target_compile_definitions(JCE PUBLIC JCE_TRACY_ENABLED=1)
else()
	target_compile_definitions(JCE PUBLIC JCE_TRACY_ENABLED=0)
endif()

# C++-only deps: kept out of the PUBLIC surface so the engine
# static library stays callable from pure C / FFI / JNI consumers.
target_link_libraries(JCE PRIVATE
	imgui::imgui)

# ── Platform-specific link options ──
if(JCE_PLATFORM_WEB)
	# Web: C-array embedding handled by caged_kingdom module
elseif(JCE_PLATFORM_ANDROID)
	target_link_libraries(JCE PUBLIC
		EGL GLESv3 android log)
elseif(JCE_PLATFORM_IOS)
	target_link_libraries(JCE PUBLIC
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
	target_link_libraries(JCE PUBLIC
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
	target_link_libraries(JCE PUBLIC psapi)
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
