# Elemental Serenity — standalone JCE SDK-consumer app.
# Built out-of-tree via `python scripts/jce.py app examples/elemental_serenity`.
# (Adapted from meadow_valley/CMakeLists.txt standalone branch.)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
	cmake_minimum_required(VERSION 3.20)
	project(ElementalSerenity C CXX)
	if(NOT DEFINED JCE_BUILD_VARIANT)
		set(JCE_BUILD_VARIANT release CACHE STRING
			"JCE build variant (debug|release|dist)")
	endif()

	find_package(JCE REQUIRED)

	add_executable(ElementalSerenity
		"${CMAKE_CURRENT_SOURCE_DIR}/src/main.c"
		"${CMAKE_CURRENT_SOURCE_DIR}/src/es_camera.c"
		"${CMAKE_CURRENT_SOURCE_DIR}/src/es_perf.c"
		"${CMAKE_CURRENT_SOURCE_DIR}/src/es_scene_orchestrator.c"
		"${CMAKE_CURRENT_SOURCE_DIR}/src/es_aid_mood.c"
		# main.c calls es_script_langs_register() and the es_script_probe_*
		# trio unconditionally; this branch's list had drifted behind the
		# in-tree branch's (line ~298) and the SDK build failed to link.
		"${CMAKE_CURRENT_SOURCE_DIR}/src/es_script_langs.c"
		"${CMAKE_CURRENT_SOURCE_DIR}/src/es_script_probe.c")
	set_target_properties(ElementalSerenity PROPERTIES OUTPUT_NAME elemental_serenity)

	# ---------------------------------------------------------------- #
	#  EVERY LANGUAGE THIS SDK HAS, in one line.                        #
	#                                                                   #
	#  What used to be here was nothing at all: the ~190 lines that     #
	#  link the backends, bake their runtime paths and call their       #
	#  register()s live in the IN-TREE branch below, which this branch  #
	#  returns before reaching.  So `scripts/jce.py app` -- the way     #
	#  this project actually ships -- produced an executable that ran   #
	#  Lua and nothing else, while the editor ran four languages, and   #
	#  nothing errored at build time.  The probe said "1 of 5 languages #
	#  live" and had been right for some time.                          #
	#                                                                   #
	#  jce_script_enable() (scripting/cmake/JCEScriptEnable.cmake, in   #
	#  the SDK) does all four steps and generates the registration      #
	#  shim; es_script_langs.c calls into it.                           #
	# ---------------------------------------------------------------- #
	if(COMMAND jce_script_enable)
		# The two NATIVE MODULES stay project code -- the classes are this
		# project's.  Their sources are added here; publishing them is the
		# callback es_script_langs.c hands the shim.
		if(TARGET jce_script_vm_cpp)
			target_sources(ElementalSerenity PRIVATE
				"${CMAKE_CURRENT_SOURCE_DIR}/src/es_flower_sway.cpp")

		endif()
		if(TARGET jce_script_vm_c)
			target_sources(ElementalSerenity PRIVATE
				"${CMAKE_CURRENT_SOURCE_DIR}/src/es_prop_surface.c")
			set_source_files_properties(
				"${CMAKE_CURRENT_SOURCE_DIR}/src/es_prop_surface.c"
				PROPERTIES LANGUAGE C)
		endif()
		jce_script_enable(ElementalSerenity)
		target_compile_definitions(ElementalSerenity PRIVATE
			ES_SCRIPT_ENABLE_SHIM=1)
	else()
		message(WARNING
			"elemental_serenity: this SDK predates jce_script_enable() — "
			"the exe will run Lua only, exactly as it did before that "
			"function existed. Rebuild the SDK.")
	endif()
	# Editor Play uses the project's native classes through the public SDK ABI.
	# Modules link the call-down interface, never a second host registry.
	foreach(_es_native IN ITEMS cpp c)
		if(TARGET jce_script_${_es_native}_module)
			if(_es_native STREQUAL "cpp")
				set(_es_module es_scripts)
				set(_es_module_source src/es_flower_sway.cpp)
			else()
				set(_es_module es_scripts_c)
				set(_es_module_source src/es_prop_surface.c)
			endif()
			add_library(${_es_module} MODULE "${_es_module_source}")
			target_link_libraries(${_es_module} PRIVATE jce_script_${_es_native}_module)
			target_compile_definitions(${_es_module} PRIVATE ES_SCRIPT_MODULE_SHARED=1)
			set_target_properties(${_es_module} PROPERTIES
				C_STANDARD 99 CXX_STANDARD 20 PREFIX ""
				RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/native/bin"
				LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/native/bin")
			add_dependencies(ElementalSerenity ${_es_module})
		endif()
	endforeach()

	jce_configure_application_target(ElementalSerenity)

	# Release crash-symbol aid (mirrors the engine root CMakeLists): /Zi +
	# /DEBUG emit elemental_serenity.pdb so jce_crash_handler resolves static
	# engine frames instead of nearest-export noise; /OPT:REF,ICF restore the
	# optimizations /DEBUG would otherwise disable.
	if(MSVC)
		target_compile_options(ElementalSerenity PRIVATE "$<$<CONFIG:Release>:/Zi>")
		target_link_options(ElementalSerenity PRIVATE
			"$<$<CONFIG:Release>:/DEBUG>"
			"$<$<CONFIG:Release>:/OPT:REF>"
			"$<$<CONFIG:Release>:/OPT:ICF>")
	endif()

	target_compile_definitions(ElementalSerenity PRIVATE SDL_MAIN_USE_CALLBACKS=1)
	target_include_directories(ElementalSerenity PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/src")
	target_link_libraries(ElementalSerenity PRIVATE JCE::JCE)
	# libcurl's schannel TLS backend needs the Windows cert-chain APIs
	# (CertGetCertificateChain etc.).  The SDK's JCEConfig links bcrypt/ws2_32
	# but not crypt32/secur32 yet — add them here for the sandbox build.
	if(WIN32)
		target_link_libraries(ElementalSerenity PRIVATE crypt32 secur32 iphlpapi)
	endif()

	# Runtime scene policy consumes public SDK APIs only.

	# ---- Web (Emscripten) link options + HTML shell --------------- #
	# Without this the target emits only elemental_serenity.js/.wasm and the
	# packager fails ("artifact missing: .html").  Mirrors caged_kingdom: a
	# self-contained shell that hosts the canvas and resumes the audio context
	# on first click (browsers gate WebAudio behind a user gesture).
	# NB: use the Emscripten toolchain's built-in EMSCRIPTEN, not
	# JCE_PLATFORM_WEB — the latter is set only in the engine's root
	# CMakeLists, which a STANDALONE app build never processes.
	if(EMSCRIPTEN)
		set(_SHELL "${CMAKE_BINARY_DIR}/shell.html")
		file(WRITE "${_SHELL}" [=[<!doctype html>
<html><head><meta charset="utf-8"><title>Elemental Serenity</title>
<style>
html,body{margin:0;padding:0;overflow:hidden;width:100%;height:100%;background:#000}
canvas{display:block;width:100%;height:100%;border:0;padding:0}
</style></head><body>
<canvas id="canvas" oncontextmenu="event.preventDefault()" tabindex="-1"></canvas>
<script>
/* Industry-standard WebGL context attributes for games (what Unity/Godot web
 * exports request).  The BROWSER DEFAULTS are hostile to fullscreen perf:
 *   antialias:true  -> hidden whole-canvas MSAA + per-frame resolve
 *   alpha:true      -> canvas composited WITH page transparency
 *   powerPreference:'default' -> dual-GPU laptops run WebGL on the iGPU
 *     (native D3D12 uses the dGPU — explains "desktop 100+fps, web 20").
 * The engine cannot reach these (context is created inside the wasm), so
 * intercept getContext and force them for every WebGL context on the page. */
(function(){
  var gc=HTMLCanvasElement.prototype.getContext;
  HTMLCanvasElement.prototype.getContext=function(type,attrs){
    if(type==='webgl'||type==='webgl2'||type==='experimental-webgl'){
      attrs=attrs||{};
      attrs.antialias=false;
      attrs.alpha=false;
      attrs.premultipliedAlpha=false;
      attrs.preserveDrawingBuffer=false;
      attrs.powerPreference='high-performance';
      attrs.desynchronized=false;
    }
    return gc.call(this,type,attrs);
  };
})();
var Module={
  canvas:document.getElementById('canvas'),
  /* Emscripten routes the engine's stderr wholesale to console.error, so
   * every [MAIN] INFO line showed as an error.  Re-grade by content. */
  printErr:function(t){
    if(/ERROR -|FATAL -/.test(t))      console.error(t);
    else if(/WARN -/.test(t))          console.warn(t);
    else                               console.info(t);
  },
  /* URL knobs -> engine env (Emscripten ENV must be set in preRun).
   *   ?autoshoot  : ES_AUTOSHOOT=1 (walk the 8 season/tod states — lets
   *                 headless capture compare every state vs desktop). */
  preRun:[function(){
    if(/[?&]autoshoot/.test(location.search)) ENV.ES_AUTOSHOOT='1';
    if(/[?&]perflog/.test(location.search))   ENV.JCE_PERF_LOG='1';
    if(/[?&]mute/.test(location.search))      ENV.JCE_AUDIO_DISABLE='1';
    /* perf-bisect kill switches (combinable, e.g. ?nograss&nopostfx) */
    if(/[?&]nograss/.test(location.search))   ENV.JCE_NO_GRASS='1';
    if(/[?&]nofoliage/.test(location.search)) ENV.JCE_NO_FOLIAGE='1';
    if(/[?&]nopostfx/.test(location.search))  ENV.JCE_NO_POSTFX='1';
    /* tunable web grass density scale, e.g. ?grass=0.4 (default 0.6 on web) */
    var gm=location.search.match(/[?&]grass=([0-9.]+)/);
    if(gm) ENV.JCE_GRASS_DENSITY=gm[1];
  }]
};
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

		set_target_properties(ElementalSerenity PROPERTIES SUFFIX ".html")
		target_link_options(ElementalSerenity PRIVATE
			-sWASM=1
			-sALLOW_MEMORY_GROWTH=1
			-sMAX_WEBGL_VERSION=2
			-sMIN_WEBGL_VERSION=2
			-sFULL_ES3=1
			-sSTACK_SIZE=1048576
			-sMALLOC=emmalloc
			# NB: -sASSERTIONS=1 (inherited from the caged_kingdom debug-friendly
			# template) instruments every memory access at runtime — measurable
			# frame cost in a Release ship.  Keep it OFF here; flip on locally
			# when chasing a wasm crash.
			-sASSERTIONS=0
			"SHELL:--shell-file ${_SHELL}")
	endif()

	# ---- _embedded_bundles.h (generated; main.c includes it) ------ #
	set(_emb_inc "${CMAKE_CURRENT_BINARY_DIR}/_embedded_bundles.h")
	file(WRITE "${_emb_inc}"
"/* Auto-generated by CMake -- DO NOT EDIT */\n"
"#ifndef JCE_PROJECT_EMBEDDED_BUNDLES_H\n"
"#define JCE_PROJECT_EMBEDDED_BUNDLES_H\n"
"#include <stddef.h>\n"
"static const struct { const char *name; const unsigned char *data; const size_t *size; } jce_project_embedded_bundles[] = {\n"
"    { 0, 0, 0 }\n"
"};\n"
"#define JCE_PROJECT_EMBEDDED_BUNDLE_COUNT 0\n"
"#endif\n")
	target_include_directories(ElementalSerenity PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

	# ---- Game asset PAK ------------------------------------------- #
	set(_ck_res "${CMAKE_CURRENT_SOURCE_DIR}/resources/assets")
	set(_ck_ui  "${CMAKE_CURRENT_SOURCE_DIR}/ui")
	set(_ck_dirs)
	if(IS_DIRECTORY "${_ck_res}")
		list(APPEND _ck_dirs "${_ck_res}")
	endif()
	if(IS_DIRECTORY "${_ck_ui}")
		list(APPEND _ck_dirs "${_ck_ui}")
	endif()
	set(_ck_cooked_dir "${CMAKE_CURRENT_BINARY_DIR}/ElementalSerenity_cooked")

	set(_ck_have_prebuilt OFF)
	if((DEFINED JCE_PROJECT_PREBUILT_ASSETS_OBJ AND EXISTS "${JCE_PROJECT_PREBUILT_ASSETS_OBJ}") OR
	   (DEFINED JCE_PROJECT_PREBUILT_ASSETS_ASM AND EXISTS "${JCE_PROJECT_PREBUILT_ASSETS_ASM}") OR
	   (DEFINED JCE_PROJECT_PREBUILT_ASSETS_C   AND EXISTS "${JCE_PROJECT_PREBUILT_ASSETS_C}"))
		set(_ck_have_prebuilt ON)
	endif()

	set(_ck_cooked_available OFF)
	if(COMMAND jce_add_pak AND
	   (_ck_have_prebuilt OR (JCE_PAK_EXECUTABLE AND _ck_dirs)))
		jce_add_pak(ElementalSerenity
			RESOURCE_DIRS ${_ck_dirs})
		if(NOT _ck_have_prebuilt AND JCE_COOK_EXECUTABLE)
			set(_ck_cooked_available ON)
		endif()
	else()
		set(_stub "${CMAKE_CURRENT_BINARY_DIR}/jce_stub_pak.c")
		file(WRITE "${_stub}"
"#include <stddef.h>\n"
"const unsigned char assets_pak_data[1] = {0};\n"
"const size_t        assets_pak_data_size = 0;\n")
		target_sources(ElementalSerenity PRIVATE "${_stub}")
	endif()

	# ---- Stage loose runtime data next to the exe ----------------- #
	if(NOT DEFINED JCE_PROJECT_COOKED_ASSETS)
		set(JCE_PROJECT_COOKED_ASSETS "resources/_cooked")
	endif()
	if(NOT DEFINED JCE_PROJECT_STAGE_LOOSE_ASSETS)
		set(JCE_PROJECT_STAGE_LOOSE_ASSETS ON)
	endif()
	if(JCE_PROJECT_STAGE_LOOSE_ASSETS)
		file(READ "${CMAKE_CURRENT_SOURCE_DIR}/jce_project.json" _es_manifest)
		string(JSON _es_manifest REMOVE "${_es_manifest}" script_modules)
		file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/es_runtime_project.json"
			CONTENT "${_es_manifest}\n")

		add_custom_command(TARGET ElementalSerenity POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
				"${CMAKE_CURRENT_BINARY_DIR}/es_runtime_project.json"
				"$<TARGET_FILE_DIR:ElementalSerenity>/jce_project.json"
			COMMENT "Staging jce_project.json next to elemental_serenity.exe"
			VERBATIM)
	endif()
	if(JCE_PROJECT_STAGE_LOOSE_ASSETS AND _ck_cooked_available)
		add_custom_command(TARGET ElementalSerenity POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_directory
				"${_ck_cooked_dir}"
				"$<TARGET_FILE_DIR:ElementalSerenity>/${JCE_PROJECT_COOKED_ASSETS}"
			COMMENT "Staging cooked assets for elemental_serenity"
			VERBATIM)
	elseif(JCE_PROJECT_STAGE_LOOSE_ASSETS AND
	       IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${JCE_PROJECT_COOKED_ASSETS}")
		add_custom_command(TARGET ElementalSerenity POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_directory
				"${CMAKE_CURRENT_SOURCE_DIR}/${JCE_PROJECT_COOKED_ASSETS}"
				"$<TARGET_FILE_DIR:ElementalSerenity>/${JCE_PROJECT_COOKED_ASSETS}"
			COMMENT "Staging cooked assets for elemental_serenity"
			VERBATIM)
	endif()

	# Particle emitter descs are read CWD-relative by the runtime
	# (jce_particles_desc_load_json -> jce_json_parse_file), not via the
	# cooked-assets root, so stage them beside the exe too.
	if(JCE_PROJECT_STAGE_LOOSE_ASSETS AND
	   IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/resources/assets/particles")
		add_custom_command(TARGET ElementalSerenity POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_directory
				"${CMAKE_CURRENT_SOURCE_DIR}/resources/assets/particles"
				"$<TARGET_FILE_DIR:ElementalSerenity>/particles"
			COMMENT "Staging particle assets beside elemental_serenity.exe"
			VERBATIM)
	endif()

	install(TARGETS ElementalSerenity DESTINATION bin)
	return()
endif()

# ------------------------------------------------------------------ #
#  In-tree branch — added by the JCE root build via                   #
#  add_subdirectory(elemental_serenity) under -DJCE_BUILD_INTREE_ES.  #
#                                                                     #
#  WHY THIS BRANCH EXISTED.  Everything above is the SDK-consumer      #
#  build (find_package(JCE)), and when this branch was written that    #
#  build COULD NOT turn on Python, Java or C++ scripting: the SDK      #
#  installed the engine fat lib and headers and nothing from           #
#  scripting/ — no jce_script_api import library, no jce_script_vm_*   #
#  archive, no jce_script Python package, no Java classes — and        #
#  JCEConfig.cmake exported no target for any of them.                 #
#                                                                     #
#  SUPERSEDED 2026-08-16.  scripting/cmake/JCEScriptingInstall.cmake   #
#  now installs that layer and JCEConfig.cmake includes the generated  #
#  JCEScripting.cmake, so the SDK-consumer build above CAN enable all  #
#  three — with the SAME target names (jce_script_vm_python,           #
#  jce_script_api, … are aliases of JCE::ScriptVmPython, …).  The      #
#  out-of-tree proof is tests/sdk_smoke_scripting in the JCE repo.     #
#                                                                     #
#  This branch is kept because it is convenient for iterating without  #
#  producing an SDK first, and because /elemental_serenity/ is         #
#  unversioned so nothing else in the repo exercises it.  It is no     #
#  longer the only door.  See elemental_serenity/MULTILANG.md.         #
# ------------------------------------------------------------------ #

set(_es_src "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_executable(ElementalSerenity
	"${_es_src}/main.c"
	"${_es_src}/es_camera.c"
	"${_es_src}/es_perf.c"
	"${_es_src}/es_scene_orchestrator.c"
	"${_es_src}/es_aid_mood.c"
	"${_es_src}/es_script_langs.c"
	"${_es_src}/es_script_probe.c")

set_target_properties(ElementalSerenity PROPERTIES
	OUTPUT_NAME elemental_serenity
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${JCE_BUILD_VARIANT}")

target_compile_definitions(ElementalSerenity PRIVATE SDL_MAIN_USE_CALLBACKS=1)
target_include_directories(ElementalSerenity PRIVATE "${_es_src}")
target_link_libraries(ElementalSerenity PRIVATE JCE)

# TWO NATIVE SCRIPT MODULES ARE COMPILED INTO THIS EXE — es_flower_sway.cpp
# (C++) and es_prop_surface.c (C) — and each of JCE_CPP_MODULE_END /
# JCE_C_MODULE_END emits the SHARED-OBJECT entry point `jce_cpp_script_module`,
# which is one external symbol.  Two of them is:
#
#   es_prop_surface.c.obj : error LNK2005: jce_cpp_script_module already
#                           defined in es_flower_sway.cpp.obj
#   elemental_serenity.exe : fatal error LNK1169
#
# measured here the first time the C module was added.  The entry is DEAD CODE
# in an executable — a statically linked host reaches a module through the
# accessor and jce_script_vm_cpp_add_module(), and nothing dlsym()s a symbol
# out of its own process image — so it is suppressed for this target only.
# es_scripts.dll and es_scripts_c.dll are separate targets and do NOT inherit
# this, which is correct: for them the entry is the only door.
target_compile_definitions(ElementalSerenity PRIVATE JCE_SCRIPT_MODULE_NO_ENTRY=1)

# _embedded_bundles.h — main.c includes it unconditionally.
set(_es_emb "${CMAKE_CURRENT_BINARY_DIR}/_embedded_bundles.h")
file(WRITE "${_es_emb}"
"/* Auto-generated by CMake -- DO NOT EDIT */\n"
"#ifndef JCE_PROJECT_EMBEDDED_BUNDLES_H\n"
"#define JCE_PROJECT_EMBEDDED_BUNDLES_H\n"
"#include <stddef.h>\n"
"static const struct { const char *name; const unsigned char *data; const size_t *size; } jce_project_embedded_bundles[] = {\n"
"    { 0, 0, 0 }\n"
"};\n"
"#define JCE_PROJECT_EMBEDDED_BUNDLE_COUNT 0\n"
"#endif\n")
target_include_directories(ElementalSerenity PRIVATE
	"${CMAKE_CURRENT_BINARY_DIR}")

# ------------------------------------------------------------------ #
#  THE REAL PACKAGER, not a stub.                                     #
#                                                                     #
#  A packaged-build-only failure is the one that costs months: the    #
#  editor and a loose-asset run both keep reading scripts off the     #
#  host filesystem, so a script the COOKER renamed or the PACKER      #
#  dropped is invisible until somebody installs the game.  This       #
#  project now carries three script files in three languages and two  #
#  of those extensions reached the pipeline recently, so the in-tree  #
#  build embeds a REAL cooked+packed archive rather than the one-byte #
#  stub, and the run that proves four languages work is a run against #
#  that archive.                                                      #
#                                                                     #
#  MEASURED WHILE WRITING THIS: `jce_cook --batch` WITHOUT            #
#  --preserve-names renames every script to <stem>.jceasset, so       #
#  scripts/es_fireflies.py becomes scripts/es_fireflies.jceasset and  #
#  resolves to no language at all.  jce_add_pak passes                #
#  --preserve-names; a hand-rolled cook step would not, and would     #
#  produce exactly the silent packaged-only failure above.  That is   #
#  the reason this calls the helper instead of the tool.              #
#                                                                     #
#  jce_add_pak() reads JCE_{COOK,PAK}_EXECUTABLE.  The SDK's          #
#  JCEConfig sets them from <sdk>/bin; the in-tree build has the      #
#  TARGETS and sets neither, so point them at the targets here.       #
# ------------------------------------------------------------------ #
if(NOT JCE_COOK_EXECUTABLE AND TARGET jce_cook)
	set(JCE_COOK_EXECUTABLE "$<TARGET_FILE:jce_cook>")
endif()
if(NOT JCE_PAK_EXECUTABLE AND TARGET jce_pak)
	set(JCE_PAK_EXECUTABLE "$<TARGET_FILE:jce_pak>")
endif()

if(COMMAND jce_add_pak AND JCE_PAK_EXECUTABLE)
	set(_es_pak_dirs "${CMAKE_CURRENT_SOURCE_DIR}/resources/assets")
	if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/ui")
		list(APPEND _es_pak_dirs "${CMAKE_CURRENT_SOURCE_DIR}/ui")
	endif()
	jce_add_pak(ElementalSerenity RESOURCE_DIRS ${_es_pak_dirs})
	add_dependencies(ElementalSerenity jce_cook jce_pak)
	unset(_es_pak_dirs)
else()
	message(WARNING
		"elemental_serenity: no jce_pak — linking the one-byte stub PAK. "
		"The run then reads loose assets only, which is exactly the "
		"configuration that cannot detect a packaging defect.")
	set(_es_nopak "${CMAKE_CURRENT_BINARY_DIR}/es_no_pak.c")
	file(WRITE "${_es_nopak}"
"/* GENERATED -- see elemental_serenity/CMakeLists.txt */\n"
"#include <stddef.h>\n"
"const unsigned char assets_pak_data[1] = {0};\n"
"const size_t        assets_pak_data_size = 0;\n")
	target_sources(ElementalSerenity PRIVATE "${_es_nopak}")
endif()

# ------------------------------------------------------------------ #
#  The three non-Lua backends.                                        #
#                                                                     #
#  EACH IS OPTIONAL AT CONFIGURE TIME AND MANDATORY AT RUN TIME, and  #
#  the halves are split on exactly that: a missing backend does NOT   #
#  drop its script from the scene (the scene is data and does not     #
#  know what this build linked).  It surfaces as the runtime refusing #
#  that entity by name and as a 0 in the probe table, which is the    #
#  loudest of the available failures and the only one that names the  #
#  language.                                                          #
# ------------------------------------------------------------------ #
set(_es_langs "lua")

# ---- Python ------------------------------------------------------- #
if(TARGET jce_script_vm_python)
	target_link_libraries(ElementalSerenity PRIVATE
		jce_script_vm_python jce_script_api)
	target_compile_definitions(ElementalSerenity PRIVATE
		ES_HAVE_SCRIPT_PYTHON=1
		ES_SCRIPT_PYTHON_PACKAGE_DIR="${JCE_SCRIPT_PYTHON_PACKAGE_DIR}")
	list(APPEND _es_langs "python")
else()
	message(WARNING
		"elemental_serenity: Python scripting OFF — no jce_script_vm_python "
		"target (scripting/python found no embeddable CPython, or "
		"-DJCE_BUILD_SCRIPT_VM_PYTHON=OFF). The scene still carries "
		"scripts/es_fireflies.py; the run will refuse it with 'no script VM "
		"claims its extension' and the probe table will show python 0/0.")
endif()

# ---- Java --------------------------------------------------------- #
if(TARGET jce_script_vm_java)
	find_package(Python3 COMPONENTS Interpreter QUIET)
	if(NOT Python3_Interpreter_FOUND)
		message(FATAL_ERROR
			"elemental_serenity: the Java backend compiles the scripting "
			"classes through scripting/java/build_java.py and there is no "
			"Python 3 interpreter.")
	endif()

	# The JDK, DERIVED from the one place the configure already found it.
	# Deriving beats a second probe: two probes can disagree, and then the
	# jni.h jce_script_vm_java compiled against and the JVM it starts are
	# different JDKs — an UnsatisfiedLinkError with no obvious cause.
	get_filename_component(_es_java_home "${JCE_JNI_INCLUDE_DIR}" DIRECTORY)
	if(WIN32)
		set(_es_java_jvm "${_es_java_home}/bin/server/jvm.dll")
	elseif(APPLE)
		set(_es_java_jvm "${_es_java_home}/lib/server/libjvm.dylib")
	else()
		set(_es_java_jvm "${_es_java_home}/lib/server/libjvm.so")
	endif()
	if(NOT EXISTS "${_es_java_jvm}")
		message(FATAL_ERROR
			"elemental_serenity: no JVM shared library at ${_es_java_jvm}, "
			"derived from JCE_JNI_INCLUDE_DIR=${JCE_JNI_INCLUDE_DIR}. A bare "
			"name is refused by jce_script_vm_java_configure() on purpose, so "
			"there is no fallback to offer.")
	endif()

	# The scripting classes the JVM loads BY CLASS PATH.  Nothing links them,
	# so the edge has to be add_dependencies: without it they are built only
	# when somebody happens to build "all", and the symptom is a green
	# `--target ElementalSerenity` followed by java 0/0 in the probe table.
	set(_es_java_classes "${CMAKE_CURRENT_BINARY_DIR}/java_classes")
	file(GLOB_RECURSE _es_java_srcs CONFIGURE_DEPENDS
		"${CMAKE_SOURCE_DIR}/scripting/java/src/main/java/*.java")
	add_custom_command(
		OUTPUT  "${_es_java_classes}/.stamp"
		COMMAND "${Python3_EXECUTABLE}"
		        "${CMAKE_SOURCE_DIR}/scripting/java/build_java.py"
		        --out "${_es_java_classes}" --no-jar
		COMMAND "${CMAKE_COMMAND}" -E touch "${_es_java_classes}/.stamp"
		DEPENDS ${_es_java_srcs}
		COMMENT "Compiling the Java scripting classes for elemental_serenity"
		VERBATIM)
	add_custom_target(es_java_classes DEPENDS "${_es_java_classes}/.stamp")

	target_link_libraries(ElementalSerenity PRIVATE
		jce_script_vm_java jce_script_api)
	# jce_script_java is NOT linked: it is loaded BY the JVM and its exports
	# are JNI entry points this executable never calls.  Build-order edge
	# only, and its absolute path is handed to the JVM as -Djce.script.library.
	add_dependencies(ElementalSerenity es_java_classes jce_script_java)

	target_compile_definitions(ElementalSerenity PRIVATE ES_HAVE_SCRIPT_JAVA=1)
	# file(GENERATE) rather than target_compile_definitions for these three:
	# $<TARGET_FILE:jce_script_java> is a generator expression, and the values
	# end up in a file a person can read when the JVM fails to start.
	file(GENERATE
		OUTPUT  "${CMAKE_CURRENT_BINARY_DIR}/generated/es_script_java_paths.gen.h"
		CONTENT "/* GENERATED by elemental_serenity/CMakeLists.txt. */
#ifndef ES_SCRIPT_JAVA_PATHS_GEN_H
#define ES_SCRIPT_JAVA_PATHS_GEN_H
#define ES_SCRIPT_JAVA_JVM_LIBRARY \"${_es_java_jvm}\"
#define ES_SCRIPT_JAVA_CLASS_PATH  \"${_es_java_classes}/classes\"
#define ES_SCRIPT_JAVA_SCRIPT_LIB  \"$<TARGET_FILE:jce_script_java>\"
#endif
")
	list(APPEND _es_langs "java")
else()
	message(WARNING
		"elemental_serenity: Java scripting OFF — no jce_script_vm_java "
		"target. Configure with -DJCE_BUILD_SCRIPT_JAVA=ON "
		"-DJCE_BUILD_SCRIPT_API_SHARED=ON and JAVA_HOME pointing at a JDK. "
		"The scene still carries scripts/EsCampfire.java; the run will refuse "
		"it and the probe table will show java 0/0.")
endif()

# ---- C++ ---------------------------------------------------------- #
# The only backend whose "script" is a CLASS rather than a file: its
# instantiate path is resolved through a module registry, not opened.
#
# THE SAME TRANSLATION UNIT IS BUILT TWICE AND THAT IS THE DESIGN, not a
# duplication:
#
#   into elemental_serenity.exe   — jce_script_vm_cpp_add_module(), no loader
#       involved, which is what a shipped game wants: one binary, no plugin
#       to find, and the classes exist before the first scene loads.
#   into es_scripts.dll           — a MODULE library the EDITOR loads at
#       project-open through jce_script_vm_cpp_load_library(), named in this
#       project's jce_project.json "script_modules".  The editor is a
#       different process and does not link this game, so without this target
#       editor Play can run three of the four languages and not the fourth.
#
# JCE_CPP_MODULE_END emits BOTH the in-process accessor and the exported
# entry symbol, so one source serves both.
if(TARGET jce_script_vm_cpp)
	target_sources(ElementalSerenity PRIVATE "${_es_src}/es_flower_sway.cpp")
	set_source_files_properties("${_es_src}/es_flower_sway.cpp" PROPERTIES
		CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)
	target_link_libraries(ElementalSerenity PRIVATE
		jce_script_vm_cpp jce_script_api)
	target_compile_definitions(ElementalSerenity PRIVATE ES_HAVE_SCRIPT_CPP=1)
	list(APPEND _es_langs "cpp")

	# jce_script_cpp_module, NOT jce_script_vm_cpp: a module must not carry
	# the registry.  A plugin that linked its own copy would publish its
	# classes into a registry the host never reads — a module that loads,
	# reports success, and whose classes no instantiate() can find.
	if(TARGET jce_script_cpp_module)
		add_library(es_scripts MODULE "${_es_src}/es_flower_sway.cpp")
		set_target_properties(es_scripts PROPERTIES
			CXX_STANDARD          20
			CXX_STANDARD_REQUIRED YES
			CXX_EXTENSIONS        OFF
			# Beside the editor and beside jce_script_api.dll: on Windows a
			# module's own dependencies resolve from the EXECUTABLE's
			# directory, not the module's, so a DLL parked anywhere else
			# would map only when the editor happened to be next to it.
			RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${JCE_BUILD_VARIANT}"
			LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${JCE_BUILD_VARIANT}")
		target_link_libraries(es_scripts PRIVATE jce_script_cpp_module)
		# The host-side es_flower_sway_register() calls
		# jce_script_vm_cpp_add_module(), which lives in the REGISTRY — the one
		# library a module must not link.  Compile it out here; the DLL's door
		# is the exported jce_cpp_script_module that JCE_CPP_MODULE_END emits.
		target_compile_definitions(es_scripts PRIVATE ES_SCRIPT_MODULE_SHARED=1)
		# Nothing links this into anything, so nothing in the dependency
		# graph would build it: without this edge it is built only when
		# somebody happens to build "all", and the symptom is a green build
		# followed by "C++ script module ABSENT" in the editor.
		add_dependencies(ElementalSerenity es_scripts)
		message(STATUS
			"elemental_serenity: es_scripts MODULE built for editor Play "
			"(declare it in jce_project.json \"script_modules\").")
	else()
		message(WARNING
			"elemental_serenity: no jce_script_cpp_module target — the "
			"editor-loadable script module will NOT be built, and editor "
			"Play will report 'C++ script module ABSENT'. The exe's own "
			"statically linked copy is unaffected.")
	endif()
else()
	message(WARNING
		"elemental_serenity: C++ scripting OFF — no jce_script_vm_cpp target.")
endif()

# ---- C ------------------------------------------------------------ #
# The fifth language, and the cheapest one this project could possibly
# add: es_prop_surface.c is compiled by the same C compiler that is
# already compiling main.c, links no interpreter, and puts nothing new
# beside the exe.  Python needs an embeddable CPython, Java needs a JDK
# to build and a JVM plus jce_script_java.dll to run, and the C++ script
# needs the C++ toolchain.
#
# IT SHARES THIS PROJECT'S OWN HEADERS, WHICH IS THE POINT.
# es_prop_surface.c includes src/es_palettes.h — the generated palette
# build/gen_director.py parses to emit the Lua director — so the seasonal
# rocks_tint exists exactly once in this project and is read by both
# consumers from the same file.  That is a thing only the language the
# project is already written in can do.
#
# SEPARATE FROM THE C++ BACKEND IN EVERY DIRECTION EXCEPT ONE: its own
# target, its own register(), its own claimed extension (.jcec), its own
# module and its own DLL — and ONE shared native class registry, because
# a compiled class has no language at run time.  jce_script_vm_c.h
# states the arrangement and why a second registry would be a fiction.
if(TARGET jce_script_vm_c)
	target_sources(ElementalSerenity PRIVATE "${_es_src}/es_prop_surface.c")
	set_source_files_properties("${_es_src}/es_prop_surface.c" PROPERTIES
		LANGUAGE C)
	target_link_libraries(ElementalSerenity PRIVATE
		jce_script_vm_c jce_script_api)
	target_compile_definitions(ElementalSerenity PRIVATE ES_HAVE_SCRIPT_C=1)
	list(APPEND _es_langs "c")

	# jce_script_c_module, NOT jce_script_vm_c: same rule as the C++ one —
	# a module must not carry the registry, or it publishes its classes
	# into a table the host never reads.
	if(TARGET jce_script_c_module)
		# A SECOND MODULE LIBRARY, not a second class in es_scripts.dll.
		# JCE_C_MODULE_END and JCE_CPP_MODULE_END both export
		# `jce_cpp_script_module` (one loader looks up one symbol), so the
		# two translation units cannot share a shared object — and one
		# module descriptor is one translation unit anyway.  This is the
		# two-entry "script_modules" shape jce_script_vm_c.h documents.
		add_library(es_scripts_c MODULE "${_es_src}/es_prop_surface.c")
		set_target_properties(es_scripts_c PROPERTIES
			C_STANDARD          99
			C_STANDARD_REQUIRED YES
			C_EXTENSIONS        OFF
			LINKER_LANGUAGE     C
			RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${JCE_BUILD_VARIANT}"
			LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${JCE_BUILD_VARIANT}")
		# es_palettes.h lives in src/ and the module includes it directly.
		target_include_directories(es_scripts_c PRIVATE "${_es_src}")
		target_link_libraries(es_scripts_c PRIVATE jce_script_c_module)
		target_compile_definitions(es_scripts_c PRIVATE
			ES_SCRIPT_MODULE_SHARED=1)
		# Nothing links this, so nothing in the dependency graph would build
		# it; without the edge it appears only under "all" and the symptom
		# is a green build followed by "native script module ABSENT" in the
		# editor.
		add_dependencies(ElementalSerenity es_scripts_c)
		message(STATUS
			"elemental_serenity: es_scripts_c MODULE built for editor Play "
			"(second entry in jce_project.json \"script_modules\").")
	else()
		message(WARNING
			"elemental_serenity: no jce_script_c_module target — the "
			"editor-loadable C script module will NOT be built, and editor "
			"Play will refuse scripts/EsPropSurface.jcec. The exe's own "
			"statically linked copy is unaffected.")
	endif()
else()
	message(WARNING
		"elemental_serenity: C scripting OFF — no jce_script_vm_c target. "
		"The scene still carries scripts/EsPropSurface.jcec; the run will "
		"refuse it and the probe table will show c 0/0.")
endif()

target_include_directories(ElementalSerenity PRIVATE
	"${CMAKE_CURRENT_BINARY_DIR}/generated")

message(STATUS "elemental_serenity: script languages = ${_es_langs}")

# jce_script_api is a SHARED library and BOTH the Python ctypes binding and the
# Java JNI shim call it by name at run time.  A missing DLL is a loader failure
# before main(), which prints nothing and looks exactly like a crash — so stage
# every shared library beside the exe.
foreach(_es_lib jce_script_api jce_script_java)
	if(TARGET ${_es_lib})
		get_target_property(_es_ty ${_es_lib} TYPE)
		if(_es_ty STREQUAL "SHARED_LIBRARY")
			add_custom_command(TARGET ElementalSerenity POST_BUILD
				COMMAND "${CMAKE_COMMAND}" -E copy_if_different
					"$<TARGET_FILE:${_es_lib}>"
					"$<TARGET_FILE_DIR:ElementalSerenity>"
				VERBATIM)
		endif()
	endif()
endforeach()

# The project manifest, beside the exe, so a run from the output directory
# finds its own startup_scene.
add_custom_command(TARGET ElementalSerenity POST_BUILD
	COMMAND "${CMAKE_COMMAND}" -E copy_if_different
		"${CMAKE_CURRENT_SOURCE_DIR}/jce_project.json"
		"$<TARGET_FILE_DIR:ElementalSerenity>/jce_project.json"
	VERBATIM)

unset(_es_src)
unset(_es_emb)
unset(_es_nopak)
unset(_es_langs)
unset(_es_lib)
unset(_es_ty)
