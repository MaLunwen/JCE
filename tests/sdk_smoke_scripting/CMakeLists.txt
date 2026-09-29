# tests/sdk_smoke_scripting — the MULTI-LANGUAGE SDK consumer gate.
#
# tests/sdk_smoke answers "can a foreign project link the engine and boot it".
# This one answers the question that had no answer at all until the SDK began
# installing scripting/: CAN A PROJECT THAT IS NOT THIS REPOSITORY RUN A
# PYTHON, JAVA OR C++ SCRIPT?  Measured before those install rules existed, on
# dist/sdk/win32-x86_64 at commit d3ecc452: find_package(JCE) declared JCE::JCE
# and nothing else — no JCE::ScriptApi, no JCE::ScriptVm*, no JCE_SCRIPT_*
# variable — with no error, because there was nothing to error about.
#
# Built OUT-OF-TREE against an installed SDK, exactly like tests/sdk_smoke:
#
#     cmake -S tests/sdk_smoke_scripting -B <bdir> -DJCE_DIR=<sdk>/lib/cmake/JCE
#
# It is a SEPARATE project from tests/sdk_smoke and not another target inside
# it, because the two gate opposite things: sdk_smoke must keep passing on an
# engine-only SDK (that is a legitimate product), and this one must FAIL on
# one.  Merged, the scripting half would have to be `if(TARGET ...)`-guarded —
# and a gate that skips itself when the thing it guards is missing is the
# shape of green that this whole campaign keeps finding.
cmake_minimum_required(VERSION 3.20)
# CXX for the same reason sdk_smoke needs it: the fat lib contains C++ objects
# and the final link must use the C++ driver.  Our own sources are C99.
project(JceSdkSmokeScripting C CXX)

# The producer passes the languages derived from the INSTALLED rosters and
# artefacts.  This project deliberately has no default: configuring it by hand
# without an explicit delivery contract must fail instead of testing whichever
# targets happen to be visible and calling that the SDK.
if(NOT JCE_SMOKE_SCRIPT_BACKENDS)
	message(FATAL_ERROR
		"sdk_smoke_scripting: JCE_SMOKE_SCRIPT_BACKENDS is empty. Pass the "
		"semicolon-separated installed SDK roster; an empty scripting layer "
		"is not a passing smoke test.")
endif()

set(_smoke_components "")
set(_smoke_languages "")
foreach(_lang IN LISTS JCE_SMOKE_SCRIPT_BACKENDS)
	string(TOLOWER "${_lang}" _lang)
	if(_lang STREQUAL "c")
		set(_component ScriptC)
	elseif(_lang STREQUAL "cpp")
		set(_component ScriptCpp)
	elseif(_lang STREQUAL "csharp")
		set(_component ScriptCSharp)
	elseif(_lang STREQUAL "java")
		set(_component ScriptJava)
	elseif(_lang STREQUAL "js")
		set(_component ScriptJs)
	elseif(_lang STREQUAL "python")
		set(_component ScriptPython)
	else()
		message(FATAL_ERROR
			"sdk_smoke_scripting: installed backend '${_lang}' has no consumer "
			"probe. Add its link/runtime probe before this SDK can pass.")
	endif()
	list(APPEND _smoke_components "${_component}")
	list(APPEND _smoke_languages "${_lang}")
endforeach()
list(REMOVE_DUPLICATES _smoke_components)
list(REMOVE_DUPLICATES _smoke_languages)

if(JCE_SMOKE_SCRIPT_API_REQUIRED)
	list(APPEND _smoke_components ScriptApi)
endif()
find_package(JCE REQUIRED COMPONENTS ${_smoke_components})
message(STATUS "sdk_smoke_scripting: required roster ${_smoke_languages}")

