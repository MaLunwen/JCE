/* Desktop JNI bridge — exposes the engine to JVM-hosted callers
 * (Java / Kotlin / Scala) on Windows / Linux / macOS.  Android does
 * NOT use this file; the Android build has its own gradle project
 * and JNI surface.
 *
 * ---------------------------------------------------------------------
 * KNOWN LAYERING INVERSION (audit JNI-01) — do NOT copy this pattern.
 * ---------------------------------------------------------------------
 * WHAT DEPENDS ON WHAT: this TU is compiled into `jce_platform` (L2,
 * engine/src/os/platform — see engine/CMakeLists.txt JCE_BUILD_JNI
 * block) purely because of where the file physically sits, yet every
 * line of it is an L6 application concern: it includes
 * <jce/application/jce_engine.h> and calls jce_engine_create /
 * _event / _iterate / _destroy, and it calls jce_api_version() whose
 * definition also lives in L6 (engine/src/application/jce_version.c).
 *
 * WHY THAT IS WRONG: a lower layer must never depend on an upper one.
 * The build only survives today because both layers are static archives
 * that are whole-archived into ONE shared library at the very end (see
 * caged_kingdom/CMakeLists.txt: /WHOLEARCHIVE:jce_platform), so the
 * unresolved L6 symbols get satisfied by the final link instead of by
 * jce_platform's own link interface.  Linking jce_platform on its own
 * (a unit test, a host tool, a dedicated server) breaks the moment
 * JCE_BUILD_JNI is on, and the layer graph no longer describes reality.
 *
 * CONCRETE FIX (deliberately NOT done here — it needs an
 * engine/CMakeLists.txt retarget that is out of scope for this change):
 *   1. Move this file out of engine/src/os/platform/ into its own
 *      consumer-side target, e.g. `jce_jni` under bindings/jni/, added
 *      only when JCE_BUILD_JNI is ON.
 *   2. target_link_libraries(jce_jni PRIVATE JCE) — the INTERFACE
 *      facade — so the bridge sits ABOVE the engine like every other
 *      consumer (games, editor, tests/sdk_smoke) instead of underneath.
 *   3. Drop the JNI include dirs / JCE_BUILD_JNI definitions currently
 *      pushed onto jce_platform and jce_application, and whole-archive
 *      `jce_jni` instead of `jce_platform` in the game's link options —
 *      the JNI entry points are the ONLY reason jce_platform is
 *      force-loaded at all.
 * Cheaper interim step if a new target is too much churn: move the file
 * to engine/src/application/ next to jce_main_sdl.c — it is an alternate
 * app entry point, not platform glue — which already removes the L2->L6
 * edge even though the bridge still ships inside the engine.
 *
 * Until then the coupling is kept as thin as possible: the bridge uses
 * the public, SDL-free engine API only, and knows about no game symbol
 * (the app descriptor arrives through the generic jce_app_get_desc
 * contract that every JCE consumer already implements via JCE_MAIN).
 */
#if defined(JCE_BUILD_JNI)
#include <jce/application/jce_app_interface.h>
#include <jce/application/jce_engine.h>
#include <jce/jce_version.h>

#include "os/core/jce_memory.h"

#include <jni.h>
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

/* The application linked into the same shared library must provide this.
 * It is the SAME generic contract the SDL entry point uses (see
 * engine/src/application/jce_main_sdl.c and <jce/application/jce_main.h>);
 * consumers emit it with JCE_MAIN(my_factory).  Declared as a plain
 * `extern` — never with JCE_API — because the definition lives on the
 * consumer side and must not inherit this TU's dllexport attribute. */
extern JceAppDesc jce_app_get_desc(void);

/* Opaque Java-side handle payload. */
typedef struct JceBridgeEngine {
    JceEngine   *engine;
    JceAppResult last_result;
} JceBridgeEngine;

static JceBridgeEngine *jce_bridge_from_handle(jlong handle)
{
    return (JceBridgeEngine *)(intptr_t)handle;
}

static const char *jce_get_system_property(JNIEnv *env, const char *prop_name,
                                            jstring *out_jstr)
{
    jclass systemClass = (*env)->FindClass(env, "java/lang/System");
    if (!systemClass) return NULL;

    jmethodID getProp = (*env)->GetStaticMethodID(env, systemClass,
        "getProperty", "(Ljava/lang/String;)Ljava/lang/String;");
    if (!getProp) return NULL;

    jstring key = (*env)->NewStringUTF(env, prop_name);
    if (!key) return NULL;

    jstring value = (jstring)(*env)->CallStaticObjectMethod(env, systemClass, getProp, key);
    (*env)->DeleteLocalRef(env, key);

    if (!value) return NULL;

    *out_jstr = value;
    return (*env)->GetStringUTFChars(env, value, NULL);
}

