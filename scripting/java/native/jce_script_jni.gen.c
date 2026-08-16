/* jce_script_jni.gen.c -- GENERATED. DO NOT EDIT.
 *
 *   python tools/scriptgen/gen_script_bindings.py --write
 *
 * The JNI half of com.jce.script.JceScript. One JNIEXPORT per native method,
 * forwarding to the matching jce_script_api_* export. It links the C ABI
 * shared library and NOTHING else -- no engine layer, no Lua.
 *
 * LOCAL REFERENCES. Every native below creates AT MOST ONE object (a jstring)
 * and returns it immediately; out values cross in arrays the CALLER allocated,
 * so nothing is allocated per element and nothing is allocated in a loop.
 *
 * That is checked, not asserted -- but NOT by -Xcheck:jni, which this banner
 * used to credit. Measured: a mutated shim leaking 200,000 local references
 * inside one native frame runs the whole differential GREEN under
 * `java -Xcheck:jni`; that checker reports reference MISUSE, not accumulation.
 * The discipline is enforced statically over this emitted file by
 * test_script_java_gate.py --
 * test_no_native_creates_more_than_one_object,
 * test_no_native_creates_a_reference_that_outlives_the_call and
 * test_the_shim_contains_no_loop (the count is only static while nothing
 * iterates). Each was applied as a mutation and observed red.
 *
 * STRING ARGUMENTS. GetStringUTFChars is paired with ReleaseStringUTFChars on
 * a SINGLE return path -- every function here has exactly one `return`, so a
 * release cannot be skipped by an early exit. When the JVM cannot allocate the
 * chars it returns NULL with an exception pending; the call is then skipped
 * (`ok`) rather than made with a NULL argument the host would have to
 * interpret.
 *
 * 71 entry points plus 3 meta (nativeApiVersion / nativeOpen / nativeClose).
 */

#include <jni.h>

#include <jce/script_api/jce_script_api.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The entity id crosses as a jlong and an entity buffer is handed straight to
 * SetLongArrayRegion. Both are only true while these are the same width. */
typedef char jce_java_entity_fits_jlong[
    (sizeof(JceScriptEntity) == sizeof(jlong)) ? 1 : -1];

static JceScriptApi *jce_java_api(jlong h)
{
    return (JceScriptApi *)(intptr_t)h;
}


JNIEXPORT jint JNICALL Java_com_jce_script_JceScript_nativeApiVersion(JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
    return (jint)jce_script_api_version();
}

JNIEXPORT jlong JNICALL Java_com_jce_script_JceScript_nativeOpen(JNIEnv *env, jclass cls,
    jlong hostPointer, jlong hostSize, jint scriptApiMin)
{
    const JceScriptHost *host = (const JceScriptHost *)(intptr_t)hostPointer;
    JceScriptApi *api;

    (void)env;
    (void)cls;
    api = jce_script_api_open(host, (size_t)hostSize, (uint32_t)scriptApiMin);
    return (jlong)(intptr_t)api;
}

JNIEXPORT void JNICALL Java_com_jce_script_JceScript_nativeClose(JNIEnv *env, jclass cls,
    jlong api)
{
    (void)env;
    (void)cls;
    jce_script_api_close(jce_java_api(api));
}


/* jce.get_position -> jce_script_api_get_position (fallible_out) */
JNIEXPORT jboolean JNICALL Java_com_jce_script_JceScript_nGetPosition(
    JNIEnv *env, jclass cls, jlong api, jlong e, jfloatArray j_outFloat)
{
    jboolean ret = JNI_FALSE;
    float out_xyz[3];
    jfloat outFloat[3];

    (void)cls;
    memset(out_xyz, 0, sizeof out_xyz);
    if (jce_script_api_get_position(jce_java_api(api), (JceScriptEntity)e, out_xyz)) {
        ret = JNI_TRUE;
        outFloat[0] = (jfloat)out_xyz[0];
        outFloat[1] = (jfloat)out_xyz[1];
        outFloat[2] = (jfloat)out_xyz[2];
        if (j_outFloat)
            (*env)->SetFloatArrayRegion(env, j_outFloat, 0, 3, outFloat);
    }
    return ret;
}


/* jce.set_position -> jce_script_api_set_position (void_call) */
JNIEXPORT void JNICALL Java_com_jce_script_JceScript_nSetPosition(
    JNIEnv *env, jclass cls, jlong api, jlong e, jfloat x, jfloat y, jfloat z)
{

    (void)env;
    (void)cls;
    jce_script_api_set_position(jce_java_api(api), (JceScriptEntity)e, (float)x, (float)y, (float)z);
}


/* jce.get_rotation -> jce_script_api_get_rotation (fallible_out) */
JNIEXPORT jboolean JNICALL Java_com_jce_script_JceScript_nGetRotation(
    JNIEnv *env, jclass cls, jlong api, jlong e, jfloatArray j_outFloat)
{
    jboolean ret = JNI_FALSE;
    float out_euler_deg[3];
    jfloat outFloat[3];

    (void)cls;
    memset(out_euler_deg, 0, sizeof out_euler_deg);
    if (jce_script_api_get_rotation(jce_java_api(api), (JceScriptEntity)e, out_euler_deg)) {
        ret = JNI_TRUE;
        outFloat[0] = (jfloat)out_euler_deg[0];
        outFloat[1] = (jfloat)out_euler_deg[1];
        outFloat[2] = (jfloat)out_euler_deg[2];
        if (j_outFloat)
            (*env)->SetFloatArrayRegion(env, j_outFloat, 0, 3, outFloat);
    }
    return ret;
}


/* jce.set_rotation -> jce_script_api_set_rotation (void_call) */
JNIEXPORT void JNICALL Java_com_jce_script_JceScript_nSetRotation(
    JNIEnv *env, jclass cls, jlong api, jlong e, jfloat x, jfloat y, jfloat z)
{

    (void)env;
    (void)cls;
    jce_script_api_set_rotation(jce_java_api(api), (JceScriptEntity)e, (float)x, (float)y, (float)z);
}


/* jce.get_scale -> jce_script_api_get_scale (fallible_out) */
JNIEXPORT jboolean JNICALL Java_com_jce_script_JceScript_nGetScale(
    JNIEnv *env, jclass cls, jlong api, jlong e, jfloatArray j_outFloat)
{
    jboolean ret = JNI_FALSE;
    float out_xyz[3];
    jfloat outFloat[3];

    (void)cls;
    memset(out_xyz, 0, sizeof out_xyz);
    if (jce_script_api_get_scale(jce_java_api(api), (JceScriptEntity)e, out_xyz)) {
        ret = JNI_TRUE;
        outFloat[0] = (jfloat)out_xyz[0];
        outFloat[1] = (jfloat)out_xyz[1];
        outFloat[2] = (jfloat)out_xyz[2];
        if (j_outFloat)
            (*env)->SetFloatArrayRegion(env, j_outFloat, 0, 3, outFloat);
    }
    return ret;
}


