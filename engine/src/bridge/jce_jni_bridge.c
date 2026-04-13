#include <stdint.h>
#include <stdbool.h>

#include <jni.h>
#include <SDL3/SDL.h>

#include "core/jce_memory.h"
#include <jce/app/jce_engine.h>
#include <jce/app/jce_app_interface.h>

/* The game (linked into the same shared library) must provide this. */
extern JceAppDesc ck_app_get_desc(void);

/* Opaque Java-side handle payload. */
typedef struct JceBridgeEngine {
    JceEngine *engine;
    SDL_AppResult last_result;
} JceBridgeEngine;

static JceBridgeEngine *jce_bridge_from_handle(jlong handle)
{
    return (JceBridgeEngine *)(intptr_t)handle;
}

static const char *jce_get_config_property(JNIEnv *env, jstring *out_config_jstr)
{
    jclass systemClass = (*env)->FindClass(env, "java/lang/System");
    if (!systemClass) {
        return NULL;
    }

    jmethodID getProp = (*env)->GetStaticMethodID(env, systemClass,
        "getProperty", "(Ljava/lang/String;)Ljava/lang/String;");
    if (!getProp) {
        return NULL;
    }

    jstring key = (*env)->NewStringUTF(env, "jce.config.path");
    if (!key) {
        return NULL;
    }

    jstring value = (jstring)(*env)->CallStaticObjectMethod(env, systemClass, getProp, key);
    (*env)->DeleteLocalRef(env, key);

    if (!value) {
        return NULL;
    }

    *out_config_jstr = value;
    return (*env)->GetStringUTFChars(env, value, NULL);
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

JNIEXPORT jlong JNICALL Java_com_jce_JceRuntime_nativeCreate(JNIEnv *env, jclass clazz)
{
    (void)clazz;

    jstring configPath = NULL;
    const char *configUtf8 = jce_get_config_property(env, &configPath);

    jce_engine_set_config_path(configUtf8);

    /* Read PAK path from Java system property (set by JceRuntime). */
    jstring pakPath = NULL;
    const char *pakUtf8 = jce_get_system_property(env, "jce.pak.path", &pakPath);
    jce_engine_set_pak_path(pakUtf8);

    JceAppDesc desc = ck_app_get_desc();
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

    bridge->last_result = SDL_APP_CONTINUE;
    return (jlong)(intptr_t)bridge;
}

JNIEXPORT jboolean JNICALL Java_com_jce_JceRuntime_nativeIterate(JNIEnv *env, jclass clazz, jlong handle)
{
    (void)env;
    (void)clazz;

    JceBridgeEngine *bridge = jce_bridge_from_handle(handle);
    if (!bridge || !bridge->engine) {
        return JNI_FALSE;
    }

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        bridge->last_result = jce_engine_event(bridge->engine, &event);
        if (bridge->last_result != SDL_APP_CONTINUE) {
            return JNI_FALSE;
        }
    }

    bridge->last_result = jce_engine_iterate(bridge->engine);
    return bridge->last_result == SDL_APP_CONTINUE ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_jce_JceRuntime_nativeShouldQuit(JNIEnv *env, jclass clazz, jlong handle)
{
    (void)env;
    (void)clazz;

    JceBridgeEngine *bridge = jce_bridge_from_handle(handle);
    if (!bridge || !bridge->engine) {
        return JNI_TRUE;
    }

    return bridge->last_result == SDL_APP_CONTINUE ? JNI_FALSE : JNI_TRUE;
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

