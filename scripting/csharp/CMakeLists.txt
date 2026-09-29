cmake_minimum_required(VERSION 3.20)

# ------------------------------------------------------------------ #
#  jce_script_vm_csharp — the "csharp" JceScriptVM, hosting .NET.     #
#                                                                     #
#  ON WHEN THE .NET APP-HOST PACK IS THERE, OFF WHEN IT IS NOT — the  #
#  same shape scripting/python and scripting/java have: option        #
#  defaults ON, probe QUIETLY, return with a STATUS line naming what  #
#  was missing.  .NET is an OPTIONAL dependency of this build.        #
#                                                                     #
#  IT IS THE MACHINE'S RUNTIME, not ours, and that is the same        #
#  decision java made about the JVM: the SDK ships the glue and the   #
#  managed assembly, and the player's .NET runs it.  Vendoring a      #
#  runtime instead would add 70 MB to every shipped game (measured:   #
#  a self-contained net8.0 publish of a hello-world is 70.3 MB) for a #
#  language that is one of six.                                       #
#                                                                     #
#  ONLY the C host is built here.  `dotnet build` is NOT run from     #
#  CMake — scripting/csharp/build_csharp.py owns the managed build,   #
#  so a machine with a C toolchain and no .NET SDK can still build    #
#  every native target this directory declares.                       #
# ------------------------------------------------------------------ #

option(JCE_BUILD_SCRIPT_CSHARP
	"Build the C# scripting backend's .NET host (needs the app-host pack)." ON)
if(NOT JCE_BUILD_SCRIPT_CSHARP)
	message(STATUS
		"JCE scripting: C# backend SKIPPED (-DJCE_BUILD_SCRIPT_CSHARP=OFF)")
	return()
endif()

# The managed side reaches the engine through the c_abi SHARED library, the
# way Java's classes do — P/Invoke is C#'s FFI and needs no shim of its own.
# Without that library there is nothing for a C# script to call.
if(NOT TARGET jce_script_api)
	message(STATUS
		"JCE scripting: C# backend SKIPPED — jce_script_api is not built "
		"(-DJCE_BUILD_SCRIPT_API_SHARED=OFF). Managed code P/Invokes that "
		"library and nothing else.")
	return()
endif()

# ── Where .NET lives ────────────────────────────────────────────────────
#
# DOTNET_ROOT first, because that is the variable the .NET tooling itself
# honours and a machine with a non-default install has already set it.
set(_jce_dotnet_roots "")
if(DEFINED ENV{DOTNET_ROOT})
	list(APPEND _jce_dotnet_roots "$ENV{DOTNET_ROOT}")
endif()
if(WIN32)
	list(APPEND _jce_dotnet_roots "$ENV{ProgramFiles}/dotnet" "C:/Program Files/dotnet")
elseif(APPLE)
	list(APPEND _jce_dotnet_roots "/usr/local/share/dotnet" "/opt/homebrew/share/dotnet")
else()
	list(APPEND _jce_dotnet_roots "/usr/share/dotnet" "/usr/lib/dotnet")
endif()

# The RID whose app-host pack carries the headers.  Only the host pack is
# per-RID; nothing here cross-compiles, so this is the BUILD machine's.
if(WIN32)
	set(_jce_rid_os "win")
elseif(APPLE)
	set(_jce_rid_os "osx")
else()
	set(_jce_rid_os "linux")
endif()
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
	set(_jce_rid_arch "x86")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
	set(_jce_rid_arch "arm64")
else()
	set(_jce_rid_arch "x64")
endif()
set(_jce_rid "${_jce_rid_os}-${_jce_rid_arch}")