/* jce.set_scale -> jce_script_api_set_scale (void_call) */
JNIEXPORT void JNICALL Java_com_jce_script_JceScript_nSetScale(
    JNIEnv *env, jclass cls, jlong api, jlong e, jfloat x, jfloat y, jfloat z)
{

    (void)env;
    (void)cls;
    jce_script_api_set_scale(jce_java_api(api), (JceScriptEntity)e, (float)x, (float)y, (float)z);
}


/* jce.set_parent -> jce_script_api_set_parent (value_return) */
JNIEXPORT jboolean JNICALL Java_com_jce_script_JceScript_nSetParent(
    JNIEnv *env, jclass cls, jlong api, jlong child, jlong parent,
    jboolean preserve_world)
{
    jboolean ret = (jboolean)0;

    (void)env;
    (void)cls;
    ret = (jboolean)jce_script_api_set_parent(jce_java_api(api), (JceScriptEntity)child, (JceScriptEntity)parent, (bool)preserve_world);
    return ret;
}


/* jce.get_parent -> jce_script_api_get_parent (value_return) */
JNIEXPORT jlong JNICALL Java_com_jce_script_JceScript_nGetParent(
    JNIEnv *env, jclass cls, jlong api, jlong child)
{
    jlong ret = (jlong)0;

    (void)env;
    (void)cls;
    ret = (jlong)jce_script_api_get_parent(jce_java_api(api), (JceScriptEntity)child);
    return ret;
}


/* jce.is_key_down -> jce_script_api_is_key_down (value_return) */
JNIEXPORT jboolean JNICALL Java_com_jce_script_JceScript_nIsKeyDown(
    JNIEnv *env, jclass cls, jlong api, jint keycode)
{
    jboolean ret = (jboolean)0;

    (void)env;
    (void)cls;
    ret = (jboolean)jce_script_api_is_key_down(jce_java_api(api), (int)keycode);
    return ret;
}


/* jce.find_with_tag -> jce_script_api_find_with_tag (value_return) */
JNIEXPORT jlong JNICALL Java_com_jce_script_JceScript_nFindWithTag(
    JNIEnv *env, jclass cls, jlong api, jstring j_tag)
{
    jlong ret = (jlong)0;
    const char *tag = NULL;
    int ok = 1;

    (void)cls;
    if (j_tag) {
        tag = (*env)->GetStringUTFChars(env, j_tag, NULL);
        if (!tag)
            ok = 0;
    }
    if (ok) {
        ret = (jlong)jce_script_api_find_with_tag(jce_java_api(api), tag);
    }
    if (tag)
        (*env)->ReleaseStringUTFChars(env, j_tag, tag);
    return ret;
}


/* jce.destroy -> jce_script_api_destroy (void_call) */
JNIEXPORT void JNICALL Java_com_jce_script_JceScript_nDestroy(
    JNIEnv *env, jclass cls, jlong api, jlong e)
{

    (void)env;
    (void)cls;
    jce_script_api_destroy(jce_java_api(api), (JceScriptEntity)e);
}


/* jce.spawn -> jce_script_api_spawn (value_return) */
JNIEXPORT jlong JNICALL Java_com_jce_script_JceScript_nSpawn(
    JNIEnv *env, jclass cls, jlong api, jstring j_prefab_path, jfloat x, jfloat y,
    jfloat z)
{
    jlong ret = (jlong)0;
    const char *prefab_path = NULL;
    int ok = 1;

    (void)cls;
    if (j_prefab_path) {
        prefab_path = (*env)->GetStringUTFChars(env, j_prefab_path, NULL);
        if (!prefab_path)
            ok = 0;
    }
    if (ok) {
        ret = (jlong)jce_script_api_spawn(jce_java_api(api), prefab_path, (float)x, (float)y, (float)z);
    }
    if (prefab_path)
        (*env)->ReleaseStringUTFChars(env, j_prefab_path, prefab_path);
    return ret;
}


/* jce.move_axis -> jce_script_api_move_axis (void_out_array) */
JNIEXPORT void JNICALL Java_com_jce_script_JceScript_nMoveAxis(
    JNIEnv *env, jclass cls, jlong api, jfloatArray j_outFloat)
{
    float out_xz[2];
    jfloat outFloat[2];

    (void)cls;
    memset(out_xz, 0, sizeof out_xz);
    jce_script_api_move_axis(jce_java_api(api), out_xz);
    outFloat[0] = (jfloat)out_xz[0];
    outFloat[1] = (jfloat)out_xz[1];
    if (j_outFloat)
        (*env)->SetFloatArrayRegion(env, j_outFloat, 0, 2, outFloat);
}


/* jce.jump_pressed -> jce_script_api_jump_pressed (value_return) */
JNIEXPORT jboolean JNICALL Java_com_jce_script_JceScript_nJumpPressed(
    JNIEnv *env, jclass cls, jlong api)
{
    jboolean ret = (jboolean)0;

    (void)env;
    (void)cls;
    ret = (jboolean)jce_script_api_jump_pressed(jce_java_api(api));
    return ret;
}


/* jce.sprint -> jce_script_api_sprint (value_return) */
JNIEXPORT jboolean JNICALL Java_com_jce_script_JceScript_nSprint(
    JNIEnv *env, jclass cls, jlong api)
{
    jboolean ret = (jboolean)0;

    (void)env;
    (void)cls;
    ret = (jboolean)jce_script_api_sprint(jce_java_api(api));
    return ret;
}


/* jce.attack_pressed -> jce_script_api_attack_pressed (value_return) */
JNIEXPORT jboolean JNICALL Java_com_jce_script_JceScript_nAttackPressed(
    JNIEnv *env, jclass cls, jlong api)
{
    jboolean ret = (jboolean)0;

    (void)env;
    (void)cls;
    ret = (jboolean)jce_script_api_attack_pressed(jce_java_api(api));
    return ret;
}


/* jce.set_time_scale -> jce_script_api_set_time_scale (void_call) */
JNIEXPORT void JNICALL Java_com_jce_script_JceScript_nSetTimeScale(
    JNIEnv *env, jclass cls, jlong api, jfloat scale)
{

    (void)env;
    (void)cls;
    jce_script_api_set_time_scale(jce_java_api(api), (float)scale);
}