if(NOT JCE_HOST_TOOLS_FOUND)
	message(FATAL_ERROR
		"sdk_smoke_scripting: this SDK ships no usable host tools "
		"(jce_cook/jce_pak/jce_bin2obj) — it cannot cook the script asset "
		"this gate resolves out of a PAK, and the gate refuses to test a "
		"degraded SDK.")
endif()

add_executable(JceSdkSmokeScripting src/main.c)
set_target_properties(JceSdkSmokeScripting PROPERTIES
	OUTPUT_NAME          jce_sdk_smoke_scripting
	C_STANDARD           99
	C_STANDARD_REQUIRED  YES)

# THE IN-TREE SPELLINGS, on purpose.  jce_script_vm_python and jce_script_api
# are the names an in-tree project writes; the SDK declares them as aliases of
# JCE::ScriptVmPython / JCE::ScriptApi so that a project's CMakeLists is the
# same file in both worlds.  Using them here is what tests that claim.
target_link_libraries(JceSdkSmokeScripting PRIVATE JCE::JCE)
if(JCE_SMOKE_SCRIPT_API_REQUIRED)
	if(NOT TARGET jce_script_api)
		message(FATAL_ERROR
			"sdk_smoke_scripting: the roster requires ScriptApi but the SDK "
			"declared no jce_script_api target")
	endif()
	target_link_libraries(JceSdkSmokeScripting PRIVATE jce_script_api)
endif()

list(FIND _smoke_languages python _smoke_python)
if(NOT _smoke_python EQUAL -1)
	target_link_libraries(JceSdkSmokeScripting PRIVATE jce_script_vm_python)
	target_compile_definitions(JceSdkSmokeScripting PRIVATE
		JCE_SMOKE_HAVE_PYTHON=1
		JCE_SMOKE_PY_PACKAGE_DIR="${JCE_SCRIPT_PYTHON_PACKAGE_DIR}"
		JCE_SMOKE_PY_SDK_VERSION="${JCE_SCRIPT_PYTHON_VERSION}")
endif()

# The .dll that both bindings load BY NAME.  Not linking it is not the
# failure mode — not COPYING it is, and on Windows that failure is a process
# that does not start, with nothing printed by the engine at all.
jce_script_stage_runtime(JceSdkSmokeScripting)

# ── The optional languages ──────────────────────────────────────────────
#
# Reported, never skipped silently: an SDK that shipped the Java backend and
# a smoke that quietly did not exercise it is exactly the "150/151 dead"
# shape.  Each branch prints which side it took.
list(FIND _smoke_languages java _smoke_java)
if(NOT _smoke_java EQUAL -1)
	target_link_libraries(JceSdkSmokeScripting PRIVATE jce_script_vm_java)
	jce_script_java_find_jvm(_smoke_jvm)
	if(_smoke_jvm)
		target_compile_definitions(JceSdkSmokeScripting PRIVATE
			JCE_SMOKE_HAVE_JAVA=1
			JCE_SMOKE_JAVA_JVM="${_smoke_jvm}"
			JCE_SMOKE_JAVA_CLASS_PATH="${JCE_SCRIPT_JAVA_CLASS_PATH}"
			JCE_SMOKE_JAVA_SCRIPT_LIB="${JCE_SCRIPT_JAVA_JNI_LIBRARY}")
		message(STATUS "sdk_smoke_scripting: Java IN (jvm ${_smoke_jvm})")
	elseif(JCE_SMOKE_CAN_RUN_TARGET)
		message(FATAL_ERROR
			"sdk_smoke_scripting: Java is in the installed roster but no JVM "
			"was found for the native run gate. Set JAVA_HOME.")
	else()
		message(STATUS
			"sdk_smoke_scripting: Java LINK-ONLY — cross target cannot run on "
			"this host")
	endif()
	unset(_smoke_jvm)
else()
	message(STATUS "sdk_smoke_scripting: Java OUT — not in installed roster")
endif()

