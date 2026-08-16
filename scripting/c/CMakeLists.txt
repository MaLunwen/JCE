cmake_minimum_required(VERSION 3.20)

# ------------------------------------------------------------------ #
#  scripting/c — C as a DRIVER language.                              #
#                                                                     #
#  Two targets, and neither is a copy of scripting/cpp:               #
#                                                                     #
#    jce_script_vm_c        CALL UP.  The "c" JceScriptVM: one file,  #
#                           which registers a second LANGUAGE over    #
#                           the native class registry scripting/cpp   #
#                           already owns.                             #
#    jce_script_c_module    What a C MODULE AUTHOR links: headers     #
#                           only, never the registry.                 #
#                                                                     #
#  There is no third target for a call-DOWN wrapper, and that is not  #
#  an omission.  The C binding of the script-facing surface IS        #
#  scripting/c_abi's generated <jce/script_api/jce_script_api.h> —    #
#  plain C99, opaque handle, one exported symbol per manifest entry.  #
#  A C script includes that directly; there is nothing to wrap.       #
#  (scripting/cpp's wrapper exists because C++ wanted RAII and        #
#  std::optional over the same ABI.)                                  #
# ------------------------------------------------------------------ #

# ================================================================== #
#  jce_script_vm_c                                                    #
# ================================================================== #

add_library(jce_script_vm_c STATIC
	"${CMAKE_CURRENT_SOURCE_DIR}/src/jce_script_vm_c.c")

set_target_properties(jce_script_vm_c PROPERTIES
	C_STANDARD          99
	C_STANDARD_REQUIRED YES
	C_EXTENSIONS        OFF)

target_include_directories(jce_script_vm_c
	PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

# ── NO `if(TARGET jce_script_vm_cpp)` HERE, ON PURPOSE ──────────────
#
# scripting/CMakeLists.txt discovers its backends by GLOB and adds them in
# glob order, so this directory ("c") is configured BEFORE "cpp" and
# `if(TARGET jce_script_vm_cpp)` reads FALSE at this point — while the target
# is perfectly real one add_subdirectory() later.  That is the same failure
# that made the editor link no scripting backends at all (a7a2dbfc) and that
# scripting/cmake/JCEScriptingInstall.cmake is placed last to avoid.  A guard
# here would not protect anything; it would just be wrong every time.
#
# target_link_libraries() does NOT need the target to exist yet: the name is
# resolved at generate time, and CMake errors if it is never defined.  That
# is the property being relied on, stated so nobody "fixes" it into a guard.
#
# PUBLIC, not PRIVATE: this backend's header INCLUDES
# <jce/script_vm/jce_script_vm_cpp.h> — the two languages share one ABI by
# construction — so a consumer that links this one needs that include path and
# that archive.  A private link would hand them a header they cannot compile.
target_link_libraries(jce_script_vm_c PUBLIC jce_script_vm_cpp)

jce_target_warnings(jce_script_vm_c STRICT)

# ── The VM source must actually reach the compiler ──────────────────
#
# Same assertion, and the same reasoning, as scripting/cpp's: a target that
# built with no VM in it would configure, compile and link, and every
# jce_script_vm_create("c") would return NULL at runtime with "no script VM
# registered for language 'c'" — which reads like a caller mistake.
get_target_property(_jce_vm_c_srcs jce_script_vm_c SOURCES)
if(NOT _jce_vm_c_srcs MATCHES "jce_script_vm_c\\.c")
	message(FATAL_ERROR
		"jce_script_vm_c has no jce_script_vm_c.c in its sources — the c "
		"JceScriptVM would not exist and the failure would surface as a NULL "
		"from jce_script_vm_create(\"c\").")
endif()
unset(_jce_vm_c_srcs)

# ================================================================== #
#  jce_script_c_module — what a C SCRIPT MODULE links.                #
#                                                                    #
#  INTERFACE, and it deliberately does NOT link jce_script_vm_c or    #
#  jce_script_vm_cpp.  A module must not carry the registry: the      #
#  registry is per-process state that belongs to the HOST, and a      #
#  plugin that linked its own copy would publish its classes into a   #
#  registry the engine never reads — a module that loads, reports     #
#  success, and whose classes no instantiate() can find.  Exactly the #
#  rule scripting/cpp states for jce_script_cpp_module.               #
# ================================================================== #

add_library(jce_script_c_module INTERFACE)

target_include_directories(jce_script_c_module INTERFACE
	"${CMAKE_CURRENT_SOURCE_DIR}/include"
	"${CMAKE_CURRENT_SOURCE_DIR}/../cpp/include"
	"${CMAKE_SOURCE_DIR}/engine/include")

# The call-DOWN half.  c_abi is added by NAME before the backend glob in
# scripting/CMakeLists.txt, so unlike the cpp link above this `if(TARGET)`
# is asking a question that is already answerable — and it prints from BOTH
# branches, because a module author who silently got no jce_script_api would
# discover it as an unresolved jce_script_api_open at their own link step.
if(TARGET jce_script_api)
	target_link_libraries(jce_script_c_module INTERFACE jce_script_api)
	message(STATUS
		"JCE scripting: C module target links jce_script_api (a C script "
		"calls the engine through <jce/script_api/jce_script_api.h>).")
else()
	message(STATUS
		"JCE scripting: C module target has NO jce_script_api "
		"(-DJCE_BUILD_SCRIPT_API_SHARED=OFF). jce_script_vm_c is still built; "
		"a C script module can be instantiated and stepped, and it cannot "
		"call the engine.")
endif()