/* jce.pause -> jce_script_api_pause (void_call) */
JNIEXPORT void JNICALL Java_com_jce_script_JceScript_nPause(
    JNIEnv *env, jclass cls, jlong api, jboolean paused)
{

    (void)env;
    (void)cls;
    jce_script_api_pause(jce_java_api(api), (bool)paused);
}


/* jce.shake_camera -> jce_script_api_shake_camera (void_call) */
JNIEXPORT void JNICALL Java_com_jce_script_JceScript_nShakeCamera(
    JNIEnv *env, jclass cls, jlong api, jfloat amount)
{

    (void)env;
    (void)cls;
    jce_script_api_shake_camera(jce_java_api(api), (float)amount);
}


/* jce.music_set_intensity -> jce_script_api_music_set_intensity (void_call) */
JNIEXPORT void JNICALL Java_com_jce_script_JceScript_nMusicSetIntensity(
    JNIEnv *env, jclass cls, jlong api, jfloat intensity)
{

    (void)env;
    (void)cls;
    jce_script_api_music_set_intensity(jce_java_api(api), (float)intensity);
}


/* jce.music_get_intensity -> jce_script_api_music_get_intensity (value_return) */
JNIEXPORT jfloat JNICALL Java_com_jce_script_JceScript_nMusicGetIntensity(
    JNIEnv *env, jclass cls, jlong api)
{
    jfloat ret = (jfloat)0;

    (void)env;
    (void)cls;
    ret = (jfloat)jce_script_api_music_get_intensity(jce_java_api(api));
    return ret;
}


/* jce.music_request_transition -> jce_script_api_music_request_transition (value_return) */
JNIEXPORT jfloat JNICALL Java_com_jce_script_JceScript_nMusicRequestTransition(
    JNIEnv *env, jclass cls, jlong api, jint to_segment)
{
    jfloat ret = (jfloat)0;

    (void)env;
    (void)cls;
    ret = (jfloat)jce_script_api_music_request_transition(jce_java_api(api), (int)to_segment);
    return ret;
}


/* jce.gas_activate -> jce_script_api_gas_activate (value_return) */
JNIEXPORT jboolean JNICALL Java_com_jce_script_JceScript_nGasActivate(
    JNIEnv *env, jclass cls, jlong api, jlong e, jint ability_id)
{
    jboolean ret = (jboolean)0;

    (void)env;
    (void)cls;
    ret = (jboolean)jce_script_api_gas_activate(jce_java_api(api), (JceScriptEntity)e, (uint32_t)ability_id);
    return ret;
}


/* jce.gas_get -> jce_script_api_gas_get (fallible_out) */
JNIEXPORT jboolean JNICALL Java_com_jce_script_JceScript_nGasGet(
    JNIEnv *env, jclass cls, jlong api, jlong e, jstring j_attr_name,
    jfloatArray j_outFloat)
{
    jboolean ret = JNI_FALSE;
    const char *attr_name = NULL;
    float out_value;
    jfloat outFloat[1];
    int ok = 1;

    (void)cls;
    if (j_attr_name) {
        attr_name = (*env)->GetStringUTFChars(env, j_attr_name, NULL);
        if (!attr_name)
            ok = 0;
    }
    memset(&out_value, 0, sizeof out_value);
    if (ok && jce_script_api_gas_get(jce_java_api(api), (JceScriptEntity)e, attr_name, &out_value)) {
        ret = JNI_TRUE;
        outFloat[0] = (jfloat)out_value;
        if (j_outFloat)
            (*env)->SetFloatArrayRegion(env, j_outFloat, 0, 1, outFloat);
    }
    if (attr_name)
        (*env)->ReleaseStringUTFChars(env, j_attr_name, attr_name);
    return ret;
}


/* jce.gas_apply -> jce_script_api_gas_apply (value_return) */
JNIEXPORT jboolean JNICALL Java_com_jce_script_JceScript_nGasApply(
    JNIEnv *env, jclass cls, jlong api, jlong e, jstring j_attr_name, jint op,
    jfloat magnitude, jfloat duration_seconds)
{
    jboolean ret = (jboolean)0;
    const char *attr_name = NULL;
    int ok = 1;

    (void)cls;
    if (j_attr_name) {
        attr_name = (*env)->GetStringUTFChars(env, j_attr_name, NULL);
        if (!attr_name)
            ok = 0;
    }
    if (ok) {
        ret = (jboolean)jce_script_api_gas_apply(jce_java_api(api), (JceScriptEntity)e, attr_name, (int)op, (float)magnitude, (float)duration_seconds);
    }
    if (attr_name)
        (*env)->ReleaseStringUTFChars(env, j_attr_name, attr_name);
    return ret;
}


/* jce.raycast -> jce_script_api_raycast (fallible_out) */
JNIEXPORT jboolean JNICALL Java_com_jce_script_JceScript_nRaycast(
    JNIEnv *env, jclass cls, jlong api, jfloatArray j_origin, jfloatArray j_dir,
    jfloat max_dist, jlongArray j_outLong, jfloatArray j_outFloat)
{
    jboolean ret = JNI_FALSE;
    float origin[3];
    float dir[3];
    JceScriptRaycastHit out;
    jlong outLong[1];
    jfloat outFloat[7];

    (void)cls;
    memset(origin, 0, sizeof origin);
    if (j_origin)
        (*env)->GetFloatArrayRegion(env, j_origin, 0, 3, (jfloat *)origin);
    memset(dir, 0, sizeof dir);
    if (j_dir)
        (*env)->GetFloatArrayRegion(env, j_dir, 0, 3, (jfloat *)dir);
    memset(&out, 0, sizeof out);
    if (jce_script_api_raycast(jce_java_api(api), origin, dir, (float)max_dist, &out)) {
        ret = JNI_TRUE;
        outLong[0] = (jlong)out.entity;
        if (j_outLong)
            (*env)->SetLongArrayRegion(env, j_outLong, 0, 1, outLong);
        outFloat[0] = (jfloat)out.point[0];
        outFloat[1] = (jfloat)out.point[1];
        outFloat[2] = (jfloat)out.point[2];
        outFloat[3] = (jfloat)out.normal[0];
        outFloat[4] = (jfloat)out.normal[1];
        outFloat[5] = (jfloat)out.normal[2];
        outFloat[6] = (jfloat)out.distance;
        if (j_outFloat)
            (*env)->SetFloatArrayRegion(env, j_outFloat, 0, 7, outFloat);
    }
    return ret;
}


