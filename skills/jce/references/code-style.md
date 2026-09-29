# Code and text conventions

First-party engine code is C99; editor code is C++20. C++ dependencies remain behind sanctioned C bridges. Keep public headers self-contained and update the appropriate API umbrella when exposing a header.

Use JCE allocation, filesystem, threading and math wrappers. Platform details belong in engine/src/os/platform/. Keep project-specific behavior out of reusable engine/editor code.

Owned text uses LF on disk and in Git. BAT/CMD files use CRLF on disk and LF in Git. Preserve binary assets and original upstream bytes. .editorconfig and .gitattributes are the authorities; do not infer newline style from an existing mixed file.

The file-size gate enforces a soft 2,000-line and hard 3,000-line limit. Split at a responsibility boundary; do not raise a baseline merely to accommodate growth. Avoid unrelated formatting while fixing behavior.
