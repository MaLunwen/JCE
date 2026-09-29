# Toolchain discovery

Use scripts/jce.py --help and subcommand help for supported arguments. Do not hardcode a developer's compiler, JDK, NDK, SDK, signing identity or asset location in a reusable script.

On Windows, the build driver selects the MSVC environment. Conan profiles own dependency settings; generated toolchains, recipes and package identities must agree with the requested variant and graphics tier. Do not reuse a graph stamp from retired source-patching hooks.

JAVA_HOME and other explicit toolchain inputs are supplied by the caller. Respect CONAN_HOME so local and CI dependency caches can be isolated. Unknown installed JCE hooks must stop synchronization instead of being overwritten.

Use an installed, verified Gradle for Android; original SDL Java is fetched from the fixed upstream cache rather than copied into public source. First-party platform packaging lives under tools/build/, with manual forwarding launchers under scripts/.

The source export, build tree, SDK install and final package are distinct directories. Record which one a command consumes. A compiler discovery or source gate does not prove a platform binary was produced.