/* jce.apply_impulse -> jce_script_api_apply_impulse (void_call) */
JNIEXPORT void JNICALL Java_com_jce_script_JceScript_nApplyImpulse(
    JNIEnv *env, jclass cls, jlong api, jlong e, jfloat x, jfloat y, jfloat z)
{

    (void)env;
    (void)cls;
    jce_script_api_apply_impulse(jce_java_api(api), (JceScriptEntity)e, (float)x, (float)y, (float)z);
}


/* jce.set_velocity -> jce_script_api_set_velocity (void_call) */
JNIEXPORT void JNICALL Java_com_jce_script_JceScript_nSetVelocity(
    JNIEnv *env, jclass cls, jlong api, jlong e, jfloat x, jfloat y, jfloat z)
{

    (void)env;
    (void)cls;
    jce_script_api_set_velocity(jce_java_api(api), (JceScriptEntity)e, (float)x, (float)y, (float)z);
}


/* jce.anim_set_float -> jce_script_api_anim_set_float (void_call) */
JNIEXPORT void JNICALL Java_com_jce_script_JceScript_nAnimSetFloat(
    JNIEnv *env, jclass cls, jlong api, jlong e, jstring j_name, jfloat v)
{
    const char *name = NULL;
    int ok = 1;

    (void)cls;
    if (j_name) {
        name = (*env)->GetStringUTFChars(env, j_name, NULL);
        if (!name)
            ok = 0;
    }
    if (ok) {
        jce_script_api_anim_set_float(jce_java_api(api), (JceScriptEntity)e, name, (float)v);
    }
    if (name)
        (*env)->ReleaseStringUTFChars(env, j_name, name);
}


/* jce.anim_set_int -> jce_script_api_anim_set_int (void_call) */
JNIEXPORT void JNICALL Java_com_jce_script_JceScript_nAnimSetInt(
    JNIEnv *env, jclass cls, jlong api, jlong e, jstring j_name, jint v)
{
    const char *name = NULL;
    int ok = 1;

    (void)cls;
    if (j_name) {
        name = (*env)->GetStringUTFChars(env, j_name, NULL);
        if (!name)
            ok = 0;
    }
    if (ok) {
        jce_script_api_anim_set_int(jce_java_api(api), (JceScriptEntity)e, name, (int)v);
    }
    if (name)
        (*env)->ReleaseStringUTFChars(env, j_name, name);
}


/* jce.anim_set_bool -> jce_script_api_anim_set_bool (void_call) */
JNIEXPORT void JNICALL Java_com_jce_script_JceScript_nAnimSetBool(
    JNIEnv *env, jclass cls, jlong api, jlong e, jstring j_name, jboolean v)
{
    const char *name = NULL;
    int ok = 1;

    (void)cls;
    if (j_name) {
        name = (*env)->GetStringUTFChars(env, j_name, NULL);
        if (!name)
            ok = 0;
    }
    if (ok) {
        jce_script_api_anim_set_bool(jce_java_api(api), (JceScriptEntity)e, name, (bool)v);
    }
    if (name)
        (*env)->ReleaseStringUTFChars(env, j_name, name);
}


/* jce.anim_set_trigger -> jce_script_api_anim_set_trigger (void_call) */
JNIEXPORT void JNICALL Java_com_jce_script_JceScript_nAnimSetTrigger(
    JNIEnv *env, jclass cls, jlong api, jlong e, jstring j_name)
{
    const char *name = NULL;
    int ok = 1;

    (void)cls;
    if (j_name) {
        name = (*env)->GetStringUTFChars(env, j_name, NULL);
        if (!name)
            ok = 0;
    }
    if (ok) {
        jce_script_api_anim_set_trigger(jce_java_api(api), (JceScriptEntity)e, name);
    }
    if (name)
        (*env)->ReleaseStringUTFChars(env, j_name, name);
}


/* jce.is_action_down -> jce_script_api_is_action_down (value_return) */
JNIEXPORT jboolean JNICALL Java_com_jce_script_JceScript_nIsActionDown(
    JNIEnv *env, jclass cls, jlong api, jstring j_name)
{
    jboolean ret = (jboolean)0;
    const char *name = NULL;
    int ok = 1;

    (void)cls;
    if (j_name) {
        name = (*env)->GetStringUTFChars(env, j_name, NULL);
        if (!name)
            ok = 0;
    }
    if (ok) {
        ret = (jboolean)jce_script_api_is_action_down(jce_java_api(api), name);
    }
    if (name)
        (*env)->ReleaseStringUTFChars(env, j_name, name);
    return ret;
}


/* jce.is_action_pressed -> jce_script_api_is_action_pressed (value_return) */
JNIEXPORT jboolean JNICALL Java_com_jce_script_JceScript_nIsActionPressed(
    JNIEnv *env, jclass cls, jlong api, jstring j_name)
{
    jboolean ret = (jboolean)0;
    const char *name = NULL;
    int ok = 1;

    (void)cls;
    if (j_name) {
        name = (*env)->GetStringUTFChars(env, j_name, NULL);
        if (!name)
            ok = 0;
    }
    if (ok) {
        ret = (jboolean)jce_script_api_is_action_pressed(jce_java_api(api), name);
    }
    if (name)
        (*env)->ReleaseStringUTFChars(env, j_name, name);
    return ret;
}


/* jce.get_axis -> jce_script_api_get_axis (value_return) */
JNIEXPORT jfloat JNICALL Java_com_jce_script_JceScript_nGetAxis(
    JNIEnv *env, jclass cls, jlong api, jstring j_name)
{
    jfloat ret = (jfloat)0;
    const char *name = NULL;
    int ok = 1;

    (void)cls;
    if (j_name) {
        name = (*env)->GetStringUTFChars(env, j_name, NULL);
        if (!name)
            ok = 0;
    }
    if (ok) {
        ret = (jfloat)jce_script_api_get_axis(jce_java_api(api), name);
    }
    if (name)
        (*env)->ReleaseStringUTFChars(env, j_name, name);
    return ret;
}