# ── Pick the NEWEST pack, explicitly ────────────────────────────────────
#
# NOT find_path with a `*` in a HINT.  Measured: this machine carries 8.0.28,
# 9.0.19 and 10.0.9 side by side, and a glob HINT hands find_path the
# directories in the order the filesystem returns them — 10.0.9 sorts BEFORE
# 8.0.28 lexicographically, so "first match" is neither newest nor oldest but
# whichever the sort happens to produce.  A host header from one version and a
# runtime from another is exactly the mismatch the java backend documents for
# jni.h and the JVM.  So: glob, sort by VERSION, take the last.
set(_jce_host_pack "")
foreach(_root IN LISTS _jce_dotnet_roots)
	set(_base "${_root}/packs/Microsoft.NETCore.App.Host.${_jce_rid}")
	if(NOT IS_DIRECTORY "${_base}")
		continue()
	endif()
	file(GLOB _vers RELATIVE "${_base}" "${_base}/*")
	set(_good "")
	foreach(_v IN LISTS _vers)
		if(IS_DIRECTORY "${_base}/${_v}"
		   AND EXISTS "${_base}/${_v}/runtimes/${_jce_rid}/native/nethost.h")
			list(APPEND _good "${_v}")
		endif()
	endforeach()
	if(_good)
		list(SORT _good COMPARE NATURAL ORDER ASCENDING)
		list(GET _good -1 _newest)
		set(_jce_host_pack "${_base}/${_newest}/runtimes/${_jce_rid}/native")
		set(_jce_host_pack_version "${_newest}")
		break()
	endif()
endforeach()

if(NOT _jce_host_pack)
	message(STATUS
		"JCE scripting: C# backend SKIPPED — no Microsoft.NETCore.App.Host."
		"${_jce_rid} pack found under ${_jce_dotnet_roots}. Install the .NET "
		"SDK, or set DOTNET_ROOT. This is a gap in the MACHINE, not in the "
		"engine: every other target still builds.")
	return()
endif()

# THE SHARED nethost, and this is a measured constraint rather than a
# preference.  The pack ships both: `nethost.lib` imports nethost.dll, and
# `libnethost.lib` is static.  Static was tried first — it ships nothing extra
# — and it CANNOT be used here:
#
#   mimalloc.lib(alloc.c.obj) : error LNK2038: mismatch detected for
#   'RuntimeLibrary': value 'MD_DynamicRelease' doesn't match value
#   'MT_StaticRelease' in libnethost.lib(nethost.cpp.obj)
#
# Microsoft builds libnethost.lib against the STATIC CRT (/MT) and this engine
# uses the dynamic one (/MD).  Mixing them is refused by the linker, and the
# only ways to satisfy it are to rebuild the .NET host pack or to move the
# whole engine to /MT — both far larger than shipping one 90 KB DLL.
#
# nethost.dll is therefore a runtime input, and the roster stages it beside
# the executable with the managed assembly.  Getting that wrong is a process
# that does not start, so it is listed there and not left to chance.
find_library(JCE_NETHOST_LIBRARY
	NAMES nethost
	HINTS "${_jce_host_pack}"
	NO_DEFAULT_PATH)
find_file(JCE_NETHOST_RUNTIME
	NAMES nethost.dll libnethost.so libnethost.dylib
	HINTS "${_jce_host_pack}"
	NO_DEFAULT_PATH)
if(NOT JCE_NETHOST_LIBRARY OR NOT JCE_NETHOST_RUNTIME)
	message(STATUS
		"JCE scripting: C# backend SKIPPED — ${_jce_host_pack} has nethost.h "
		"but not both the import library and the runtime beside it.")
	return()
endif()
mark_as_advanced(JCE_NETHOST_LIBRARY JCE_NETHOST_RUNTIME)

add_library(jce_script_vm_csharp STATIC
	"${CMAKE_CURRENT_SOURCE_DIR}/src/jce_script_vm_csharp.c")
set_target_properties(jce_script_vm_csharp PROPERTIES
	C_STANDARD 99 C_STANDARD_REQUIRED YES C_EXTENSIONS OFF)
target_include_directories(jce_script_vm_csharp
	PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
	PRIVATE "${_jce_host_pack}")
target_link_libraries(jce_script_vm_csharp PUBLIC "${JCE_NETHOST_LIBRARY}")
# jce_platform carries jce_library_* (this file loads hostfxr through it) and
# jce_fs_host_*; jce_script carries the registry.  Named explicitly rather than
# left to jce_core, which is what scripting/java does and for the same reason:
# a transitive link that happens to work today is a link nobody declared.
target_link_libraries(jce_script_vm_csharp
	PUBLIC  jce_core jce_script
	PRIVATE jce_platform)
jce_target_warnings(jce_script_vm_csharp STRICT)

# Published for the SDK install rules and for build_csharp.py, which needs the
# same answer and must not compute a second one.
set(JCE_DOTNET_HOST_PACK "${_jce_host_pack}"
	CACHE INTERNAL "The .NET app-host pack the C# backend was built against")
