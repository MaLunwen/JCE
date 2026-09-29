# jce_backend.cmake — java's entry in the scripting roster.
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
# this engine have java", which is a property of the source tree; whether
# jce_script_vm_java exists is a separate question this file does not touch.
# Collapsing the two would turn "your build lacks java" into "java is not
# a language", and those have different fixes.
#
# Plain set() only: this file is include()d into whatever scope asks, so it must
# define no functions and must not depend on being read from any one directory.

set(JCE_BACKEND_LANGUAGE   "java")
set(JCE_BACKEND_VM_TARGET  "jce_script_vm_java")

# TWO extensions, and they are not interchangeable: engine/src/resource/
# jce_archive_cook.c dispatches compression on the catalog's `form`, so .java is
# SOURCE (joins the shared text dictionary) and .class is BYTECODE (opaque).
set(JCE_BACKEND_EXTENSIONS "java;class")
set(JCE_BACKEND_NEEDS_SCRIPT_API ON)

# Three runtime inputs, none discoverable by the backend: an ABSOLUTE jvm
# library (jce_library_open attaches to an already-resident module of the same
# BARE name on Windows, so configure() refuses a bare one), the class path
# holding com.jce.script, and the absolute path of the JNI shim.
set(JCE_BACKEND_RUNTIME_VARS
    "JCE_SCRIPT_JAVA_JVM_LIBRARY;JCE_SCRIPT_JAVA_CLASS_PATH")

# OPTIONAL: a fallback the shim only reaches if its own runtime lookup fails.
# The JNI shim is resolved BESIDE THE EXECUTABLE first, because that is where
# staging puts it and the only path that exists on a player's machine; the
# baked one is the in-tree convenience.  Empty here is not a defect, so it must
# not warn -- a warning nobody can act on is a warning people learn to skip.
set(JCE_BACKEND_RUNTIME_VARS_OPTIONAL "JCE_SCRIPT_JAVA_JNI_LIBRARY")

set(JCE_BACKEND_REGISTER_INCLUDE "#include <jce/script_vm/jce_script_vm_java.h>")
set(JCE_BACKEND_REGISTER_FRAGMENT "register.c.in")

# NOT a "simple" import: this backend's SDK declaration needs more than an
# archive plus JCE::JCE (a version-matched find_package, an extra INTERFACE
# target, or a MODULE the JVM loads), so JCEScripting.cmake.in keeps a
# hand-written block for it.  A backend that IS just an archive sets this ON
# and needs no block at all.
set(JCE_BACKEND_SDK_IMPORT_SIMPLE OFF)

# The JNI shim is LOADED BY THE JVM, never linked, so nothing drags it along:
# it must be copied beside the consumer's executable or the JVM cannot find it.
# A missing one is an UnsatisfiedLinkError inside the JVM, far from the cause.
set(JCE_BACKEND_STAGE_TARGETS "jce_script_java")

# ...and so must the CLASSES.  The shim was staged beside the exe and the
# class tree was not, so a packaged game found the .dll and then failed on
# the class path -- it started, exited 0, and refused every .java by name.
# Directories need their own list: copy_if_different does not do them.
# The destination name is fixed by JCE_SE_JAVA_CLASSES_DIR in
# scripting/cmake/jce_script_register_linked.c.in, which is what looks for
# it beside the executable.
if(JCE_SCRIPT_JAVA_CLASS_PATH)
	set(JCE_BACKEND_STAGE_DIRS
		"${JCE_SCRIPT_JAVA_CLASS_PATH}|jce_script_java_classes")
endif()

# WHAT ELSE MUST BE IN AN SDK for this backend to be usable.  The archive alone
# produces UnsatisfiedLinkError (no shim) or ClassNotFoundException (no
# classes) inside the CONSUMER's JVM, a long way from the SDK that omitted them.
set(JCE_BACKEND_SDK_REQUIRES_SHARED "jce_script_java")
set(JCE_BACKEND_SDK_REQUIRES_DIR "share/jce/scripting/java/classes")