/* jce.get_pointer_delta -> jce_script_api_get_pointer_delta (void_out_array) */
JNIEXPORT void JNICALL Java_com_jce_script_JceScript_nGetPointerDelta(
    JNIEnv *env, jclass cls, jlong api, jfloatArray j_outFloat)
{
    float out_xy[2];
    jfloat outFloat[2];

    (void)cls;
    memset(out_xy, 0, sizeof out_xy);
    jce_script_api_get_pointer_delta(jce_java_api(api), out_xy);
    outFloat[0] = (jfloat)out_xy[0];
    outFloat[1] = (jfloat)out_xy[1];
    if (j_outFloat)
        (*env)->SetFloatArrayRegion(env, j_outFloat, 0, 2, outFloat);
}


/* jce.get_pointer_wheel -> jce_script_api_get_pointer_wheel (value_return) */
JNIEXPORT jfloat JNICALL Java_com_jce_script_JceScript_nGetPointerWheel(
    JNIEnv *env, jclass cls, jlong api)
{
    jfloat ret = (jfloat)0;

    (void)env;
    (void)cls;
    ret = (jfloat)jce_script_api_get_pointer_wheel(jce_java_api(api));
    return ret;
}


/* jce.is_pointer_down -> jce_script_api_is_pointer_down (value_return) */
JNIEXPORT jboolean JNICALL Java_com_jce_script_JceScript_nIsPointerDown(
    JNIEnv *env, jclass cls, jlong api, jint button)
{
    jboolean ret = (jboolean)0;

    (void)env;
    (void)cls;
    ret = (jboolean)jce_script_api_is_pointer_down(jce_java_api(api), (int)button);
    return ret;
}


/* jce.get_touch_count -> jce_script_api_get_touch_count (value_return) */
JNIEXPORT jint JNICALL Java_com_jce_script_JceScript_nGetTouchCount(
    JNIEnv *env, jclass cls, jlong api)
{
    jint ret = (jint)0;

    (void)env;
    (void)cls;
    ret = (jint)jce_script_api_get_touch_count(jce_java_api(api));
    return ret;
}


/* jce.get_touch -> jce_script_api_get_touch (fallible_out) */
JNIEXPORT jboolean JNICALL Java_com_jce_script_JceScript_nGetTouch(
    JNIEnv *env, jclass cls, jlong api, jint index, jlongArray j_outLong,
    jfloatArray j_outFloat)
{
    jboolean ret = JNI_FALSE;
    uint64_t id;
    float x;
    float y;
    float pressure;
    jlong outLong[1];
    jfloat outFloat[3];

    (void)cls;
    memset(&id, 0, sizeof id);
    memset(&x, 0, sizeof x);
    memset(&y, 0, sizeof y);
    memset(&pressure, 0, sizeof pressure);
    if (jce_script_api_get_touch(jce_java_api(api), (int)index, &id, &x, &y, &pressure)) {
        ret = JNI_TRUE;
        outLong[0] = (jlong)id;
        if (j_outLong)
            (*env)->SetLongArrayRegion(env, j_outLong, 0, 1, outLong);
        outFloat[0] = (jfloat)x;
        outFloat[1] = (jfloat)y;
        outFloat[2] = (jfloat)pressure;
        if (j_outFloat)
            (*env)->SetFloatArrayRegion(env, j_outFloat, 0, 3, outFloat);
    }
    return ret;
}


/* jce.tr -> jce_script_api_tr (value_return) */
JNIEXPORT jstring JNICALL Java_com_jce_script_JceScript_nTr(
    JNIEnv *env, jclass cls, jlong api, jstring j_key)
{
    jstring ret = NULL;
    const char *v = NULL;
    const char *key = NULL;
    int ok = 1;

    (void)cls;
    if (j_key) {
        key = (*env)->GetStringUTFChars(env, j_key, NULL);
        if (!key)
            ok = 0;
    }
    if (ok) {
        v = jce_script_api_tr(jce_java_api(api), key);
        if (v)
            ret = (*env)->NewStringUTF(env, v);
    }
    if (key)
        (*env)->ReleaseStringUTFChars(env, j_key, key);
    return ret;
}


/* jce.get_locale -> jce_script_api_get_locale (value_return) */
JNIEXPORT jstring JNICALL Java_com_jce_script_JceScript_nGetLocale(
    JNIEnv *env, jclass cls, jlong api)
{
    jstring ret = NULL;
    const char *v = NULL;

    (void)cls;
    v = jce_script_api_get_locale(jce_java_api(api));
    if (v)
        ret = (*env)->NewStringUTF(env, v);
    return ret;
}


/* jce.set_locale -> jce_script_api_set_locale (void_call) */
JNIEXPORT void JNICALL Java_com_jce_script_JceScript_nSetLocale(
    JNIEnv *env, jclass cls, jlong api, jstring j_locale)
{
    const char *locale = NULL;
    int ok = 1;

    (void)cls;
    if (j_locale) {
        locale = (*env)->GetStringUTFChars(env, j_locale, NULL);
        if (!locale)
            ok = 0;
    }
    if (ok) {
        jce_script_api_set_locale(jce_java_api(api), locale);
    }
    if (locale)
        (*env)->ReleaseStringUTFChars(env, j_locale, locale);
}


/* jce.get_velocity -> jce_script_api_get_velocity (fallible_out) */
JNIEXPORT jboolean JNICALL Java_com_jce_script_JceScript_nGetVelocity(
    JNIEnv *env, jclass cls, jlong api, jlong e, jfloatArray j_outFloat)
{
    jboolean ret = JNI_FALSE;
    float out[3];
    jfloat outFloat[3];

    (void)cls;
    memset(out, 0, sizeof out);
    if (jce_script_api_get_velocity(jce_java_api(api), (JceScriptEntity)e, out)) {
        ret = JNI_TRUE;
        outFloat[0] = (jfloat)out[0];
        outFloat[1] = (jfloat)out[1];
        outFloat[2] = (jfloat)out[2];
        if (j_outFloat)
            (*env)->SetFloatArrayRegion(env, j_outFloat, 0, 3, outFloat);
    }
    return ret;
}


/* jce.vehicle_set_input -> jce_script_api_vehicle_set_input (void_call) */
JNIEXPORT void JNICALL Java_com_jce_script_JceScript_nVehicleSetInput(
    JNIEnv *env, jclass cls, jlong api, jlong e, jfloat throttle, jfloat brake,
    jfloat steer)
{

    (void)env;
    (void)cls;
    jce_script_api_vehicle_set_input(jce_java_api(api), (JceScriptEntity)e, (float)throttle, (float)brake, (float)steer);
}