/* ABI handshake.  The Java binding calls these BEFORE anything else and
 * refuses to run against a native library it was not built for — see
 * JceRuntime's static initializer.  Mirrors the C consumer handshake in
 * tests/sdk_smoke/src/main.c. */
JNIEXPORT jint JNICALL Java_com_jce_JceRuntime_nativeApiVersion(JNIEnv *env, jclass clazz)
{
    (void)env;
    (void)clazz;

    return (jint)jce_api_version();
}

JNIEXPORT jstring JNICALL Java_com_jce_JceRuntime_nativeApiVersionString(JNIEnv *env, jclass clazz)
{
    (void)clazz;

    return (*env)->NewStringUTF(env, jce_api_version_string());
}

JNIEXPORT jlong JNICALL Java_com_jce_JceRuntime_nativeCreate(JNIEnv *env, jclass clazz)
{
    (void)clazz;

    jstring configPath = NULL;
    const char *configUtf8 = jce_get_system_property(env, "jce.config.path", &configPath);

    jce_engine_set_config_path(configUtf8);

    /* Read PAK path from Java system property (set by JceRuntime). */
    jstring pakPath = NULL;
    const char *pakUtf8 = jce_get_system_property(env, "jce.pak.path", &pakPath);
    jce_engine_set_pak_path(pakUtf8);

    JceAppDesc desc = jce_app_get_desc();
    jce_engine_set_app_desc(&desc);

    JceBridgeEngine *bridge = (JceBridgeEngine *)JCE_CALLOC(1, sizeof(*bridge));
    if (!bridge) {
        if (configPath && configUtf8)
            (*env)->ReleaseStringUTFChars(env, configPath, configUtf8);
        if (pakPath && pakUtf8)
            (*env)->ReleaseStringUTFChars(env, pakPath, pakUtf8);
        return 0;
    }

    bridge->engine = jce_engine_create(0, NULL);
    if (!bridge->engine) {
        JCE_FREE(bridge);
        if (configPath && configUtf8)
            (*env)->ReleaseStringUTFChars(env, configPath, configUtf8);
        if (pakPath && pakUtf8)
            (*env)->ReleaseStringUTFChars(env, pakPath, pakUtf8);
        return 0;
    }

    if (configPath && configUtf8)
        (*env)->ReleaseStringUTFChars(env, configPath, configUtf8);
    if (pakPath && pakUtf8)
        (*env)->ReleaseStringUTFChars(env, pakPath, pakUtf8);

    bridge->last_result = JCE_APP_CONTINUE;
    return (jlong)(intptr_t)bridge;
}

/* Returns the raw JceAppResult (0 CONTINUE / 1 SUCCESS / 2 FAILURE) — a
 * jboolean would collapse "quit cleanly" and "quit with an error" into the
 * same value and Java could never tell the two apart. */
JNIEXPORT jint JNICALL Java_com_jce_JceRuntime_nativeIterate(JNIEnv *env, jclass clazz, jlong handle)
{
    (void)env;
    (void)clazz;

    JceBridgeEngine *bridge = jce_bridge_from_handle(handle);
    if (!bridge || !bridge->engine) {
        return (jint)JCE_APP_FAILURE;
    }

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        bridge->last_result = jce_engine_event(bridge->engine, &event);
        if (bridge->last_result != JCE_APP_CONTINUE) {
            return (jint)bridge->last_result;
        }
    }

    bridge->last_result = jce_engine_iterate(bridge->engine);
    return (jint)bridge->last_result;
}

/* Last JceAppResult observed by nativeIterate; FAILURE for a dead handle. */
JNIEXPORT jint JNICALL Java_com_jce_JceRuntime_nativeLastResult(JNIEnv *env, jclass clazz, jlong handle)
{
    (void)env;
    (void)clazz;

    JceBridgeEngine *bridge = jce_bridge_from_handle(handle);
    if (!bridge || !bridge->engine) {
        return (jint)JCE_APP_FAILURE;
    }

    return (jint)bridge->last_result;
}

JNIEXPORT void JNICALL Java_com_jce_JceRuntime_nativeDestroy(JNIEnv *env, jclass clazz, jlong handle)
{
    (void)env;
    (void)clazz;

    JceBridgeEngine *bridge = jce_bridge_from_handle(handle);
    if (!bridge) {
        return;
    }

    if (bridge->engine) {
        jce_engine_destroy(bridge->engine);
        bridge->engine = NULL;
    }

    JCE_FREE(bridge);
}


#endif /* JCE_BUILD_JNI */
