# JCEScriptEnable.cmake — jce_script_enable(<target> ...)
#
# ONE source, included from BOTH worlds: the in-tree build (scripting/
# CMakeLists.txt) and the installed SDK (JCEScripting.cmake includes it from
# beside itself).  That is not tidiness — JCEScripting.cmake.in says it in its
# own words: "two spellings for one question is how a project ends up with a
# branch that only ever ran in one of the two worlds", and this file exists
# because a project hit exactly that.
#
# ══ THE DEFECT THIS ENDS ═══════════════════════════════════════════════
#
# The engine registers exactly ONE language for itself — the built-in Lua VM.
# Every other backend registers itself from a function the APPLICATION calls,
# and if the application never calls it the backend is not merely off: the
# extension is unclaimed and every entity carrying such a script is refused at
# run time while the scene otherwise works perfectly.  The four steps —
#
#   1. BUILD the backend   2. LINK it beside jce_script_api
#   3. CALL its register() 4. TELL IT WHERE ITS RUNTIME LIVES
#
# — were, until now, ~200 lines of CMake plus ~200 lines of C in every project,
# and MEASURED in this repository: a project wrote all four correctly in its
# in-tree CMake branch and not in the SDK-consumer branch it actually ships
# from, so its editor ran five languages and its shipped executable ran one.
# Nothing errored at build time.  The probe in that project reported
# "1 of 5 languages live" and it had been true for some time.
#
# Unity, Unreal and Godot all put language availability in the BUILD layER —
# player settings, module descriptors, .gdextension manifests — never in
# hand-written game code.  Same shape here:
#
#     jce_script_enable(MyGame)                       # CMake, one line
#     jce_script_register_linked_languages(NULL);     # C, one line
#
# ══ WHAT STAYS PROJECT CODE ════════════════════════════════════════════
#
# Publishing native (cpp/c) script MODULES.  Those classes are the project's,
# so the project passes a callback to the C call above.  Everything else here
# is boilerplate, and boilerplate that is copied is boilerplate that diverges.

if(COMMAND jce_script_enable)
	return()
endif()

set(_JCE_SCRIPT_ENABLE_DIR "${CMAKE_CURRENT_LIST_DIR}"
	CACHE INTERNAL "Directory holding jce_script_register_linked.{c,h}.in")

