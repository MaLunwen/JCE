# engine/java/ — Desktop JNI Bridge (JVM binding)

## Identity
- **Layer**: consumer-side binding that sits **above** the engine (it drives the public `jce_engine_*` API). The C half currently still compiles into `jce_platform` — see the layering-inversion note at the top of `engine/src/os/platform/jce_jni_bridge.c`.
- **Language**: **Java 11** (no preview features, no third-party deps).
- **Packaged**: `scripts/package-jni-jar.bat` → fat JAR with `natives/<os>-<arch>/` + `game_assets.pak`.
- **Android is NOT this**: the Android build has its own gradle project and JNI surface; nothing here is compiled for it.

## File map
- `com/jce/JceRuntime.java` — the binding: native-library extraction/loading, ABI handshake, and the four engine entry points (`nativeCreate`, `nativeIterate`, `nativeLastResult`, `nativeDestroy`) plus `nativeApiVersion`/`nativeApiVersionString`.
- `com/jce/Main.java` — reference `main()` for a standalone JAR (macOS `-XstartOnFirstThread` relaunch, frame loop, exit code).

## Rules
1. **Pure forwarding only.** Zero gameplay/logic in Java. Every method body either calls a `native` declaration or marshals a value to/from C.
2. **All native methods declared here** must have a matching `Java_com_jce_JceRuntime_*` definition in `engine/src/os/platform/jce_jni_bridge.c` (guarded by `JCE_BUILD_JNI`). Change one side, change the other in the same commit — signature drift is a runtime `UnsatisfiedLinkError`, not a compile error.
3. **ABI handshake is mandatory.** `JceRuntime`'s static initializer calls `nativeApiVersion()` and rejects a mismatch against `EXPECTED_API_VERSION`. Bump that constant in lockstep with `project(JCE VERSION ...)` in the root `CMakeLists.txt`.
4. **`JceRuntime.Result` mirrors `JceAppResult`** (`jce_engine.h`) ordinal-for-ordinal. Never collapse it back into a boolean: callers need to tell a clean quit from an error quit.
5. **No third-party Java deps** — JDK only. No Gradle plugins, no Kotlin.
6. **The bridge knows no game.** The app descriptor arrives through the generic `jce_app_get_desc()` contract (`JCE_MAIN()` in `<jce/application/jce_main.h>`); never reference a specific game's factory.
7. **Config/PAK paths cross via system properties** (`jce.config.path`, `jce.pak.path`) — set in Java, read in C with `System.getProperty`.

## Don't
- Don't add UI widgets here (use RmlUI / ImGui on the native side).
- Don't reference SDL Java classes — SDL is an implementation detail of the native side.
- Don't put `_WIN32`-style constants here; detect the platform via `os.name` / `os.arch` as `detectClassifier()` does.

## Common tasks
- **Expose a new native call** → add `native <type> nativeFoo(...)` here + the `Java_com_jce_JceRuntime_nativeFoo` definition in `engine/src/os/platform/jce_jni_bridge.c`.
- **Rebuild JAR** → `scripts/package-jni-jar.bat`.
