# jce_backend.cmake — cpp's entry in the scripting roster.
#
# DISCOVERED, not listed anywhere.  scripting/cmake/JCEScriptEnable.cmake globs
# scripting/*/jce_backend.cmake (in-tree) or lib/cmake/JCE/backends/*.cmake
# (from an installed SDK), so the BUILD layer needs no central edit.
#
# THAT IS THE BUILD LAYER ONLY, and the qualification matters.  There is a
# SECOND, independent discovery seam: tools/scriptgen/scriptgen_core.py's
# discover_backends() globs tools/scriptgen/emit_*.py for the module-level
# BACKEND that generates a language's bindings to the engine.  A language whose
# scripts call the engine at all needs one there too.  Both seams are globs and
# neither needs a central file edited, but "add a directory" is the build half
# of the job, not the whole of it -- an earlier version of this comment said
# otherwise and a reader would have believed it.
#
# PRESENT EVEN WHEN THE BACKEND CANNOT BE BUILT HERE.  The roster answers "does
# this engine have cpp", which is a property of the source tree; whether
# jce_script_vm_cpp exists is a separate question this file does not touch.
# Collapsing the two would turn "your build lacks cpp" into "cpp is not
# a language", and those have different fixes.
#
# Plain set() only: this file is include()d into whatever scope asks, so it must
# define no functions and must not depend on being read from any one directory.

set(JCE_BACKEND_LANGUAGE   "cpp")
set(JCE_BACKEND_VM_TARGET  "jce_script_vm_cpp")

set(JCE_BACKEND_EXTENSIONS "jcecpp")
set(JCE_BACKEND_NEEDS_SCRIPT_API ON)
set(JCE_BACKEND_HAS_NATIVE_MODULES ON)

# jce_script_cpp.hpp opens with #error "requires C++17 or later", so the
# standard is part of the contract and not the consumer's problem.  Consumers
# reached for set_source_files_properties(... CXX_STANDARD 20), which is a
# silent no-op: CXX_STANDARD is a TARGET property.
set(JCE_BACKEND_CXX_STANDARD 17)

set(JCE_BACKEND_REGISTER_INCLUDE "#include <jce/script_vm/jce_script_vm_cpp.h>")
set(JCE_BACKEND_REGISTER_FRAGMENT "register.c.in")

# NOT a "simple" import: this backend's SDK declaration needs more than an
# archive plus JCE::JCE (a version-matched find_package, an extra INTERFACE
# target, or a MODULE the JVM loads), so JCEScripting.cmake.in keeps a
# hand-written block for it.  A backend that IS just an archive sets this ON
# and needs no block at all.
set(JCE_BACKEND_SDK_IMPORT_SIMPLE OFF)