# ---------------------------------------------------------------------- #
#  jce_script_enable(<target>                                             #
#                    [LANGUAGES <lang>...]   default: every one available #
#                    [REQUIRED]              fail configure on a miss     #
#                    [NO_STAGE])             skip the runtime copy        #
#                                                                         #
#  Links the language backends this build actually has, bakes the runtime #
#  inputs they cannot discover, and generates the registration shim that   #
#  jce_script_register_linked_languages() lives in.                        #
#                                                                         #
#  Silent by default about what is MISSING, loud about what it DID: a      #
#  consumer that asked for everything available cannot be missing anything, #
#  and a consumer that named languages gets REQUIRED to say so.  The       #
#  status line names the languages, because "which languages does this     #
#  binary have" is the question every symptom in this area reduces to.     #
# ---------------------------------------------------------------------- #
function(jce_script_enable JCE_SE_TARGET)
	cmake_parse_arguments(_se "REQUIRED;NO_STAGE" "" "LANGUAGES" ${ARGN})

	if(NOT TARGET ${JCE_SE_TARGET})
		message(FATAL_ERROR
			"jce_script_enable(${JCE_SE_TARGET}): no such target. Call this "
			"AFTER add_executable()/add_library().")
	endif()

	# ---- discover the roster ----------------------------------------- #
	#
	# KNOWN is every backend this tree (or this SDK) carries a roster for, and
	# is deliberately independent of whether the backend can be BUILT here.
	# AVAILABLE is the subset whose VM target exists.  Two projections over one
	# set, because "there is no such language" and "that language is not in your
	# build" have different fixes and a design with only the second cannot say
	# which one you are looking at.
	#
	# CONFIGURE_DEPENDS so a NEW backend directory re-runs CMake by itself,
	# matching scripting/CMakeLists.txt, which discovers the same directories.
	file(GLOB _rosters CONFIGURE_DEPENDS
		"${_JCE_SCRIPT_ENABLE_DIR}/backends/*/jce_backend.cmake"
		"${_JCE_SCRIPT_ENABLE_DIR}/../*/jce_backend.cmake")
	set(_known "")
	set(_avail "")
	set(_exempt "")
	foreach(_r IN LISTS _rosters)
		set(JCE_BACKEND_LANGUAGE "")
		set(JCE_BACKEND_VM_TARGET "")
		set(JCE_BACKEND_UNAVAILABLE_ON "")
		include("${_r}")
		if(NOT JCE_BACKEND_LANGUAGE OR NOT JCE_BACKEND_VM_TARGET)
			message(FATAL_ERROR
				"jce_script_enable: ${_r} is not a roster — it must set "
				"JCE_BACKEND_LANGUAGE and JCE_BACKEND_VM_TARGET. A file that "
				"is silently skipped here is a backend that builds and is "
				"never linked.")
		endif()
		list(APPEND _known "${JCE_BACKEND_LANGUAGE}")
		set(_jce_se_roster_${JCE_BACKEND_LANGUAGE} "${_r}")
		if(TARGET ${JCE_BACKEND_VM_TARGET})
			list(APPEND _avail "${JCE_BACKEND_LANGUAGE}")
		endif()
		# EXEMPT is a THIRD projection, and it exists so the dist gate below
		# can tell two identical-looking absences apart: "this machine has no
		# JDK" (a defect in a shipping build) and "the Web target has no
		# embeddable CPython to begin with" (a fact about the platform, which
		# no amount of installing can change).  Without it the gate would have
		# exactly two options, both wrong -- refuse every Web dist build, or
		# accept a desktop dist that silently lost a language.
		#
		# The BACKEND declares it, not the gate.  A list here would be a
		# central file every new backend has to edit, which is the thing the
		# roster glob exists to avoid.
		if(JCE_BACKEND_UNAVAILABLE_ON AND
		   CMAKE_SYSTEM_NAME IN_LIST JCE_BACKEND_UNAVAILABLE_ON)
			list(APPEND _exempt "${JCE_BACKEND_LANGUAGE}")
		endif()
	endforeach()
	list(REMOVE_DUPLICATES _known)
	list(REMOVE_DUPLICATES _avail)
	if(NOT _known)
		message(FATAL_ERROR
			"jce_script_enable(${JCE_SE_TARGET}): found no backend roster under "
			"${_JCE_SCRIPT_ENABLE_DIR}. An SDK installs one per backend into "
			"lib/cmake/JCE/backends/; in-tree they are scripting/*/"
			"jce_backend.cmake. Finding none means the layer is not installed, "
			"NOT that this engine has no scripting languages.")
	endif()

	if(_se_LANGUAGES)
		set(_want "${_se_LANGUAGES}")
	else()
		set(_want "${_avail}")
	endif()
	list(REMOVE_DUPLICATES _want)

	# "lua" is accepted and ignored on purpose: it is inside the engine and
	# always present, and refusing it would make the obvious spelling
	# — LANGUAGES lua python — an error for no reason a caller can act on.
	if("lua" IN_LIST _want)
		list(REMOVE_ITEM _want lua)
	endif()

	set(_got "")
	set(_missing "")
	foreach(_l ${_want})
		if(NOT _l IN_LIST _known)
			message(FATAL_ERROR
				"jce_script_enable(${JCE_SE_TARGET}): '${_l}' is not a JCE "
				"scripting language. Known: ${_known} (lua is built into the "
				"engine and always present). This list is discovered, so a "
				"language whose backend directory is here but whose toolchain "
				"is not would appear above and be reported as unavailable "
				"instead — seeing it absent here means the engine really has "
				"no such language.")
		endif()
		if(_l IN_LIST _avail)
			list(APPEND _got ${_l})
		else()
			list(APPEND _missing ${_l})
		endif()
	endforeach()

	# A SHIPPING BUILD DOES NOT SILENTLY LOSE A LANGUAGE.
	#
	# java, python, csharp and js each detect their toolchain and switch
	# themselves off when it is absent -- deliberately, so a contributor
	# without a JDK, an embeddable CPython or a .NET pack can still configure
	# the engine (scripting/python/CMakeLists.txt records that reasoning and it
	# is right).  The cost is that the SAME silence applies to the artefact
	# that gets shipped: build a dist editor on a machine without a JDK and it
	# is a dist editor that cannot run .jcejava, with nothing in the build
	# saying so.  That is the "a target stops being built and nobody notices"
	# failure the python file names, arriving one variant later.
	#
	# WHY THIS COMPARES AGAINST _known AND NOT _missing.
	#
	# The first version of this gate tested _missing, and _missing CANNOT BE
	# NON-EMPTY for the caller that matters.  A caller who names no LANGUAGES
	# gets `set(_want "${_avail}")` a few lines up, so _want is _avail by
	# construction, so _missing is empty by construction -- and every in-tree
	# caller, editor/CMakeLists.txt included, names no LANGUAGES.  The gate
	# ran, reported nothing, and could not have reported anything.  MEASURED:
	# `cmake -DJCE_BUILD_VARIANT=dist -DJCE_BUILD_SCRIPT_VM_PYTHON=OFF` -- a
	# dist editor with Python deliberately switched off -- configured with
	# exit 0.  It was a gate that could not fail, which is the failure shape
	# this repository has paid for most often.
	#
	# The default is the whole defect.  "Every language available" quietly
	# means "every language that survived", and what did not survive is
	# precisely what nobody wanted to lose.  So for a caller who wrote no
	# list, the expectation is _known -- every language this ENGINE has, which
	# is a property of the source tree and does not shrink when a toolchain is
	# missing.  A caller who DID write a list is held to that list: they said
	# what they ship, and dist must deliver it.
	#
	# EXEMPT comes out first: a backend whose roster names this system is
	# absent by design, not by omission.
	#
	# dist ONLY, not release.  release is what a contributor builds, and making
	# it hard would re-create exactly the problem the QUIET detection exists to
	# avoid.  dist is the variant whose output leaves this machine.
	if(JCE_BUILD_VARIANT STREQUAL "dist")
		if(_se_LANGUAGES)
			set(_dist_expect "${_want}")
			set(_dist_why "this call names LANGUAGES")
		else()
			set(_dist_expect "${_known}")
			set(_dist_why
				"this call names no LANGUAGES, which means every language the "
				"engine has")
		endif()
		foreach(_x ${_exempt})
			list(REMOVE_ITEM _dist_expect ${_x})
		endforeach()
		set(_dist_absent "")
		foreach(_l ${_dist_expect})
			if(NOT _l IN_LIST _avail)
				list(APPEND _dist_absent ${_l})
			endif()
		endforeach()
		if(_dist_absent)
			message(FATAL_ERROR
				"jce_script_enable(${JCE_SE_TARGET}): JCE_BUILD_VARIANT=dist, "
				"and language(s) '${_dist_absent}' are not in this build "
				"(${_dist_why}). A dist artefact is the one that ships, so a "
				"language missing here ships missing -- silently, because the "
				"backends switch themselves off when their toolchain is "
				"absent. This engine has: '${_known}'. This build has: "
				"'${_avail}'. java needs a JDK (JAVA_HOME); python needs an "
				"embeddable CPython (development headers and import library); "
				"csharp needs the .NET app-host pack (DOTNET_ROOT); js needs "
				"quickjs-ng from conan. Build dist on a machine that has "
				"them, or pass LANGUAGES to this call so the omission is "
				"written down in the project rather than discovered by a "
				"user.")
		endif()
	endif()

	if(_missing AND _se_REQUIRED)
		message(FATAL_ERROR
			"jce_script_enable(${JCE_SE_TARGET}): REQUIRED language(s) "
			"'${_missing}' are not in this build. Available: "
			"'${_avail}'. An SDK ships a backend only if the SDK BUILD had "
			"its toolchain: java needs a JDK (JAVA_HOME) and "
			"`jce.py sdk --script-java`, python needs an embeddable CPython. "
			"Both backends detect their toolchain and switch themselves off "
			"when it is absent, so a missing one here means the SDK was built "
			"on a machine without it.")
	elseif(_missing)
		message(STATUS
			"JCE scripting: ${JCE_SE_TARGET} asked for '${_missing}' and this "
			"build does not have them — those scripts will be refused at run "
			"time, by name. Pass REQUIRED to make this a configure error.")
	endif()

	if(NOT _got)
		message(STATUS
			"JCE scripting: ${JCE_SE_TARGET} links no backend beyond the "
			"engine's built-in Lua. No shim was generated.")
		return()
	endif()

	# ---- link, and define one guard per language --------------------- #
	# ---- link, and take every per-language trait from its roster ------ #
	#
	# The traits below used to be if("cpp" IN_LIST _got) branches in this file,
	# i.e. this function knowing things about languages.  A sixth backend would
	# have had to come here and add its own branch, and nothing would have said
	# so -- it would simply have been linked without its trait.
	set(_needs_api OFF)
	set(_has_native OFF)
	set(_cxx_std "")
	foreach(_l ${_got})
		set(JCE_BACKEND_VM_TARGET "")
		set(JCE_BACKEND_NEEDS_SCRIPT_API OFF)
		set(JCE_BACKEND_HAS_NATIVE_MODULES OFF)
		set(JCE_BACKEND_CXX_STANDARD "")
		include("${_jce_se_roster_${_l}}")
		target_link_libraries(${JCE_SE_TARGET} PRIVATE ${JCE_BACKEND_VM_TARGET})
		string(TOUPPER "${_l}" _L)
		target_compile_definitions(${JCE_SE_TARGET} PRIVATE
			JCE_SCRIPT_LINKED_${_L}=1)
		if(JCE_BACKEND_NEEDS_SCRIPT_API)
			set(_needs_api ON)
		endif()
		if(JCE_BACKEND_HAS_NATIVE_MODULES)
			set(_has_native ON)
		endif()
		if(JCE_BACKEND_CXX_STANDARD)
			if(NOT _cxx_std OR JCE_BACKEND_CXX_STANDARD GREATER _cxx_std)
				set(_cxx_std "${JCE_BACKEND_CXX_STANDARD}")
			endif()
		endif()
	endforeach()

	# NATIVE MODULES COMPILED INTO THIS BINARY MUST NOT EMIT THE LOADER ENTRY.
	# JCE_C_MODULE_END and JCE_CPP_MODULE_END both export the one symbol a
	# plugin loader looks up, `jce_cpp_script_module` -- so a project with both
	# a C and a C++ module linked statically (which is exactly what "one module
	# is one language" asks a multi-language project to do, and what a shipped
	# game wants, since then there is no plugin to find) fails to link with
	#
	#     LNK2005: jce_cpp_script_module already defined in <obj>
	#
	# naming a symbol its sources never mention.  This function is called on the
	# HOST target, where the entry is dead code -- a statically linked host
	# reaches its modules through the accessor, and nothing dlsym()s a symbol
	# out of its own executable.  A module built as a separate shared object
	# does NOT call this function and keeps its entry, which is the only door
	# there.  Driven by JCE_BACKEND_HAS_NATIVE_MODULES so a future native
	# backend inherits it by declaring the trait, not by editing this file.
	if(_has_native)
		target_compile_definitions(${JCE_SE_TARGET} PRIVATE
			JCE_SCRIPT_MODULE_NO_ENTRY=1)
	endif()

	# THE C++ STANDARD IS PART OF THE CONTRACT, not the consumer's problem.
	# jce_script_cpp.hpp opens with `#error "requires C++17 or later"`, and a
	# project whose module translation unit is compiled at the compiler's
	# default (C++14 on MSVC) hits that error rather than anything actionable.
	#
	# Consumers reached for set_source_files_properties(... CXX_STANDARD 20),
	# which is a NO-OP: CXX_STANDARD is a TARGET property and CMake does not
	# warn about it on a source.  In-tree that went unnoticed because the root
	# build sets CMAKE_CXX_STANDARD globally, so the wrong call sat next to a
	# working build for as long as nobody consumed the SDK -- MEASURED on the
	# first SDK-consumer build that enabled the cpp backend.
	if(_cxx_std)
		get_property(_langs GLOBAL PROPERTY ENABLED_LANGUAGES)
		if("CXX" IN_LIST _langs)
			target_compile_features(${JCE_SE_TARGET} PRIVATE cxx_std_${_cxx_std})
		else()
			message(WARNING
				"jce_script_enable(${JCE_SE_TARGET}): a linked backend needs C++"
				"${_cxx_std} but CXX is not an enabled language in this "
				"project. Add CXX to project() — a native script module's "
				"translation unit needs a C++ compiler, and the failure "
				"without one names a missing CMAKE_CXX_COMPILER rather than "
				"this.")
		endif()
	endif()

	# jce_script_api is what the python and java BINDINGS call down through,
	# and what a cpp/c module links.  Linked once, when any roster asks.
	if(_needs_api)
		if(TARGET jce_script_api)
			target_link_libraries(${JCE_SE_TARGET} PRIVATE jce_script_api)
		else()
			message(WARNING
				"JCE scripting: ${JCE_SE_TARGET} links backend(s) that call down "
				"through jce_script_api (${_got}) but this build has no such "
				"target. Every engine call FROM a script will fail at the first "
				"one.")
		endif()
	endif()

	# ---- the runtime inputs the backends cannot discover -------------- #
	#
	# Each roster NAMES the variables its fragment substitutes; whoever knows
	# how to find them SETS them (the backend's own CMakeLists in-tree, and
	# JCEScripting.cmake from an SDK).  This function only consumes, so a new
	# backend with a new runtime input needs no branch here.
	#
	# EMPTY IS A WARNING AND NOT AN ERROR, by name, because the two known cases
	# differ: a missing python package dir is fatal at the first .py, while a
	# missing java JVM path is only the FALLBACK -- the shim prefers JAVA_HOME at
	# run time, because the JVM belongs to whoever runs the game.
	set(_runtime_vars "")
	foreach(_l ${_got})
		set(JCE_BACKEND_RUNTIME_VARS "")
		set(JCE_BACKEND_RUNTIME_VARS_OPTIONAL "")
		include("${_jce_se_roster_${_l}}")
		# The optional ones are substituted into the shim exactly the same
		# way; they simply do not warn when empty, because the fragment has
		# its own runtime lookup and this is only its fallback.
		list(APPEND _runtime_vars ${JCE_BACKEND_RUNTIME_VARS_OPTIONAL})
		foreach(_v IN LISTS JCE_BACKEND_RUNTIME_VARS)
			list(APPEND _runtime_vars "${_v}")
			if(NOT DEFINED ${_v} OR "${${_v}}" STREQUAL "")
				message(WARNING
					"JCE scripting: ${JCE_SE_TARGET} links the ${_l} backend but "
					"${_v} is empty. That is a runtime input the backend cannot "
					"discover for itself; the generated shim will bake an empty "
					"string and the failure will surface inside the running game, "
					"at the first ${_l} script.")
			endif()
		endforeach()
	endforeach()

	# ---- generate the shim, in TWO configure_file passes -------------- #
	#
	# Pass 1 substitutes each backend's OWN fragment with that backend's
	# variables and writes it out.  Pass 2 substitutes the outer template with
	# the concatenated fragments as ONE value.
	#
	# This works because configure_file scans its INPUT FILE and does not
	# re-scan the VALUES it substitutes: a fragment whose text contains an
	# @NAME@ after pass 1 is not touched again in pass 2, and a value carrying
	# a `;` passes through whole.  That is what lets each backend keep its C
	# beside the code it registers, instead of this file assembling C out of
	# CMake strings or carrying one #ifdef per language.
	set(_out "${CMAKE_CURRENT_BINARY_DIR}/jce_script_enable/${JCE_SE_TARGET}")
	set(JCE_SE_INCLUDES "")
	set(JCE_SE_FRAGMENTS "")
	set(JCE_SE_HAS_NATIVE_MODULES 0)
	set(JCE_SE_NEEDS_SCRIPT_API 0)
	foreach(_l ${_got})
		set(JCE_BACKEND_REGISTER_INCLUDE "")
		set(JCE_BACKEND_REGISTER_FRAGMENT "")
		set(JCE_BACKEND_HAS_NATIVE_MODULES OFF)
		set(JCE_BACKEND_NEEDS_SCRIPT_API OFF)
		include("${_jce_se_roster_${_l}}")
		if(JCE_BACKEND_NEEDS_SCRIPT_API)
			set(JCE_SE_NEEDS_SCRIPT_API 1)
		endif()
		if(JCE_BACKEND_HAS_NATIVE_MODULES)
			set(JCE_SE_HAS_NATIVE_MODULES 1)
		endif()
		if(NOT JCE_BACKEND_REGISTER_FRAGMENT)
			message(FATAL_ERROR
				"jce_script_enable: the ${_l} roster declares no "
				"JCE_BACKEND_REGISTER_FRAGMENT. Without one the backend is "
				"LINKED AND NEVER REGISTERED, which is the quietest failure in "
				"this whole layer: the extension resolves to nothing and every "
				"entity carrying such a script is refused at run time while the "
				"scene works perfectly.")
		endif()
		get_filename_component(_rdir "${_jce_se_roster_${_l}}" DIRECTORY)
		set(_frag_in "${_rdir}/${JCE_BACKEND_REGISTER_FRAGMENT}")
		if(NOT EXISTS "${_frag_in}")
			message(FATAL_ERROR
				"jce_script_enable: the ${_l} roster names "
				"${JCE_BACKEND_REGISTER_FRAGMENT} but ${_frag_in} does not "
				"exist. NOT skipped: a silently missing fragment links the "
				"backend and registers nothing. If this is an installed SDK, "
				"the fragment was not installed beside its roster.")
		endif()
		configure_file("${_frag_in}" "${_out}/frag_${_l}.c" @ONLY)
		file(READ "${_out}/frag_${_l}.c" _frag_text)
		string(TOUPPER "${_l}" _L)
		string(APPEND JCE_SE_INCLUDES
			"#ifdef JCE_SCRIPT_LINKED_${_L}\n${JCE_BACKEND_REGISTER_INCLUDE}\n#endif\n")
		string(APPEND JCE_SE_FRAGMENTS
			"\n#ifdef JCE_SCRIPT_LINKED_${_L}\n${_frag_text}#endif\n")
	endforeach()
	string(REPLACE ";" " " JCE_SE_LANGUAGES_TEXT "${_got}")
	configure_file("${_JCE_SCRIPT_ENABLE_DIR}/jce_script_register_linked.h.in"
		"${_out}/jce_script_register_linked.h" @ONLY)
	configure_file("${_JCE_SCRIPT_ENABLE_DIR}/jce_script_register_linked.c.in"
		"${_out}/jce_script_register_linked.c" @ONLY)
	target_sources(${JCE_SE_TARGET} PRIVATE
		"${_out}/jce_script_register_linked.c")
	target_include_directories(${JCE_SE_TARGET} PRIVATE "${_out}")

	# ---- the shared libraries the LOADER looks for by name ------------ #
	#
	# ROSTER-DRIVEN, and defined here rather than only in the SDK config,
	# because an IN-TREE consumer needs staging just as much: the editor was
	# doing it with two hand-written POST_BUILD copies, and jce_script_enable
	# silently skipped staging in-tree because jce_script_stage_runtime is
	# declared in JCEScripting.cmake, which only an SDK consumer includes.
	#
	# Skipping it is a process that does not start, on Windows with no message
	# from the engine at all -- so it is on by default and opting out is
	# explicit.
	if(NOT _se_NO_STAGE)
		set(_stage "")
		if(_needs_api)
			list(APPEND _stage jce_script_api)
		endif()
		set(_stage_files "")
		set(_stage_dirs "")
		foreach(_l ${_got})
			set(JCE_BACKEND_STAGE_TARGETS "")
			set(JCE_BACKEND_STAGE_FILES "")
			set(JCE_BACKEND_STAGE_DIRS "")
			include("${_jce_se_roster_${_l}}")
			list(APPEND _stage ${JCE_BACKEND_STAGE_TARGETS})
			list(APPEND _stage_files ${JCE_BACKEND_STAGE_FILES})
			list(APPEND _stage_dirs ${JCE_BACKEND_STAGE_DIRS})
		endforeach()
		list(REMOVE_DUPLICATES _stage)
		foreach(_t IN LISTS _stage)
			if(TARGET ${_t})
				add_custom_command(TARGET ${JCE_SE_TARGET} POST_BUILD
					COMMAND "${CMAKE_COMMAND}" -E copy_if_different
						"$<TARGET_FILE:${_t}>" "$<TARGET_FILE_DIR:${JCE_SE_TARGET}>"
					VERBATIM)
			endif()
		endforeach()

		# STAGE_FILES, beside STAGE_TARGETS: a backend's runtime input is not
		# always something CMake builds.  The C# backend's managed assembly is
		# produced by `dotnet build` (deliberately outside CMake, so a machine
		# with no .NET SDK still builds every native target), so there is no
		# target to take $<TARGET_FILE:> of -- and staging it is not optional:
		# hostfxr finds the runtimeconfig BY NAME beside the assembly, and the
		# backend looks for the assembly beside the executable.
		#
		# Missing at CONFIGURE time is not an error: the managed build may not
		# have run yet, and a hard failure here would make `cmake` depend on
		# it.  copy_if_different reports the miss at BUILD time, once, naming
		# the file.
		list(REMOVE_DUPLICATES _stage_files)
		foreach(_f IN LISTS _stage_files)
			if(_f)
				add_custom_command(TARGET ${JCE_SE_TARGET} POST_BUILD
					COMMAND "${CMAKE_COMMAND}" -E copy_if_different
						"${_f}" "$<TARGET_FILE_DIR:${JCE_SE_TARGET}>"
					VERBATIM)
			endif()
		endforeach()

		# STAGE_DIRS, beside STAGE_FILES, because two of the runtime inputs
		# are DIRECTORIES and copy_if_different does not do directories:
		#   java   -- the com/jce/script/*.class tree the JVM needs
		#   python -- the jce_script package that goes on sys.path
		#
		# Both were resolved ONLY through their configure-time paths, which
		# point into the SDK install.  So a packaged game carried every
		# runtime .dll and neither of these, and on any machine without this
		# SDK it started, exited 0, and refused java and python by name.
		# Measured: hiding dist/sdk/**/share/jce/scripting cost a packaged
		# game exactly those two languages while it still ran.
		#
		# The names are fixed by the registration shim
		# (JCE_SE_JAVA_CLASSES_DIR / JCE_SE_PYTHON_PACKAGE_DIR in
		# jce_script_register_linked.c.in), which is what looks beside the
		# executable for them -- one name, declared once, read by both sides.
		list(REMOVE_DUPLICATES _stage_dirs)
		foreach(_d IN LISTS _stage_dirs)
			if(_d)
				string(REPLACE "|" ";" _dparts "${_d}")
				list(GET _dparts 0 _dsrc)
				list(GET _dparts 1 _dname)
				add_custom_command(TARGET ${JCE_SE_TARGET} POST_BUILD
					COMMAND "${CMAKE_COMMAND}" -E copy_directory
						"${_dsrc}" "$<TARGET_FILE_DIR:${JCE_SE_TARGET}>/${_dname}"
					VERBATIM)
			endif()
		endforeach()
	endif()

	# The SDK config also declares jce_script_stage_runtime() for consumers that
	# call it directly; the loop above covers callers of this function.
	message(STATUS
		"JCE scripting: ${JCE_SE_TARGET} runs lua (built in) plus ${_got}")
endfunction()
