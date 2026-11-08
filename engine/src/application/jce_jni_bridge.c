/* Desktop JNI application host for Windows / Linux / macOS, compiled into
 * jce_application beside the SDL host. Java owns the bridge handle; this host
 * drives the public Engine lifecycle, retains deferred cleanup and consumes
 * the private terminal ticket only after Engine/bridge release. The consumer
 * supplies its descriptor through the generic JCE_MAIN contract, not a game
 * symbol. Android has a separate Gradle/JNI surface and does not use this TU. */
#if defined(JCE_BUILD_JNI)
#include <jce/application/jce_app_interface.h>
#include <jce/application/jce_engine.h>
#include <jce/jce_version.h>

#include "os/core/jce_memory.h"
#include "application/jce_engine_terminal_internal.h"

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
    bool closing;
    uint64_t terminal_session;
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
    if ((*env)->ExceptionCheck(env)) return NULL;
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

    jlong handle = 0;
    jstring configPath = NULL;
    jstring pakPath = NULL;
    const char *pakUtf8 = NULL;
    const char *configUtf8 = jce_get_system_property(env, "jce.config.path", &configPath);
    if ((*env)->ExceptionCheck(env)) goto done;

    jce_engine_set_config_path(configUtf8);

    /* Read PAK path from Java system property (set by JceRuntime). */
    pakUtf8 = jce_get_system_property(env, "jce.pak.path", &pakPath);
    if ((*env)->ExceptionCheck(env)) goto done;
    jce_engine_set_pak_path(pakUtf8);

    JceAppDesc desc = jce_app_get_desc();
    jce_engine_set_app_desc(&desc);

    JceBridgeEngine *bridge = (JceBridgeEngine *)JCE_CALLOC(1, sizeof(*bridge));
    if (!bridge) goto done;

    const uint64_t before = jce_engine_terminal_cursor_internal();
    bridge->last_result = jce_engine_create_owned(0, NULL, &bridge->engine);
    bridge->terminal_session = jce_engine_terminal_after_create_internal(before);
    bridge->closing = bridge->last_result != JCE_APP_CONTINUE;
    if (!bridge->engine) {
        const uint64_t session = bridge->terminal_session;
        const int result = bridge->last_result;
        JCE_FREE(bridge);
        (void)jce_engine_terminal_emit_internal(session, "jni", result, true);
        goto done;
    }

    handle = (jlong)(intptr_t)bridge;
done:
    if (configPath && configUtf8)
        (*env)->ReleaseStringUTFChars(env, configPath, configUtf8);
    if (pakPath && pakUtf8)
        (*env)->ReleaseStringUTFChars(env, pakPath, pakUtf8);

    return handle;
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
    if (bridge->closing || bridge->last_result != JCE_APP_CONTINUE)
        return (jint)bridge->last_result;

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

/* Last create/iterate result, including a failed boot with retained ownership. */
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

    bridge->closing = true;
    if (bridge->last_result == JCE_APP_CONTINUE)
        bridge->last_result = JCE_APP_SUCCESS;
    if (!jce_engine_try_destroy(&bridge->engine)) {
        /* The void JNI signature stays compatible. A pending exception skips
         * Java's handle-clear statement, preserving the same retryable owner. */
        jclass error = (*env)->FindClass(env, "java/lang/IllegalStateException");
        if (error) {
            (*env)->ThrowNew(env, error,
                "JCE cleanup deferred; retry close() on the same runtime");
            (*env)->DeleteLocalRef(env, error);
        }
        return;
    }

    const uint64_t session = bridge->terminal_session;
    const int result = bridge->last_result;
    const bool owner_empty = bridge->engine == NULL;
    JCE_FREE(bridge);
    (void)jce_engine_terminal_emit_internal(session, "jni", result, owner_empty);
}


#endif /* JCE_BUILD_JNI */