# The js backend brings a third-party static library with it, so "the SDK
# ships it" is not the same claim as "a consumer can link it".  IN means
# this smoke links jce_script_vm_js AND the quickjs the SDK installed.
# The C# backend brings a third-party RUNTIME with it, which is the half only
# an SDK consumer can prove: an install that shipped the archive but not the
# nethost import library links here (the conan-free graph hides nothing) and
# fails on the consumer; one that shipped the native host but not the managed
# assembly links AND runs, and then refuses to register.
list(FIND _smoke_languages csharp _smoke_csharp)
if(NOT _smoke_csharp EQUAL -1)
	target_link_libraries(JceSdkSmokeScripting PRIVATE jce_script_vm_csharp)
	if(NOT JCE_SCRIPT_CSHARP_ASSEMBLY OR
	   NOT EXISTS "${JCE_SCRIPT_CSHARP_ASSEMBLY}")
		message(FATAL_ERROR
			"sdk_smoke_scripting: C# is in the installed roster but its managed "
			"assembly is absent")
	endif()
	target_compile_definitions(JceSdkSmokeScripting PRIVATE
		JCE_SMOKE_HAVE_CSHARP=1
		JCE_SMOKE_CSHARP_ASSEMBLY="${JCE_SCRIPT_CSHARP_ASSEMBLY}")
	message(STATUS "sdk_smoke_scripting: C# IN (${JCE_SCRIPT_CSHARP_ASSEMBLY})")
else()
	message(STATUS "sdk_smoke_scripting: C# OUT - not in installed roster")
endif()

list(FIND _smoke_languages js _smoke_js)
if(NOT _smoke_js EQUAL -1)
	target_link_libraries(JceSdkSmokeScripting PRIVATE jce_script_vm_js)
	target_compile_definitions(JceSdkSmokeScripting PRIVATE JCE_SMOKE_HAVE_JS=1)
	message(STATUS "sdk_smoke_scripting: JS IN")
else()
	message(STATUS "sdk_smoke_scripting: JS OUT - not in installed roster")
endif()

list(FIND _smoke_languages cpp _smoke_cpp)
if(NOT _smoke_cpp EQUAL -1)
	target_link_libraries(JceSdkSmokeScripting PRIVATE jce_script_vm_cpp)
	target_compile_definitions(JceSdkSmokeScripting PRIVATE JCE_SMOKE_HAVE_CPP=1)
	message(STATUS "sdk_smoke_scripting: C++ IN")
else()
	message(STATUS "sdk_smoke_scripting: C++ OUT — not in installed roster")
endif()

list(FIND _smoke_languages c _smoke_c)
if(NOT _smoke_c EQUAL -1)
	target_link_libraries(JceSdkSmokeScripting PRIVATE jce_script_vm_c)
	target_compile_definitions(JceSdkSmokeScripting PRIVATE JCE_SMOKE_HAVE_C=1)
	message(STATUS "sdk_smoke_scripting: C IN")
else()
	message(STATUS "sdk_smoke_scripting: C OUT — not in installed roster")
endif()

# Cook + pack + embed, the turnkey consumer path.  The script asset goes
# through jce_cook, and jce_add_pak passes --preserve-names.
#
# That flag USED TO BE the only thing standing between this gate and a
# packaged-only death: without it jce_cook renamed every script to
# <stem>.jceasset, which resolves to NO LANGUAGE, and only the shipped build
# noticed — the editor keeps running loose files.  It is no longer load-
# bearing for scripts: jce_cook now refuses to rename anything the catalog
# calls a script, whatever the flag says, because a renamed script is never
# useful to anyone.  The flag still governs every OTHER asset, which is why
# it is still passed here.
#
# *Enforced by:* tests/os/resource/test_jce_cook_script_names.c, which runs
# the real cooker DELIBERATELY WITHOUT the flag.  This gate is downstream of
# that one and asserts the consumer-facing consequence, not the rule.
jce_add_pak(JceSdkSmokeScripting RESOURCE_DIRS assets)