/* jce.vehicle_get_speed -> jce_script_api_vehicle_get_speed (value_return) */
JNIEXPORT jfloat JNICALL Java_com_jce_script_JceScript_nVehicleGetSpeed(
    JNIEnv *env, jclass cls, jlong api, jlong e)
{
    jfloat ret = (jfloat)0;

    (void)env;
    (void)cls;
    ret = (jfloat)jce_script_api_vehicle_get_speed(jce_java_api(api), (JceScriptEntity)e);
    return ret;
}


/* jce.get_move -> jce_script_api_get_move (void_out_array) */
JNIEXPORT void JNICALL Java_com_jce_script_JceScript_nGetMove(
    JNIEnv *env, jclass cls, jlong api, jfloatArray j_outFloat)
{
    float out[3];
    jfloat outFloat[3];

    (void)cls;
    memset(out, 0, sizeof out);
    jce_script_api_get_move(jce_java_api(api), out);
    outFloat[0] = (jfloat)out[0];
    outFloat[1] = (jfloat)out[1];
    outFloat[2] = (jfloat)out[2];
    if (j_outFloat)
        (*env)->SetFloatArrayRegion(env, j_outFloat, 0, 3, outFloat);
}


/* jce.ui_get_slider -> jce_script_api_ui_get_slider (fallible_out) */
JNIEXPORT jboolean JNICALL Java_com_jce_script_JceScript_nUiGetSlider(
    JNIEnv *env, jclass cls, jlong api, jlong e, jfloatArray j_outFloat)
{
    jboolean ret = JNI_FALSE;
    float out;
    jfloat outFloat[1];

    (void)cls;
    memset(&out, 0, sizeof out);
    if (jce_script_api_ui_get_slider(jce_java_api(api), (JceScriptEntity)e, &out)) {
        ret = JNI_TRUE;
        outFloat[0] = (jfloat)out;
        if (j_outFloat)
            (*env)->SetFloatArrayRegion(env, j_outFloat, 0, 1, outFloat);
    }
    return ret;
}


/* jce.ui_set_slider -> jce_script_api_ui_set_slider (void_call) */
JNIEXPORT void JNICALL Java_com_jce_script_JceScript_nUiSetSlider(
    JNIEnv *env, jclass cls, jlong api, jlong e, jfloat v)
{

    (void)env;
    (void)cls;
    jce_script_api_ui_set_slider(jce_java_api(api), (JceScriptEntity)e, (float)v);
}


/* jce.ui_get_toggle -> jce_script_api_ui_get_toggle (fallible_out) */
JNIEXPORT jboolean JNICALL Java_com_jce_script_JceScript_nUiGetToggle(
    JNIEnv *env, jclass cls, jlong api, jlong e, jbooleanArray j_outBoolean)
{
    jboolean ret = JNI_FALSE;
    bool out;
    jboolean outBoolean[1];

    (void)cls;
    memset(&out, 0, sizeof out);
    if (jce_script_api_ui_get_toggle(jce_java_api(api), (JceScriptEntity)e, &out)) {
        ret = JNI_TRUE;
        outBoolean[0] = (jboolean)out;
        if (j_outBoolean)
            (*env)->SetBooleanArrayRegion(env, j_outBoolean, 0, 1, outBoolean);
    }
    return ret;
}


/* jce.ui_set_toggle -> jce_script_api_ui_set_toggle (void_call) */
JNIEXPORT void JNICALL Java_com_jce_script_JceScript_nUiSetToggle(
    JNIEnv *env, jclass cls, jlong api, jlong e, jboolean v)
{

    (void)env;
    (void)cls;
    jce_script_api_ui_set_toggle(jce_java_api(api), (JceScriptEntity)e, (bool)v);
}


/* jce.ui_set_text -> jce_script_api_ui_set_text (void_call) */
JNIEXPORT void JNICALL Java_com_jce_script_JceScript_nUiSetText(
    JNIEnv *env, jclass cls, jlong api, jlong e, jstring j_txt)
{
    const char *txt = NULL;
    int ok = 1;

    (void)cls;
    if (j_txt) {
        txt = (*env)->GetStringUTFChars(env, j_txt, NULL);
        if (!txt)
            ok = 0;
    }
    if (ok) {
        jce_script_api_ui_set_text(jce_java_api(api), (JceScriptEntity)e, txt);
    }
    if (txt)
        (*env)->ReleaseStringUTFChars(env, j_txt, txt);
}


/* jce.send_message -> jce_script_api_send_message (void_call) */
JNIEXPORT void JNICALL Java_com_jce_script_JceScript_nSendMessage(
    JNIEnv *env, jclass cls, jlong api, jlong target, jstring j_msg, jdouble number_arg,
    jstring j_str_arg)
{
    const char *msg = NULL;
    const char *str_arg = NULL;
    int ok = 1;

    (void)cls;
    if (j_msg) {
        msg = (*env)->GetStringUTFChars(env, j_msg, NULL);
        if (!msg)
            ok = 0;
    }
    if (ok && j_str_arg) {
        str_arg = (*env)->GetStringUTFChars(env, j_str_arg, NULL);
        if (!str_arg)
            ok = 0;
    }
    if (ok) {
        jce_script_api_send_message(jce_java_api(api), (JceScriptEntity)target, msg, (double)number_arg, str_arg);
    }
    if (str_arg)
        (*env)->ReleaseStringUTFChars(env, j_str_arg, str_arg);
    if (msg)
        (*env)->ReleaseStringUTFChars(env, j_msg, msg);
}


/* jce.broadcast -> jce_script_api_broadcast (void_call) */
JNIEXPORT void JNICALL Java_com_jce_script_JceScript_nBroadcast(
    JNIEnv *env, jclass cls, jlong api, jstring j_msg, jdouble number_arg,
    jstring j_str_arg)
{
    const char *msg = NULL;
    const char *str_arg = NULL;
    int ok = 1;

    (void)cls;
    if (j_msg) {
        msg = (*env)->GetStringUTFChars(env, j_msg, NULL);
        if (!msg)
            ok = 0;
    }
    if (ok && j_str_arg) {
        str_arg = (*env)->GetStringUTFChars(env, j_str_arg, NULL);
        if (!str_arg)
            ok = 0;
    }
    if (ok) {
        jce_script_api_broadcast(jce_java_api(api), msg, (double)number_arg, str_arg);
    }
    if (str_arg)
        (*env)->ReleaseStringUTFChars(env, j_str_arg, str_arg);
    if (msg)
        (*env)->ReleaseStringUTFChars(env, j_msg, msg);
}


