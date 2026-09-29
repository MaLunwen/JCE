# jce_backend.cmake — python's entry in the scripting roster.
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
# this engine have python", which is a property of the source tree; whether
# jce_script_vm_python exists is a separate question this file does not touch.
# Collapsing the two would turn "your build lacks python" into "python is not
# a language", and those have different fixes.
#
# Plain set() only: this file is include()d into whatever scope asks, so it must
# define no functions and must not depend on being read from any one directory.

set(JCE_BACKEND_LANGUAGE   "python")
set(JCE_BACKEND_VM_TARGET  "jce_script_vm_python")

set(JCE_BACKEND_EXTENSIONS "py")
set(JCE_BACKEND_NEEDS_SCRIPT_API ON)

# ABSENT BY DESIGN, not by omission -- the systems where this backend cannot
# exist however well the machine is equipped.  Web has neither a process
# CPython nor a loadable ScriptApi shared library for the ctypes package, so
# scripting/python/CMakeLists.txt returns early for Emscripten on purpose.
#
# This line is read by jce_script_enable()'s dist gate, which otherwise refuses
# any dist build missing a language the engine has.  Without it that gate would
# have to choose between refusing every Web dist build and accepting a desktop
# dist that silently lost Python -- and the second is the defect it exists for.
# A platform absence and a missing toolchain look identical from the outside;
# only the backend knows which one it is, so the backend is what says so.
set(JCE_BACKEND_UNAVAILABLE_ON "Emscripten")

# THE RUNTIME INPUT THIS BACKEND CANNOT DISCOVER: the directory holding the
# jce_script package.  Without it `import jce_script.vm` fails and instantiate
# returns 0 -- the script is silently absent, not degraded.  Named here so the
# generator can bake it, and declared REQUIRED so a build that has the backend
# and not the path fails loudly instead of at the first .py.
set(JCE_BACKEND_RUNTIME_VARS "JCE_SCRIPT_PYTHON_PACKAGE_DIR")

# Python3::Python is supplied by the consumer's matching CPython install.
# Stage its runtime beside the executable just like the other by-name
# libraries; on Windows, linking the import library without pythonXY.dll makes
# the process fail before JCE can log an error.
set(JCE_BACKEND_STAGE_TARGETS "Python3::Python")

# The jce_script PACKAGE is a directory, and it is the one runtime input
# python cannot start without -- but staging only ever handled files, so it
# was reached solely through the configure-time path, which points into the
# SDK install and does not exist on a player machine.  The destination name
# is fixed by JCE_SE_PYTHON_PACKAGE_DIR in
# scripting/cmake/jce_script_register_linked.c.in.
if(JCE_SCRIPT_PYTHON_PACKAGE_DIR)
	set(JCE_BACKEND_STAGE_DIRS
		"${JCE_SCRIPT_PYTHON_PACKAGE_DIR}|jce_script_python")
endif()

set(JCE_BACKEND_REGISTER_INCLUDE "#include \"jce_script_vm_python.h\"")
set(JCE_BACKEND_REGISTER_FRAGMENT "register.c.in")

# NOT a "simple" import: this backend's SDK declaration needs more than an
# archive plus JCE::JCE (a version-matched find_package, an extra INTERFACE
# target, or a MODULE the JVM loads), so JCEScripting.cmake.in keeps a
# hand-written block for it.  A backend that IS just an archive sets this ON
# and needs no block at all.
set(JCE_BACKEND_SDK_IMPORT_SIMPLE OFF)

# Without the jce_script package the backend links and then fails at the first
# script with ImportError -- and JCEScripting.cmake already refuses to declare
# the component without it, so the shipped-report must ask the same question.
set(JCE_BACKEND_SDK_REQUIRES_DIR "share/jce/scripting/python/jce_script")
