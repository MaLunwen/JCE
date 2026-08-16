cmake_minimum_required(VERSION 3.20)

# ------------------------------------------------------------------ #
#  jce_script_api — the script-facing shared library.                 #
#                                                                     #
#  Owner decision 7: "a small new shared target exporting the         #
#  scripting entry points ... the engine itself stays static and      #
#  unchanged."  It is followed here in the strongest available form:  #
#  this target links NO engine layer at all.  It is pure marshalling  #
#  glue — every call it serves ends in a function pointer the HOST    #
#  supplied through JceScriptHost, and the engine behind that pointer #
#  lives in the executable, exactly as GDExtension's glue lives       #
#  beside the Godot binary rather than containing it.                 #
#                                                                     #
#  That is also what makes the sandbox claim structural rather than   #
#  aspirational: "a script cannot reach past the scripting surface    #
#  because the surface is all the shared object exports" is true here #
#  because there is no engine code inside the object to reach.        #
#                                                                     #
#  Consequence, stated rather than hidden: a process with no engine   #
#  in it cannot do anything with this library.  It is the in-process  #
#  binding transport (decision 1), not a way to drive JCE from a bare #
#  Python interpreter.                                                #
# ------------------------------------------------------------------ #

set(_JCE_SCRIPT_API_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_library(jce_script_api SHARED
	"${_JCE_SCRIPT_API_SRC}/jce_script_api.gen.c"
	"${_JCE_SCRIPT_API_SRC}/jce_script_api_core.c")

set_target_properties(jce_script_api PROPERTIES
	C_STANDARD          99
	C_STANDARD_REQUIRED YES
	C_EXTENSIONS        OFF
	# CMake would otherwise define jce_script_api_EXPORTS for this target,
	# which is only ever read by a dllexport macro.  There is none: the
	# export set is the generated .def / version script.
	DEFINE_SYMBOL       ""
	# The whole point of this target is that the export set is a curated
	# list.  ON would export every symbol in the object and make the
	# symbol-table test's "nothing else" half meaningless.
	WINDOWS_EXPORT_ALL_SYMBOLS OFF)

target_include_directories(jce_script_api
	PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
	        "${CMAKE_SOURCE_DIR}/engine/include"
	PRIVATE "${_JCE_SCRIPT_API_SRC}")

target_compile_definitions(jce_script_api PRIVATE JCE_SCRIPT_API_BUILD=1)

jce_target_warnings(jce_script_api STRICT)

if(MSVC)
	# MSVC exports nothing by default, so the .def IS the export set.
	# Listed as a source (not as a bare /DEF: link flag) so that editing it
	# relinks: a stale export table that still matches the committed list is
	# exactly the "green by accident" state this repo keeps rediscovering.
	target_sources(jce_script_api PRIVATE
		"${_JCE_SCRIPT_API_SRC}/jce_script_api.def")
else()
	target_compile_options(jce_script_api PRIVATE -fvisibility=hidden)
	if(NOT APPLE)
		# GNU ld / lld.  Apple's linker has no --version-script; there
		# -fvisibility=hidden plus the JCE_SCRIPT_API attribute on every
		# declaration is the whole mechanism.
		target_link_options(jce_script_api PRIVATE
			"LINKER:--version-script=${_JCE_SCRIPT_API_SRC}/jce_script_api.map")
		set_property(TARGET jce_script_api APPEND PROPERTY
			LINK_DEPENDS "${_JCE_SCRIPT_API_SRC}/jce_script_api.map")
	endif()
endif()

# ── Both halves of the export mechanism must actually be in the build ──
#
# Same reasoning as engine/CMakeLists.txt's assertion that the generated Lua
# bindings reach the compiler: the generator proves the files are CORRECT and
# says nothing about whether they are BUILT.  Each of these failures produces a
# library that links and loads:
#
#   * the forwarders missing  -> a DLL whose exports resolve to nothing;
#   * the .def missing        -> a DLL that exports NOTHING, under which the
#                                symbol-table test's "exports nothing else"
#                                assertion passes vacuously.
#
# The second is the dangerous one, so it is a FATAL_ERROR and not a warning.
get_target_property(_jce_script_api_srcs jce_script_api SOURCES)
if(NOT _jce_script_api_srcs MATCHES "jce_script_api\\.gen\\.c")
	message(FATAL_ERROR
		"jce_script_api sources do not include jce_script_api.gen.c — the "
		"generated forwarders are not being compiled. Regenerate with:\n"
		"  python tools/scriptgen/gen_script_c_abi.py --write")
endif()
if(MSVC AND NOT _jce_script_api_srcs MATCHES "jce_script_api\\.def")
	message(FATAL_ERROR
		"jce_script_api has no module-definition file on MSVC — the DLL would "
		"export nothing and the symbol-table test would pass vacuously. "
		"Regenerate with:\n"
		"  python tools/scriptgen/gen_script_c_abi.py --write")
endif()
unset(_jce_script_api_srcs)