/* jce.has_component -> jce_script_api_has_component (value_return) */
JNIEXPORT jboolean JNICALL Java_com_jce_script_JceScript_nHasComponent(
    JNIEnv *env, jclass cls, jlong api, jlong e, jstring j_comp_name)
{
    jboolean ret = (jboolean)0;
    const char *comp_name = NULL;
    int ok = 1;

    (void)cls;
    if (j_comp_name) {
        comp_name = (*env)->GetStringUTFChars(env, j_comp_name, NULL);
        if (!comp_name)
            ok = 0;
    }
    if (ok) {
        ret = (jboolean)jce_script_api_has_component(jce_java_api(api), (JceScriptEntity)e, comp_name);
    }
    if (comp_name)
        (*env)->ReleaseStringUTFChars(env, j_comp_name, comp_name);
    return ret;
}


/* jce.is_component_enabled -> jce_script_api_is_component_enabled (value_return) */
JNIEXPORT jboolean JNICALL Java_com_jce_script_JceScript_nIsComponentEnabled(
    JNIEnv *env, jclass cls, jlong api, jlong e, jstring j_comp_name)
{
    jboolean ret = (jboolean)0;
    const char *comp_name = NULL;
    int ok = 1;

    (void)cls;
    if (j_comp_name) {
        comp_name = (*env)->GetStringUTFChars(env, j_comp_name, NULL);
        if (!comp_name)
            ok = 0;
    }
    if (ok) {
        ret = (jboolean)jce_script_api_is_component_enabled(jce_java_api(api), (JceScriptEntity)e, comp_name);
    }
    if (comp_name)
        (*env)->ReleaseStringUTFChars(env, j_comp_name, comp_name);
    return ret;
}


/* jce.set_component_enabled -> jce_script_api_set_component_enabled (void_call) */
JNIEXPORT void JNICALL Java_com_jce_script_JceScript_nSetComponentEnabled(
    JNIEnv *env, jclass cls, jlong api, jlong e, jstring j_comp_name, jboolean on)
{
    const char *comp_name = NULL;
    int ok = 1;

    (void)cls;
    if (j_comp_name) {
        comp_name = (*env)->GetStringUTFChars(env, j_comp_name, NULL);
        if (!comp_name)
            ok = 0;
    }
    if (ok) {
        jce_script_api_set_component_enabled(jce_java_api(api), (JceScriptEntity)e, comp_name, (bool)on);
    }
    if (comp_name)
        (*env)->ReleaseStringUTFChars(env, j_comp_name, comp_name);
}


/* jce.net_is_server -> jce_script_api_net_is_server (value_return) */
JNIEXPORT jboolean JNICALL Java_com_jce_script_JceScript_nNetIsServer(
    JNIEnv *env, jclass cls, jlong api)
{
    jboolean ret = (jboolean)0;

    (void)env;
    (void)cls;
    ret = (jboolean)jce_script_api_net_is_server(jce_java_api(api));
    return ret;
}


/* jce.net_is_client -> jce_script_api_net_is_client (value_return) */
JNIEXPORT jboolean JNICALL Java_com_jce_script_JceScript_nNetIsClient(
    JNIEnv *env, jclass cls, jlong api)
{
    jboolean ret = (jboolean)0;

    (void)env;
    (void)cls;
    ret = (jboolean)jce_script_api_net_is_client(jce_java_api(api));
    return ret;
}


/* jce.net_spawn -> jce_script_api_net_spawn (value_return) */
JNIEXPORT jlong JNICALL Java_com_jce_script_JceScript_nNetSpawn(
    JNIEnv *env, jclass cls, jlong api, jstring j_prefab_path, jfloat x, jfloat y,
    jfloat z)
{
    jlong ret = (jlong)0;
    const char *prefab_path = NULL;
    int ok = 1;

    (void)cls;
    if (j_prefab_path) {
        prefab_path = (*env)->GetStringUTFChars(env, j_prefab_path, NULL);
        if (!prefab_path)
            ok = 0;
    }
    if (ok) {
        ret = (jlong)jce_script_api_net_spawn(jce_java_api(api), prefab_path, (float)x, (float)y, (float)z);
    }
    if (prefab_path)
        (*env)->ReleaseStringUTFChars(env, j_prefab_path, prefab_path);
    return ret;
}


/* jce.rpc_send -> jce_script_api_rpc_send (value_return) */
JNIEXPORT jboolean JNICALL Java_com_jce_script_JceScript_nRpcSend(
    JNIEnv *env, jclass cls, jlong api, jlong e, jstring j_event, jint target,
    jstring j_payload)
{
    jboolean ret = (jboolean)0;
    const char *event = NULL;
    const char *payload = NULL;
    int ok = 1;

    (void)cls;
    if (j_event) {
        event = (*env)->GetStringUTFChars(env, j_event, NULL);
        if (!event)
            ok = 0;
    }
    if (ok && j_payload) {
        payload = (*env)->GetStringUTFChars(env, j_payload, NULL);
        if (!payload)
            ok = 0;
    }
    if (ok) {
        ret = (jboolean)jce_script_api_rpc_send(jce_java_api(api), (JceScriptEntity)e, event, (int)target, payload);
    }
    if (payload)
        (*env)->ReleaseStringUTFChars(env, j_payload, payload);
    if (event)
        (*env)->ReleaseStringUTFChars(env, j_event, event);
    return ret;
}


/* jce.particle_burst -> jce_script_api_particle_burst (void_call) */
JNIEXPORT void JNICALL Java_com_jce_script_JceScript_nParticleBurst(
    JNIEnv *env, jclass cls, jlong api, jlong e, jint count)
{

    (void)env;
    (void)cls;
    jce_script_api_particle_burst(jce_java_api(api), (JceScriptEntity)e, (int)count);
}


/* jce.particle_set_emitting -> jce_script_api_particle_set_emitting (void_call) */
JNIEXPORT void JNICALL Java_com_jce_script_JceScript_nParticleSetEmitting(
    JNIEnv *env, jclass cls, jlong api, jlong e, jboolean on)
{

    (void)env;
    (void)cls;
    jce_script_api_particle_set_emitting(jce_java_api(api), (JceScriptEntity)e, (bool)on);
}


/* jce.particle_set_color -> jce_script_api_particle_set_color (void_call) */
JNIEXPORT void JNICALL Java_com_jce_script_JceScript_nParticleSetColor(
    JNIEnv *env, jclass cls, jlong api, jlong e, jfloat r, jfloat g, jfloat b)
{

    (void)env;
    (void)cls;
    jce_script_api_particle_set_color(jce_java_api(api), (JceScriptEntity)e, (float)r, (float)g, (float)b);
}