set(JCE_DOTNET_HOST_PACK_VERSION "${_jce_host_pack_version}"
	CACHE INTERNAL "Its version")

# Where build_csharp.py puts the managed assembly.  A CACHE variable and not a
# computed path in three places: the install rule, the smoke and the tests all
# need to agree on it, and three spellings is how they stop agreeing.
set(JCE_SCRIPT_CSHARP_ASSEMBLY
	"${CMAKE_CURRENT_SOURCE_DIR}/managed/artifacts/JceScript.dll"
	CACHE INTERNAL "The managed JceScript assembly build_csharp.py produces")
set(JCE_SCRIPT_CSHARP_NETHOST "${JCE_NETHOST_RUNTIME}"
	CACHE INTERNAL "nethost's runtime library, which must be staged")
set(JCE_SCRIPT_CSHARP_RUNTIMECONFIG
	"${CMAKE_CURRENT_SOURCE_DIR}/managed/artifacts/JceScript.runtimeconfig.json"
	CACHE INTERNAL "Its runtimeconfig, which hostfxr needs to pick a runtime")

# BUILT HERE, ONCE -- same shape as scripting/java's class compilation.
#
# build_csharp.py deliberately owns the managed build (see its header: wiring
# `dotnet build` into the native build would make the .NET SDK a hard
# dependency of `cmake --build`, which it is not).  But "not wired into the
# native build" had become "not wired into ANYTHING": nothing in the tree ran
# it, so `python scripts/jce.py sdk` -- the one command whose job is to produce
# an SDK -- produced one with jce_script_vm_csharp.lib, the header and the
# cmake backend registration all installed, and no managed assembly.  The
# consumer links, runs, and then refuses to register .cs.  tests/sdk_smoke's
# configure said so out loud ("this SDK ships the native host but not the
# managed assembly ... This is a gap in the SDK") and passed anyway.
#
# dotnet is NOT a requirement of the engine build.  Absent, the assembly is
# simply not produced and JCE_SCRIPT_CSHARP_ASSEMBLY points at a file that does
# not exist -- which every consumer already tests with EXISTS and reports by
# name.  Exactly the javac shape.
find_package(Python3 COMPONENTS Interpreter QUIET)
find_program(JCE_DOTNET_EXECUTABLE dotnet
	DOC "dotnet SDK used to build the managed JceScript assembly")
mark_as_advanced(JCE_DOTNET_EXECUTABLE)
if(Python3_Interpreter_FOUND AND JCE_DOTNET_EXECUTABLE)
	file(GLOB_RECURSE _jce_cs_srcs CONFIGURE_DEPENDS
		"${CMAKE_CURRENT_SOURCE_DIR}/managed/JceScript/*.cs"
		"${CMAKE_CURRENT_SOURCE_DIR}/managed/JceScript/*.csproj")
	add_custom_command(
		OUTPUT  "${JCE_SCRIPT_CSHARP_ASSEMBLY}"
		        "${JCE_SCRIPT_CSHARP_RUNTIMECONFIG}"
		COMMAND "${Python3_EXECUTABLE}"
		        "${CMAKE_CURRENT_SOURCE_DIR}/build_csharp.py"
		DEPENDS ${_jce_cs_srcs}
		COMMENT "Building the managed JceScript assembly (dotnet)"
		VERBATIM)
	# NOTHING LINKS THIS, so the build edge has to be an explicit target that
	# consumers add_dependencies() on -- the same trap the Java classes carry.
	add_custom_target(jce_script_csharp_assembly
		DEPENDS "${JCE_SCRIPT_CSHARP_ASSEMBLY}")
	add_dependencies(jce_script_vm_csharp jce_script_csharp_assembly)
	unset(_jce_cs_srcs)
else()
	message(STATUS
		"JCE scripting: the managed JceScript assembly will NOT be built -- "
		"Python3 interpreter: ${Python3_Interpreter_FOUND}, dotnet: "
		"${JCE_DOTNET_EXECUTABLE}. The native half still builds; a .cs script "
		"would reach hostfxr and fail to load the assembly, so consumers report "
		"the missing assembly by name instead of registering.")
endif()

message(STATUS
	"JCE scripting: C# backend ON (.NET app-host ${_jce_host_pack_version}, .cs)")
