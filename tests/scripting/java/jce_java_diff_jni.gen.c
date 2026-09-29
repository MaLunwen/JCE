/* jce_java_diff_jni.gen.c -- GENERATED. DO NOT EDIT.
 *
 *   python tools/scriptgen/gen_script_bindings.py --write
 *
 * The test-side JNI helper. It hands the Java driver the ADDRESS of the same
 * recording mock host the Lua reference driver uses, and reads back the trace.
 *
 * It is deliberately separate from scripting/java/native: the production shim
 * must not carry a line of test code, and the Java driver reaches the surface
 * through the production shim exactly as a game would.
 */

#include <jni.h>

#include "jce_java_diff_host.gen.h"

#include <stdint.h>

JNIEXPORT jlong JNICALL Java_com_jce_script_diff_JceDiffHost_fullHostPointer(JNIEnv *env,
                                                         jclass cls)
{
    (void)env;
    (void)cls;
    return (jlong)(intptr_t)jce_java_diff_host_full();
}

JNIEXPORT jlong JNICALL Java_com_jce_script_diff_JceDiffHost_partialHostPointer(JNIEnv *env,
                                                            jclass cls)
{
    (void)env;
    (void)cls;
    return (jlong)(intptr_t)jce_java_diff_host_partial();
}

JNIEXPORT jlong JNICALL Java_com_jce_script_diff_JceDiffHost_hostSize(JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
    return (jlong)jce_java_diff_host_size();
}

JNIEXPORT void JNICALL Java_com_jce_script_diff_JceDiffHost_reset(JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
    jce_java_diff_reset();
}

JNIEXPORT jstring JNICALL Java_com_jce_script_diff_JceDiffHost_trace(JNIEnv *env, jclass cls)
{
    (void)cls;
    return (*env)->NewStringUTF(env, jce_java_diff_trace());
}

JNIEXPORT jint JNICALL Java_com_jce_script_diff_JceDiffHost_caseCount(JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
    return (jint)jce_java_diff_case_count();
}

JNIEXPORT jstring JNICALL Java_com_jce_script_diff_JceDiffHost_caseLabel(JNIEnv *env, jclass cls,
                                                     jint i)
{
    (void)cls;
    return (*env)->NewStringUTF(env, jce_java_diff_case_label((int)i));
}
