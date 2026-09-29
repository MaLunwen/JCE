# jce_backend.cmake — c's entry in the scripting roster.
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
# this engine have c", which is a property of the source tree; whether
# jce_script_vm_c exists is a separate question this file does not touch.
# Collapsing the two would turn "your build lacks c" into "c is not
# a language", and those have different fixes.
#
# Plain set() only: this file is include()d into whatever scope asks, so it must
# define no functions and must not depend on being read from any one directory.

set(JCE_BACKEND_LANGUAGE   "c")
set(JCE_BACKEND_VM_TARGET  "jce_script_vm_c")

# The extensions this backend claims at run time.  Cross-checked against the
# OFFLINE catalog in engine/src/resource/jce_asset_ext.c by
# tools/audit/check_script_language_catalog.py -- the catalog is what the
# cooker, the bundle manifest and the editor's Script picker read, and it is
# deliberately unconditional: a cooker with no C backend linked still has to
# know that turret.jcec is C in order to pack it for a runtime that does.
set(JCE_BACKEND_EXTENSIONS "jcec")

# A C script reaches the engine only through scripting/c_abi.
set(JCE_BACKEND_NEEDS_SCRIPT_API ON)

# Compiles native MODULES into the host binary, so the host must suppress the
# shared-object entry point -- JCE_C_MODULE_END and JCE_CPP_MODULE_END both
# export the one symbol a plugin loader looks up.
set(JCE_BACKEND_HAS_NATIVE_MODULES ON)

set(JCE_BACKEND_REGISTER_INCLUDE "#include <jce/script_vm/jce_script_vm_c.h>")
set(JCE_BACKEND_REGISTER_FRAGMENT "register.c.in")

# NOT a "simple" import: this backend's SDK declaration needs more than an
# archive plus JCE::JCE (a version-matched find_package, an extra INTERFACE
# target, or a MODULE the JVM loads), so JCEScripting.cmake.in keeps a
# hand-written block for it.  A backend that IS just an archive sets this ON
# and needs no block at all.
set(JCE_BACKEND_SDK_IMPORT_SIMPLE OFF)

# The C VM derives its table from the C++ VM and calls its native-class
# registry.  Shipping only jce_script_vm_c therefore produces a component
# that configures but cannot link in an external consumer.
set(JCE_BACKEND_SDK_REQUIRES_LINK "jce_script_vm_cpp")
