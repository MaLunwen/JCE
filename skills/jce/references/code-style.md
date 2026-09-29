# Code style

Follow AGENTS.md and the closest module charter. Engine code is C99; editor
code is C++20. Sanctioned C++ dependency bridges have named owners in
tools/lint/engine_cxx_bridge_allow.txt. A new exception needs review.

Use jce_<module>_<verb> functions, JceType types and uppercase constants with
the JCE prefix. Keep public headers
self-contained and expose APIs through their owning umbrella. Use jce_math,
jce allocation, filesystem, platform and task wrappers in engine code.
Do not copy platform conditionals, allocators or a service implementation.

One responsibility per translation unit. The soft size limit is 2000 lines,
the hard limit is 3000; check_file_size.py records the current exceptions.
Plan a responsibility split when touching a large file, instead of inflating
a baseline to hide growth. Comments explain intent or a non-obvious constraint.

Owned text uses LF, except BAT/CMD use CRLF on disk and LF in Git. The Git
attributes are authoritative; .editorconfig keeps normal editor writes aligned.
Never normalize, reformat or repair original upstream copies and binary assets.

Run python scripts/jce.py lint for structural rules and architecture checks.
New gates require a negative control as well as a green repository run.
Public-header changes require rebuilding the SDK and checking a real consumer.