/* jce.find_by_name -> jce_script_api_find_by_name (first_and_count) */
JNIEXPORT jint JNICALL Java_com_jce_script_JceScript_nFindByName(
    JNIEnv *env, jclass cls, jlong api, jstring j_name, jlongArray j_out)
{
    jint ret = 0;
    const char *name = NULL;
    jlong found[2];
    int n;
    int ok = 1;

    (void)cls;
    if (j_name) {
        name = (*env)->GetStringUTFChars(env, j_name, NULL);
        if (!name)
            ok = 0;
    }
    memset(found, 0, sizeof found);
    if (ok) {
        n = jce_script_api_find_by_name(jce_java_api(api), name, (JceScriptEntity *)found, 2);
        if (n > 2)
            n = 2;
        if (n > 0 && j_out)
            (*env)->SetLongArrayRegion(env, j_out, 0, n, found);
        ret = (jint)(n > 0 ? n : 0);
    }
    if (name)
        (*env)->ReleaseStringUTFChars(env, j_name, name);
    return ret;
}


/* jce.find_by_prefix -> jce_script_api_find_by_prefix (entity_table) */
JNIEXPORT jint JNICALL Java_com_jce_script_JceScript_nFindByPrefix(
    JNIEnv *env, jclass cls, jlong api, jstring j_prefix, jlongArray j_out)
{
    jint ret = 0;
    const char *prefix = NULL;
    jlong found[1024];
    int n;
    int ok = 1;

    (void)cls;
    if (j_prefix) {
        prefix = (*env)->GetStringUTFChars(env, j_prefix, NULL);
        if (!prefix)
            ok = 0;
    }
    memset(found, 0, sizeof found);
    if (ok) {
        n = jce_script_api_find_by_prefix(jce_java_api(api), prefix, (JceScriptEntity *)found, 1024);
        if (n > 1024)
            n = 1024;
        if (n > 0 && j_out)
            (*env)->SetLongArrayRegion(env, j_out, 0, n, found);
        ret = (jint)(n > 0 ? n : 0);
    }
    if (prefix)
        (*env)->ReleaseStringUTFChars(env, j_prefix, prefix);
    return ret;
}


/* jce.comp_get -> jce_script_api_comp_get (owned_string_release) */
JNIEXPORT jstring JNICALL Java_com_jce_script_JceScript_nCompGet(
    JNIEnv *env, jclass cls, jlong api, jlong e, jstring j_type)
{
    jstring ret = NULL;
    char stackbuf[1024];
    char *heap = NULL;
    char *buf = stackbuf;
    int cap = (int)sizeof stackbuf;
    int n;
    const char *type = NULL;
    int ok = 1;

    (void)cls;
    if (j_type) {
        type = (*env)->GetStringUTFChars(env, j_type, NULL);
        if (!type)
            ok = 0;
    }
    if (ok) {
        stackbuf[0] = '\0';
        n = jce_script_api_comp_get(jce_java_api(api), (JceScriptEntity)e, type, buf, cap);
        if (n >= cap) {
            heap = (char *)malloc((size_t)n + 1u);
            if (heap) {
                buf = heap;
                cap = n + 1;
                n = jce_script_api_comp_get(jce_java_api(api), (JceScriptEntity)e, type, buf, cap);
            }
        }
        if (n >= 0)
            ret = (*env)->NewStringUTF(env, buf);
        free(heap);
    }
    if (type)
        (*env)->ReleaseStringUTFChars(env, j_type, type);
    return ret;
}


/* jce.comp_set -> jce_script_api_comp_set (value_return) */
JNIEXPORT jboolean JNICALL Java_com_jce_script_JceScript_nCompSet(
    JNIEnv *env, jclass cls, jlong api, jlong e, jstring j_type, jstring j_json)
{
    jboolean ret = (jboolean)0;
    const char *type = NULL;
    const char *json = NULL;
    int ok = 1;

    (void)cls;
    if (j_type) {
        type = (*env)->GetStringUTFChars(env, j_type, NULL);
        if (!type)
            ok = 0;
    }
    if (ok && j_json) {
        json = (*env)->GetStringUTFChars(env, j_json, NULL);
        if (!json)
            ok = 0;
    }
    if (ok) {
        ret = (jboolean)jce_script_api_comp_set(jce_java_api(api), (JceScriptEntity)e, type, json);
    }
    if (json)
        (*env)->ReleaseStringUTFChars(env, j_json, json);
    if (type)
        (*env)->ReleaseStringUTFChars(env, j_type, type);
    return ret;
}


/* jce.render_get -> jce_script_api_render_get (owned_string_release) */
JNIEXPORT jstring JNICALL Java_com_jce_script_JceScript_nRenderGet(
    JNIEnv *env, jclass cls, jlong api)
{
    jstring ret = NULL;
    char stackbuf[1024];
    char *heap = NULL;
    char *buf = stackbuf;
    int cap = (int)sizeof stackbuf;
    int n;

    (void)cls;
    stackbuf[0] = '\0';
    n = jce_script_api_render_get(jce_java_api(api), buf, cap);
    if (n >= cap) {
        heap = (char *)malloc((size_t)n + 1u);
        if (heap) {
            buf = heap;
            cap = n + 1;
            n = jce_script_api_render_get(jce_java_api(api), buf, cap);
        }
    }
    if (n >= 0)
        ret = (*env)->NewStringUTF(env, buf);
    free(heap);
    return ret;
}


/* jce.render_set -> jce_script_api_render_set (value_return) */
JNIEXPORT jboolean JNICALL Java_com_jce_script_JceScript_nRenderSet(
    JNIEnv *env, jclass cls, jlong api, jstring j_json)
{
    jboolean ret = (jboolean)0;
    const char *json = NULL;
    int ok = 1;

    (void)cls;
    if (j_json) {
        json = (*env)->GetStringUTFChars(env, j_json, NULL);
        if (!json)
            ok = 0;
    }
    if (ok) {
        ret = (jboolean)jce_script_api_render_set(jce_java_api(api), json);
    }
    if (json)
        (*env)->ReleaseStringUTFChars(env, j_json, json);
    return ret;
}


/* jce.audio_set_volume -> jce_script_api_audio_set_volume (void_call) */
JNIEXPORT void JNICALL Java_com_jce_script_JceScript_nAudioSetVolume(
    JNIEnv *env, jclass cls, jlong api, jlong e, jfloat volume)
{

    (void)env;
    (void)cls;
    jce_script_api_audio_set_volume(jce_java_api(api), (JceScriptEntity)e, (float)volume);
}
