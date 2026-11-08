# Code and text conventions

First-party engine code is C99; editor code is C++20. C++ dependencies remain behind sanctioned C bridges. Keep public headers self-contained and update the appropriate API umbrella when exposing a header.

Use JCE allocation, filesystem, threading and math wrappers. Platform details belong in engine/src/os/platform/. Keep project-specific behavior out of reusable engine/editor code.

Owned text uses LF on disk and in Git. BAT/CMD files use CRLF on disk and LF in Git. Preserve binary assets and original upstream bytes. Git line-ending normalization follows .gitattributes.

The file-size gate enforces a soft 2,000-line and hard 3,000-line limit. Split at a responsibility boundary; do not raise a baseline merely to accommodate growth. Avoid unrelated formatting while fixing behavior.

Use readable US English and ASCII in first-party code/comments where practical;
localized UI text belongs to i18n resources. Follow repository formatting and
readable Linux/Luanti-style conventions. Optional owner-supplied style PDFs do
not become clone prerequisites. Runtime is C99 too; editor remains C++20.
