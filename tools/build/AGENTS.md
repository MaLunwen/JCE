# tools/build — automated build implementation

jce.py owns platform selection, Conan/CMake ordering, SDK installation and
consumer builds. scripts/jce.py is a compatibility entry point. Shared command
helpers and non-desktop recipes are implemented here; they take explicit
consumer/toolchain input. Never supply a named game's icon, target or content
as a reusable builder's default. fetch_vendor_sources.py only creates new,
hash-verified cache trees; existing originals are verified and never changed.

Android host packaging is `android/`. Original SDL Java comes from the
verified external source cache; no Gradle wrapper or SDL source is tracked.
Use an installed Gradle 8.14.1 and supply SDK/NDK environment settings.

Native Windows single-EXE editor builds are owned by standalone.py and selected
with --standalone --variant dist. Use an isolated Conan static-CRT graph and
build directory; validate ordinary and delayed PE imports before delivery.
Validate embedded notice bytes against their source files before delivery.
SDKs and external managed runtimes are separate products. Do not replace native
single-file execution with a self-extracting wrapper or an embedded SDK bundle.
The explicit language set is Lua/C/C++/JavaScript; the full language build stays
available. Verify relocation with only the EXE and inspect actual captures.
