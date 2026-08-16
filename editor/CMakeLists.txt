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

# Editor is C++20 (per project policy: engine = C99, editor = C++20).
set_target_properties(JCE_Editor PROPERTIES
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED YES
	CXX_EXTENSIONS NO)

# The editor is a pure L7 engine consumer: it reaches the engine only through
# the JCE facade + the sanctioned jce_tools_imgui (ImGui) target. It links no
# third-party engine dependency directly — cjson/SDL3_image/bgfx were dead
# direct links (editor sources reference them only in comments; verified by
# scripts/lint/check_editor_consumer_purity.py). Removed per architecture
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
if(TARGET jce_script_vm_python)
	target_link_libraries(JCE_Editor PRIVATE jce_script_vm_python)
	target_compile_definitions(JCE_Editor PRIVATE
		JCE_EDITOR_HAVE_SCRIPT_PYTHON=1
		JCE_EDITOR_SCRIPT_PYTHON_PACKAGE_DIR="${JCE_SCRIPT_PYTHON_PACKAGE_DIR}")
endif()

if(TARGET jce_script_vm_java)
	# The JDK, DERIVED from the one place the configure already found it, for
	# the same reason tools/gpu_task derives it: two probes can disagree, and
	# then the jni.h the backend compiled against and the JVM it starts are
	# different JDKs — an UnsatisfiedLinkError with no obvious cause.
	get_filename_component(_ed_java_home "${JCE_JNI_INCLUDE_DIR}" DIRECTORY)
	if(WIN32)
		set(_ed_java_jvm "${_ed_java_home}/bin/server/jvm.dll")
	elseif(APPLE)
		set(_ed_java_jvm "${_ed_java_home}/lib/server/libjvm.dylib")
	else()
		set(_ed_java_jvm "${_ed_java_home}/lib/server/libjvm.so")
	endif()

	find_package(Python3 COMPONENTS Interpreter QUIET)
	if(NOT Python3_Interpreter_FOUND)
		message(WARNING
			"JCE_Editor: Java scripting NOT wired into Play — the scripting "
			"classes are compiled by scripting/java/build_java.py and there "
			"is no Python 3 interpreter.")
	else()
		# The classes the JVM loads BY CLASS PATH.  Nothing links them, so the
		# edge must be add_dependencies: without it they are built only when
		# somebody happens to build "all", and the symptom is a green editor
		# build followed by "no script VM claims .java" in Play.
		set(_ed_java_classes "${CMAKE_CURRENT_BINARY_DIR}/java_classes")
		file(GLOB_RECURSE _ed_java_srcs CONFIGURE_DEPENDS
			"${CMAKE_SOURCE_DIR}/scripting/java/src/main/java/*.java")
		add_custom_command(
			OUTPUT  "${_ed_java_classes}/.stamp"
			COMMAND "${Python3_EXECUTABLE}"
			        "${CMAKE_SOURCE_DIR}/scripting/java/build_java.py"
			        --out "${_ed_java_classes}" --no-jar
			COMMAND "${CMAKE_COMMAND}" -E touch "${_ed_java_classes}/.stamp"
			DEPENDS ${_ed_java_srcs}
			COMMENT "Compiling the Java scripting classes for editor Play"
			VERBATIM)
		add_custom_target(jce_editor_java_classes
			DEPENDS "${_ed_java_classes}/.stamp")

		target_link_libraries(JCE_Editor PRIVATE jce_script_vm_java)
		# jce_script_java is loaded BY the JVM, not linked: build-order only.
		add_dependencies(JCE_Editor jce_editor_java_classes jce_script_java)
		target_compile_definitions(JCE_Editor PRIVATE
			JCE_EDITOR_HAVE_SCRIPT_JAVA=1)
		# file(GENERATE) for the shim path: $<TARGET_FILE:...> is a generator
		# expression and cannot go through target_compile_definitions here.
		file(GENERATE
			OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/jce_editor_java_paths.gen.h"
			CONTENT "/* GENERATED by editor/CMakeLists.txt. */
#ifndef JCE_EDITOR_JAVA_PATHS_GEN_H
#define JCE_EDITOR_JAVA_PATHS_GEN_H
#define JCE_EDITOR_SCRIPT_JAVA_JVM_LIBRARY \"${_ed_java_jvm}\"
#define JCE_EDITOR_SCRIPT_JAVA_CLASS_PATH  \"${_ed_java_classes}/classes\"
#define JCE_EDITOR_SCRIPT_JAVA_SCRIPT_LIB  \"$<TARGET_FILE:jce_script_java>\"
#endif
")
		target_include_directories(JCE_Editor PRIVATE
			"${CMAKE_CURRENT_BINARY_DIR}/generated")
	endif()
	unset(_ed_java_home)
	unset(_ed_java_jvm)
	unset(_ed_java_srcs)
	unset(_ed_java_classes)
endif()

# ------------------------------------------------------------------ #
#  C++ scripting in editor Play                                       #
#                                                                     #
#  The VM only.  A cpp script CLASS lives in a module the PROJECT     #
#  builds; the editor loads it at run time from the paths in that      #
#  project's jce_project.json ("script_modules").  Nothing about the   #
#  project is known at configure time and nothing here tries to be.    #
#                                                                     #
#  jce_script_vm_cpp needs no toolchain and no runtime — it is a       #
#  registry and eighteen forwarders in C99 — so unlike Python and      #
#  Java it is present whenever scripting/ was added at all.            #
# ------------------------------------------------------------------ #
if(TARGET jce_script_vm_cpp)
	target_link_libraries(JCE_Editor PRIVATE jce_script_vm_cpp)
	target_compile_definitions(JCE_Editor PRIVATE JCE_EDITOR_HAVE_SCRIPT_CPP=1)
	message(STATUS
		"JCE_Editor: C++ scripting wired into Play (project modules come from "
		"jce_project.json \"script_modules\").")
else()
	# NOT SILENT, and the reason is a measured failure: add_subdirectory()
	# ordering put editor/ above scripting/ once, every if(TARGET) here read
	# FALSE, the configure was green, and Play linked no backend at all.  An
	# if(TARGET) that is lying looks exactly like one that is right, so both
	# branches say something.
	message(WARNING
		"JCE_Editor: C++ scripting NOT wired into Play — there is no "
		"jce_script_vm_cpp target at this point in the configure. If "
		"scripting/ IS being built, this is the add_subdirectory ordering "
		"trap: scripting/ must come BEFORE editor/ in the root CMakeLists.")
endif()

# ------------------------------------------------------------------ #
#  C scripting in editor Play                                         #
#                                                                     #
#  A SEPARATE if(), not a clause of the one above, because "c" is a    #
#  separate registered LANGUAGE: an editor that linked the cpp VM and  #
#  not this one would print `editor Play can run: ... cpp` and refuse  #
#  a .jcec by extension, which is precisely the answer this language   #
#  was added to stop giving.                                          #
#                                                                     #
#  The CLASSES are the same classes — one native registry serves both  #
#  languages — so there is nothing extra to load: a project module     #
#  already reaches the editor through jce_project.json                 #
#  ("script_modules"), whichever of the two languages wrote it.        #
# ------------------------------------------------------------------ #
if(TARGET jce_script_vm_c)
	target_link_libraries(JCE_Editor PRIVATE jce_script_vm_c)
	target_compile_definitions(JCE_Editor PRIVATE JCE_EDITOR_HAVE_SCRIPT_C=1)
	message(STATUS
		"JCE_Editor: C scripting wired into Play (.jcec; classes come from "
		"the same project modules the cpp VM reads).")
else()
	# Both branches speak, for the reason the cpp block above records.
	message(WARNING
		"JCE_Editor: C scripting NOT wired into Play — there is no "
		"jce_script_vm_c target at this point in the configure. A .jcec "
		"script will resolve to no language in Play while still running in a "
		"game build that links the backend.")
endif()

# jce_script_api is a SHARED library and BOTH bindings call it by name at run
# time; a project's cpp script module links it too, and the loader resolves a
# module's dependencies from the EXECUTABLE's directory, not the module's.  A
# missing DLL is a loader failure that prints nothing, so stage it beside
# jce_editor.exe whenever any backend is in.
if(TARGET jce_script_api AND
   (TARGET jce_script_vm_python OR TARGET jce_script_vm_java
    OR TARGET jce_script_vm_cpp))
	add_custom_command(TARGET JCE_Editor POST_BUILD
		COMMAND "${CMAKE_COMMAND}" -E copy_if_different
			"$<TARGET_FILE:jce_script_api>" "$<TARGET_FILE_DIR:JCE_Editor>"
		VERBATIM)
endif()
if(TARGET jce_script_java)
	add_custom_command(TARGET JCE_Editor POST_BUILD
		COMMAND "${CMAKE_COMMAND}" -E copy_if_different
			"$<TARGET_FILE:jce_script_java>" "$<TARGET_FILE_DIR:JCE_Editor>"
		VERBATIM)
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
