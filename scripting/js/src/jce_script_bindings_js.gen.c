/* jce_script_bindings_js.gen.c - GENERATED. DO NOT EDIT.
 *
 *   python tools/scriptgen/gen_script_bindings.py --write
 *
 * Source of truth: struct JceScriptHost in
 * engine/include/jce/middleware/script/jce_script.h (C types, arity,
 * parameter names) joined with the exposure decisions in
 * engine/src/middleware/script/script_exposure.json (name, shape, modifiers).
 *
 * This file is owned WHOLE by the generator.  There are no sentinel-delimited
 * regions: a tool that rewrites part of a file containing hand-written code
 * eventually eats the hand-written code.
 *
 * EVERY function guards the host member before calling it, and that is not
 * optional.  jce_script_vm_create copies min(host_size, sizeof s->host) over
 * a zeroed table, so a member an older caller's header did not have stays
 * NULL and calling it unguarded jumps through whatever followed the caller's
 * shorter object.
 *
 * The guard has TWO spellings and grepping for only the first will convince
 * you this file is broken when it is not:
 *
 *     s->have_host && s->host.<member>          100 functions
 *     !s->have_host || !s->host.<member>        1 function (jce.get_touch),
 *                                               where an index_base binding
 *                                               folds the guard into its
 *                                               early return
 *
 * `s` is the CONTEXT OPAQUE, set once by js_create.  A binding reached with a
 * NULL opaque returns undefined rather than dereferencing it: that state is
 * unreachable through jce_script_vm_create, and a crash would be a worse
 * answer than a no-op if some future path ever made it reachable.
 */

#include "jce_script_bindings_js.gen.h"

#include <string.h>

/* jce.get_position - shape: fallible_out
 * LOCAL position of `entity` -- its own translation, not composed up the parent chain. Absent when the entity has no transform. Use get_world_position for the composed pose. This doc said 'World-space' until 2026-08-26; the implementation always returned the local TRS (jce_scene_get_transform), and the wrong word was generated into all five language SDKs. */
static JSValue js_jce_get_position(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    float out_xyz[3];
    if (s->have_host && s->host.get_position &&
        s->host.get_position(s->host.user, e, out_xyz)) {
        ret = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, ret, 0, JS_NewFloat64(ctx, (double)out_xyz[0]));
        JS_SetPropertyUint32(ctx, ret, 1, JS_NewFloat64(ctx, (double)out_xyz[1]));
        JS_SetPropertyUint32(ctx, ret, 2, JS_NewFloat64(ctx, (double)out_xyz[2]));
    } else {
        ret = JS_NULL;   /* miss */
    }
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.set_position - shape: void_call */
static JSValue js_jce_set_position(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    double x_d = 0.0;
    if (JS_ToFloat64(ctx, &x_d, argv[1]) < 0) goto fail;
    float x = (float)x_d;
    double y_d = 0.0;
    if (JS_ToFloat64(ctx, &y_d, argv[2]) < 0) goto fail;
    float y = (float)y_d;
    double z_d = 0.0;
    if (JS_ToFloat64(ctx, &z_d, argv[3]) < 0) goto fail;
    float z = (float)z_d;
    if (s->have_host && s->host.set_position)
        s->host.set_position(s->host.user, e, x, y, z);
    ret = JS_UNDEFINED;
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.get_rotation - shape: fallible_out */
static JSValue js_jce_get_rotation(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    float out_euler_deg[3];
    if (s->have_host && s->host.get_rotation &&
        s->host.get_rotation(s->host.user, e, out_euler_deg)) {
        ret = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, ret, 0, JS_NewFloat64(ctx, (double)out_euler_deg[0]));
        JS_SetPropertyUint32(ctx, ret, 1, JS_NewFloat64(ctx, (double)out_euler_deg[1]));
        JS_SetPropertyUint32(ctx, ret, 2, JS_NewFloat64(ctx, (double)out_euler_deg[2]));
    } else {
        ret = JS_NULL;   /* miss */
    }
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.set_rotation - shape: void_call */
static JSValue js_jce_set_rotation(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    double x_d = 0.0;
    if (JS_ToFloat64(ctx, &x_d, argv[1]) < 0) goto fail;
    float x = (float)x_d;
    double y_d = 0.0;
    if (JS_ToFloat64(ctx, &y_d, argv[2]) < 0) goto fail;
    float y = (float)y_d;
    double z_d = 0.0;
    if (JS_ToFloat64(ctx, &z_d, argv[3]) < 0) goto fail;
    float z = (float)z_d;
    if (s->have_host && s->host.set_rotation)
        s->host.set_rotation(s->host.user, e, x, y, z);
    ret = JS_UNDEFINED;
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.get_scale - shape: fallible_out */
static JSValue js_jce_get_scale(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    float out_xyz[3];
    if (s->have_host && s->host.get_scale &&
        s->host.get_scale(s->host.user, e, out_xyz)) {
        ret = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, ret, 0, JS_NewFloat64(ctx, (double)out_xyz[0]));
        JS_SetPropertyUint32(ctx, ret, 1, JS_NewFloat64(ctx, (double)out_xyz[1]));
        JS_SetPropertyUint32(ctx, ret, 2, JS_NewFloat64(ctx, (double)out_xyz[2]));
    } else {
        ret = JS_NULL;   /* miss */
    }
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.get_world_position - shape: fallible_out
 * WORLD position of `entity`: its local TRS composed up the parent chain (jce_scene_get_world_matrix). Absent when the entity has no transform. Every parented rig -- arms, jaws, fingers, pads -- needs this rather than get_position. */
static JSValue js_jce_get_world_position(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    float out_xyz[3];
    if (s->have_host && s->host.get_world_position &&
        s->host.get_world_position(s->host.user, e, out_xyz)) {
        ret = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, ret, 0, JS_NewFloat64(ctx, (double)out_xyz[0]));
        JS_SetPropertyUint32(ctx, ret, 1, JS_NewFloat64(ctx, (double)out_xyz[1]));
        JS_SetPropertyUint32(ctx, ret, 2, JS_NewFloat64(ctx, (double)out_xyz[2]));
    } else {
        ret = JS_NULL;   /* miss */
    }
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.set_scale - shape: void_call */
static JSValue js_jce_set_scale(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    double x_d = 0.0;
    if (JS_ToFloat64(ctx, &x_d, argv[1]) < 0) goto fail;
    float x = (float)x_d;
    double y_d = 0.0;
    if (JS_ToFloat64(ctx, &y_d, argv[2]) < 0) goto fail;
    float y = (float)y_d;
    double z_d = 0.0;
    if (JS_ToFloat64(ctx, &z_d, argv[3]) < 0) goto fail;
    float z = (float)z_d;
    if (s->have_host && s->host.set_scale)
        s->host.set_scale(s->host.user, e, x, y, z);
    ret = JS_UNDEFINED;
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.set_parent - shape: value_return
 * The ONLY boolean argument on this surface that is type-checked; the other five accept any truthy value. The luaL_checktype is emitted from this entry's `strict` modifier -- no line citation, because the hand-written body that carried it is gone. test_strict_emits_a_type_check_only_for_the_named_parameter is what fails if the emitter drops it. */
static JSValue js_jce_set_parent(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double child_d = 0.0;
    if (JS_ToFloat64(ctx, &child_d, argv[0]) < 0) goto fail;
    JceScriptEntity child = (JceScriptEntity)child_d;
    double parent_d = 0.0;
    if (JS_ToFloat64(ctx, &parent_d, argv[1]) < 0) goto fail;
    JceScriptEntity parent = (JceScriptEntity)parent_d;
    bool preserve_world = JS_ToBool(ctx, argv[2]) != 0;
    bool v = (s->have_host && s->host.set_parent)
                ? s->host.set_parent(s->host.user, child, parent, preserve_world) : false;
    ret = JS_NewBool(ctx, v);
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.get_parent - shape: value_return */
static JSValue js_jce_get_parent(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double child_d = 0.0;
    if (JS_ToFloat64(ctx, &child_d, argv[0]) < 0) goto fail;
    JceScriptEntity child = (JceScriptEntity)child_d;
    JceScriptEntity v = (s->have_host && s->host.get_parent)
                ? s->host.get_parent(s->host.user, child) : 0;
    ret = JS_NewUint32(ctx, (uint32_t)v);
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.is_key_down - shape: value_return */
static JSValue js_jce_is_key_down(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double keycode_d = 0.0;
    if (JS_ToFloat64(ctx, &keycode_d, argv[0]) < 0) goto fail;
    int keycode = (int)keycode_d;
    bool v = (s->have_host && s->host.is_key_down)
                ? s->host.is_key_down(s->host.user, keycode) : false;
    ret = JS_NewBool(ctx, v);
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.find_with_tag - shape: value_return */
static JSValue js_jce_find_with_tag(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    const char *tag = NULL, *tag_cs = NULL;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    tag_cs = JS_ToCString(ctx, argv[0]);
    if (!tag_cs) goto fail;
    tag = tag_cs;
    JceScriptEntity v = (s->have_host && s->host.find_with_tag)
                ? s->host.find_with_tag(s->host.user, tag) : 0;
    ret = JS_NewUint32(ctx, (uint32_t)v);
    if (tag_cs) JS_FreeCString(ctx, tag_cs);
    return ret;
fail:
    if (tag_cs) JS_FreeCString(ctx, tag_cs);
    return JS_EXCEPTION;
}

/* jce.destroy - shape: void_call */
static JSValue js_jce_destroy(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    if (s->have_host && s->host.destroy_entity)
        s->host.destroy_entity(s->host.user, e);
    ret = JS_UNDEFINED;
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.spawn - shape: value_return */
static JSValue js_jce_spawn(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    const char *prefab_path = NULL, *prefab_path_cs = NULL;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    prefab_path_cs = JS_ToCString(ctx, argv[0]);
    if (!prefab_path_cs) goto fail;
    prefab_path = prefab_path_cs;
    double x_d = 0.0;
    if (argc > 1 && !JS_IsUndefined(argv[1]) &&
        JS_ToFloat64(ctx, &x_d, argv[1]) < 0) goto fail;
    float x = (float)x_d;
    double y_d = 0.0;
    if (argc > 2 && !JS_IsUndefined(argv[2]) &&
        JS_ToFloat64(ctx, &y_d, argv[2]) < 0) goto fail;
    float y = (float)y_d;
    double z_d = 0.0;
    if (argc > 3 && !JS_IsUndefined(argv[3]) &&
        JS_ToFloat64(ctx, &z_d, argv[3]) < 0) goto fail;
    float z = (float)z_d;
    JceScriptEntity v = (s->have_host && s->host.spawn)
                ? s->host.spawn(s->host.user, prefab_path, x, y, z) : 0;
    ret = JS_NewUint32(ctx, (uint32_t)v);
    if (prefab_path_cs) JS_FreeCString(ctx, prefab_path_cs);
    return ret;
fail:
    if (prefab_path_cs) JS_FreeCString(ctx, prefab_path_cs);
    return JS_EXCEPTION;
}

/* jce.move_axis - shape: void_out_array */
static JSValue js_jce_move_axis(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    float out_xz[2] = { 0.0f, 0.0f };
    if (s->have_host && s->host.move_axis)
        s->host.move_axis(s->host.user, out_xz);
    ret = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, ret, 0, JS_NewFloat64(ctx, (double)out_xz[0]));
    JS_SetPropertyUint32(ctx, ret, 1, JS_NewFloat64(ctx, (double)out_xz[1]));
    return ret;
}

/* jce.jump_pressed - shape: value_return */
static JSValue js_jce_jump_pressed(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    bool v = (s->have_host && s->host.input_button)
                ? s->host.input_button(s->host.user, (int)0) : false;
    ret = JS_NewBool(ctx, v);
    return ret;
}

/* jce.sprint - shape: value_return */
static JSValue js_jce_sprint(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    bool v = (s->have_host && s->host.input_button)
                ? s->host.input_button(s->host.user, (int)1) : false;
    ret = JS_NewBool(ctx, v);
    return ret;
}

/* jce.attack_pressed - shape: value_return */
static JSValue js_jce_attack_pressed(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    bool v = (s->have_host && s->host.input_button)
                ? s->host.input_button(s->host.user, (int)2) : false;
    ret = JS_NewBool(ctx, v);
    return ret;
}

/* jce.set_time_scale - shape: void_call */
static JSValue js_jce_set_time_scale(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double scale_d = 0.0;
    if (JS_ToFloat64(ctx, &scale_d, argv[0]) < 0) goto fail;
    float scale = (float)scale_d;
    if (s->have_host && s->host.set_time_scale)
        s->host.set_time_scale(s->host.user, scale);
    ret = JS_UNDEFINED;
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.pause - shape: void_call
 * jce.pause() with no argument pauses; jce.pause(false) resumes. */
static JSValue js_jce_pause(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    bool paused = (argc > 0 && !JS_IsUndefined(argv[0]))
        ? (JS_ToBool(ctx, argv[0]) != 0) : true;
    if (s->have_host && s->host.set_paused)
        s->host.set_paused(s->host.user, paused);
    ret = JS_UNDEFINED;
    return ret;
}

/* jce.shake_camera - shape: void_call */
static JSValue js_jce_shake_camera(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double amount_d = 0.5;
    if (argc > 0 && !JS_IsUndefined(argv[0]) &&
        JS_ToFloat64(ctx, &amount_d, argv[0]) < 0) goto fail;
    float amount = (float)amount_d;
    if (s->have_host && s->host.shake_camera)
        s->host.shake_camera(s->host.user, amount);
    ret = JS_UNDEFINED;
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.music_set_intensity - shape: void_call */
static JSValue js_jce_music_set_intensity(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double intensity_d = 0.0;
    if (JS_ToFloat64(ctx, &intensity_d, argv[0]) < 0) goto fail;
    float intensity = (float)intensity_d;
    if (s->have_host && s->host.music_set_intensity)
        s->host.music_set_intensity(s->host.user, intensity);
    ret = JS_UNDEFINED;
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.music_get_intensity - shape: value_return */
static JSValue js_jce_music_get_intensity(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    float v = (s->have_host && s->host.music_get_intensity)
                ? s->host.music_get_intensity(s->host.user) : 0.0f;
    ret = JS_NewFloat64(ctx, (double)v);
    return ret;
}

/* jce.music_request_transition - shape: value_return
 * Absolute playhead time of the quantized switch; negative on miss or no track, which is why the no-host value is -1 and not 0. */
static JSValue js_jce_music_request_transition(JSContext *ctx, JSValueConst this_val,
                                               int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double to_segment_d = 0.0;
    if (JS_ToFloat64(ctx, &to_segment_d, argv[0]) < 0) goto fail;
    int to_segment = (int)to_segment_d;
    float v = (s->have_host && s->host.music_request_transition)
                ? s->host.music_request_transition(s->host.user, to_segment) : -1.0f;
    ret = JS_NewFloat64(ctx, (double)v);
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.gas_activate - shape: value_return */
static JSValue js_jce_gas_activate(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    double ability_id_d = 0.0;
    if (JS_ToFloat64(ctx, &ability_id_d, argv[1]) < 0) goto fail;
    uint32_t ability_id = (uint32_t)ability_id_d;
    bool v = (s->have_host && s->host.gas_activate)
                ? s->host.gas_activate(s->host.user, e, ability_id) : false;
    ret = JS_NewBool(ctx, v);
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.gas_get - shape: fallible_out */
static JSValue js_jce_gas_get(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    const char *attr_name = NULL, *attr_name_cs = NULL;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    attr_name_cs = JS_ToCString(ctx, argv[1]);
    if (!attr_name_cs) goto fail;
    attr_name = attr_name_cs;
    float out_value = 0.0f;
    if (s->have_host && s->host.gas_get &&
        s->host.gas_get(s->host.user, e, attr_name, &out_value)) {
        ret = JS_NewFloat64(ctx, (double)out_value);
    } else {
        ret = JS_NULL;   /* miss */
    }
    if (attr_name_cs) JS_FreeCString(ctx, attr_name_cs);
    return ret;
fail:
    if (attr_name_cs) JS_FreeCString(ctx, attr_name_cs);
    return JS_EXCEPTION;
}

/* jce.gas_apply - shape: value_return */
static JSValue js_jce_gas_apply(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    const char *attr_name = NULL, *attr_name_cs = NULL;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    attr_name_cs = JS_ToCString(ctx, argv[1]);
    if (!attr_name_cs) goto fail;
    attr_name = attr_name_cs;
    double op_d = 0;
    if (argc > 2 && !JS_IsUndefined(argv[2]) &&
        JS_ToFloat64(ctx, &op_d, argv[2]) < 0) goto fail;
    int op = (int)op_d;
    double magnitude_d = 0.0;
    if (JS_ToFloat64(ctx, &magnitude_d, argv[3]) < 0) goto fail;
    float magnitude = (float)magnitude_d;
    double duration_seconds_d = 0.0;
    if (argc > 4 && !JS_IsUndefined(argv[4]) &&
        JS_ToFloat64(ctx, &duration_seconds_d, argv[4]) < 0) goto fail;
    float duration_seconds = (float)duration_seconds_d;
    bool v = (s->have_host && s->host.gas_apply)
                ? s->host.gas_apply(s->host.user, e, attr_name, op, magnitude, duration_seconds) : false;
    ret = JS_NewBool(ctx, v);
    if (attr_name_cs) JS_FreeCString(ctx, attr_name_cs);
    return ret;
fail:
    if (attr_name_cs) JS_FreeCString(ctx, attr_name_cs);
    return JS_EXCEPTION;
}

/* jce.raycast - shape: fallible_out
 * 8 values on a hit; a MISS pushes integer 0, not nil -- scripts branch on `e == 0`. */
static JSValue js_jce_raycast(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    float origin[3] = { 0 };
    {
        int ai;
        for (ai = 0; ai < 3; ++ai) {
            double ad = 0.0;
            JSValue ae = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)ai);
            int abad = JS_ToFloat64(ctx, &ad, ae) < 0;
            JS_FreeValue(ctx, ae);
            if (abad) goto fail;
            origin[ai] = (float)ad;
        }
    }
    float dir[3] = { 0 };
    {
        int ai;
        for (ai = 0; ai < 3; ++ai) {
            double ad = 0.0;
            JSValue ae = JS_GetPropertyUint32(ctx, argv[1], (uint32_t)ai);
            int abad = JS_ToFloat64(ctx, &ad, ae) < 0;
            JS_FreeValue(ctx, ae);
            if (abad) goto fail;
            dir[ai] = (float)ad;
        }
    }
    double max_dist_d = 0.0;
    if (JS_ToFloat64(ctx, &max_dist_d, argv[2]) < 0) goto fail;
    float max_dist = (float)max_dist_d;
    JceScriptRaycastHit out;
    memset(&out, 0, sizeof out);
    if (s->have_host && s->host.raycast &&
        s->host.raycast(s->host.user, origin, dir, max_dist, &out)) {
        ret = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, ret, 0, JS_NewUint32(ctx, (uint32_t)out.entity));
        JS_SetPropertyUint32(ctx, ret, 1, JS_NewFloat64(ctx, (double)out.point[0]));
        JS_SetPropertyUint32(ctx, ret, 2, JS_NewFloat64(ctx, (double)out.point[1]));
        JS_SetPropertyUint32(ctx, ret, 3, JS_NewFloat64(ctx, (double)out.point[2]));
        JS_SetPropertyUint32(ctx, ret, 4, JS_NewFloat64(ctx, (double)out.normal[0]));
        JS_SetPropertyUint32(ctx, ret, 5, JS_NewFloat64(ctx, (double)out.normal[1]));
        JS_SetPropertyUint32(ctx, ret, 6, JS_NewFloat64(ctx, (double)out.normal[2]));
        JS_SetPropertyUint32(ctx, ret, 7, JS_NewFloat64(ctx, (double)out.distance));
    } else {
        ret = JS_NewInt32(ctx, (int32_t)0);   /* miss */
    }
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.raycast_filtered - shape: fallible_out
 * Closest hit along the ray, honouring a layer mask and the trigger skip -- 8 values on a hit; a MISS pushes integer 0, not nil, so scripts branch on `e == 0`, the same as jce.raycast. layer_mask 0 means every layer and hit_triggers defaults to false, so the common call stays origin/dir/distance and the filter is what you add when you need it. hit_triggers is separate from the mask because a trigger volume is not a layer: collapsing them would make 'ignore triggers on layer 3' inexpressible. */
static JSValue js_jce_raycast_filtered(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    float origin[3] = { 0 };
    {
        int ai;
        for (ai = 0; ai < 3; ++ai) {
            double ad = 0.0;
            JSValue ae = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)ai);
            int abad = JS_ToFloat64(ctx, &ad, ae) < 0;
            JS_FreeValue(ctx, ae);
            if (abad) goto fail;
            origin[ai] = (float)ad;
        }
    }
    float dir[3] = { 0 };
    {
        int ai;
        for (ai = 0; ai < 3; ++ai) {
            double ad = 0.0;
            JSValue ae = JS_GetPropertyUint32(ctx, argv[1], (uint32_t)ai);
            int abad = JS_ToFloat64(ctx, &ad, ae) < 0;
            JS_FreeValue(ctx, ae);
            if (abad) goto fail;
            dir[ai] = (float)ad;
        }
    }
    double max_dist_d = 0.0;
    if (JS_ToFloat64(ctx, &max_dist_d, argv[2]) < 0) goto fail;
    float max_dist = (float)max_dist_d;
    double layer_mask_d = 0;
    if (argc > 3 && !JS_IsUndefined(argv[3]) &&
        JS_ToFloat64(ctx, &layer_mask_d, argv[3]) < 0) goto fail;
    uint32_t layer_mask = (uint32_t)layer_mask_d;
    bool hit_triggers = (argc > 4 && !JS_IsUndefined(argv[4]))
        ? (JS_ToBool(ctx, argv[4]) != 0) : false;
    JceScriptRaycastHit out;
    memset(&out, 0, sizeof out);
    if (s->have_host && s->host.raycast_filtered &&
        s->host.raycast_filtered(s->host.user, origin, dir, max_dist, layer_mask, hit_triggers, &out)) {
        ret = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, ret, 0, JS_NewUint32(ctx, (uint32_t)out.entity));
        JS_SetPropertyUint32(ctx, ret, 1, JS_NewFloat64(ctx, (double)out.point[0]));
        JS_SetPropertyUint32(ctx, ret, 2, JS_NewFloat64(ctx, (double)out.point[1]));
        JS_SetPropertyUint32(ctx, ret, 3, JS_NewFloat64(ctx, (double)out.point[2]));
        JS_SetPropertyUint32(ctx, ret, 4, JS_NewFloat64(ctx, (double)out.normal[0]));
        JS_SetPropertyUint32(ctx, ret, 5, JS_NewFloat64(ctx, (double)out.normal[1]));
        JS_SetPropertyUint32(ctx, ret, 6, JS_NewFloat64(ctx, (double)out.normal[2]));
        JS_SetPropertyUint32(ctx, ret, 7, JS_NewFloat64(ctx, (double)out.distance));
    } else {
        ret = JS_NewInt32(ctx, (int32_t)0);   /* miss */
    }
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.raycast_all - shape: entity_table
 * Every entity the ray passes through, as one array sorted near to far. layer_mask 0 means every layer; hit_triggers defaults to false. Returns ENTITIES rather than full hit records because the eight-value hit does not survive as an array shape across seven languages without inventing a per-language container -- re-query a specific one with jce.raycast_filtered when you need its point and normal. */
static JSValue js_jce_raycast_all(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    float origin[3] = { 0 };
    {
        int ai;
        for (ai = 0; ai < 3; ++ai) {
            double ad = 0.0;
            JSValue ae = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)ai);
            int abad = JS_ToFloat64(ctx, &ad, ae) < 0;
            JS_FreeValue(ctx, ae);
            if (abad) goto fail;
            origin[ai] = (float)ad;
        }
    }
    float dir[3] = { 0 };
    {
        int ai;
        for (ai = 0; ai < 3; ++ai) {
            double ad = 0.0;
            JSValue ae = JS_GetPropertyUint32(ctx, argv[1], (uint32_t)ai);
            int abad = JS_ToFloat64(ctx, &ad, ae) < 0;
            JS_FreeValue(ctx, ae);
            if (abad) goto fail;
            dir[ai] = (float)ad;
        }
    }
    double max_dist_d = 0.0;
    if (JS_ToFloat64(ctx, &max_dist_d, argv[2]) < 0) goto fail;
    float max_dist = (float)max_dist_d;
    double layer_mask_d = 0;
    if (argc > 3 && !JS_IsUndefined(argv[3]) &&
        JS_ToFloat64(ctx, &layer_mask_d, argv[3]) < 0) goto fail;
    uint32_t layer_mask = (uint32_t)layer_mask_d;
    bool hit_triggers = (argc > 4 && !JS_IsUndefined(argv[4]))
        ? (JS_ToBool(ctx, argv[4]) != 0) : false;
    JceScriptEntity found[256];
    int n = 0;
    if (s->have_host && s->host.raycast_all)
        n = s->host.raycast_all(s->host.user, origin, dir, max_dist, layer_mask, hit_triggers, found, 256);
    {
        int fi;
        ret = JS_NewArray(ctx);
        for (fi = 0; fi < n; ++fi)
            JS_SetPropertyUint32(ctx, ret, (uint32_t)fi,
                                 JS_NewUint32(ctx, (uint32_t)found[fi]));
    }
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.apply_impulse - shape: void_call */
static JSValue js_jce_apply_impulse(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    double x_d = 0.0;
    if (JS_ToFloat64(ctx, &x_d, argv[1]) < 0) goto fail;
    float x = (float)x_d;
    double y_d = 0.0;
    if (JS_ToFloat64(ctx, &y_d, argv[2]) < 0) goto fail;
    float y = (float)y_d;
    double z_d = 0.0;
    if (JS_ToFloat64(ctx, &z_d, argv[3]) < 0) goto fail;
    float z = (float)z_d;
    if (s->have_host && s->host.apply_impulse)
        s->host.apply_impulse(s->host.user, e, x, y, z);
    ret = JS_UNDEFINED;
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.set_velocity - shape: void_call */
static JSValue js_jce_set_velocity(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    double x_d = 0.0;
    if (JS_ToFloat64(ctx, &x_d, argv[1]) < 0) goto fail;
    float x = (float)x_d;
    double y_d = 0.0;
    if (JS_ToFloat64(ctx, &y_d, argv[2]) < 0) goto fail;
    float y = (float)y_d;
    double z_d = 0.0;
    if (JS_ToFloat64(ctx, &z_d, argv[3]) < 0) goto fail;
    float z = (float)z_d;
    if (s->have_host && s->host.set_velocity)
        s->host.set_velocity(s->host.user, e, x, y, z);
    ret = JS_UNDEFINED;
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.anim_set_float - shape: void_call */
static JSValue js_jce_anim_set_float(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    const char *name = NULL, *name_cs = NULL;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    name_cs = JS_ToCString(ctx, argv[1]);
    if (!name_cs) goto fail;
    name = name_cs;
    double v_d = 0.0;
    if (JS_ToFloat64(ctx, &v_d, argv[2]) < 0) goto fail;
    float v = (float)v_d;
    if (s->have_host && s->host.anim_set_float)
        s->host.anim_set_float(s->host.user, e, name, v);
    ret = JS_UNDEFINED;
    if (name_cs) JS_FreeCString(ctx, name_cs);
    return ret;
fail:
    if (name_cs) JS_FreeCString(ctx, name_cs);
    return JS_EXCEPTION;
}

/* jce.anim_set_int - shape: void_call */
static JSValue js_jce_anim_set_int(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    const char *name = NULL, *name_cs = NULL;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    name_cs = JS_ToCString(ctx, argv[1]);
    if (!name_cs) goto fail;
    name = name_cs;
    double v_d = 0.0;
    if (JS_ToFloat64(ctx, &v_d, argv[2]) < 0) goto fail;
    int v = (int)v_d;
    if (s->have_host && s->host.anim_set_int)
        s->host.anim_set_int(s->host.user, e, name, v);
    ret = JS_UNDEFINED;
    if (name_cs) JS_FreeCString(ctx, name_cs);
    return ret;
fail:
    if (name_cs) JS_FreeCString(ctx, name_cs);
    return JS_EXCEPTION;
}

/* jce.anim_set_bool - shape: void_call */
static JSValue js_jce_anim_set_bool(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    const char *name = NULL, *name_cs = NULL;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    name_cs = JS_ToCString(ctx, argv[1]);
    if (!name_cs) goto fail;
    name = name_cs;
    bool v = JS_ToBool(ctx, argv[2]) != 0;
    if (s->have_host && s->host.anim_set_bool)
        s->host.anim_set_bool(s->host.user, e, name, v);
    ret = JS_UNDEFINED;
    if (name_cs) JS_FreeCString(ctx, name_cs);
    return ret;
fail:
    if (name_cs) JS_FreeCString(ctx, name_cs);
    return JS_EXCEPTION;
}

/* jce.anim_set_trigger - shape: void_call */
static JSValue js_jce_anim_set_trigger(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    const char *name = NULL, *name_cs = NULL;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    name_cs = JS_ToCString(ctx, argv[1]);
    if (!name_cs) goto fail;
    name = name_cs;
    if (s->have_host && s->host.anim_set_trigger)
        s->host.anim_set_trigger(s->host.user, e, name);
    ret = JS_UNDEFINED;
    if (name_cs) JS_FreeCString(ctx, name_cs);
    return ret;
fail:
    if (name_cs) JS_FreeCString(ctx, name_cs);
    return JS_EXCEPTION;
}

/* jce.is_action_down - shape: value_return */
static JSValue js_jce_is_action_down(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    const char *name = NULL, *name_cs = NULL;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    name_cs = JS_ToCString(ctx, argv[0]);
    if (!name_cs) goto fail;
    name = name_cs;
    bool v = (s->have_host && s->host.action_down)
                ? s->host.action_down(s->host.user, name) : false;
    ret = JS_NewBool(ctx, v);
    if (name_cs) JS_FreeCString(ctx, name_cs);
    return ret;
fail:
    if (name_cs) JS_FreeCString(ctx, name_cs);
    return JS_EXCEPTION;
}

/* jce.is_action_pressed - shape: value_return */
static JSValue js_jce_is_action_pressed(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    const char *name = NULL, *name_cs = NULL;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    name_cs = JS_ToCString(ctx, argv[0]);
    if (!name_cs) goto fail;
    name = name_cs;
    bool v = (s->have_host && s->host.action_pressed)
                ? s->host.action_pressed(s->host.user, name) : false;
    ret = JS_NewBool(ctx, v);
    if (name_cs) JS_FreeCString(ctx, name_cs);
    return ret;
fail:
    if (name_cs) JS_FreeCString(ctx, name_cs);
    return JS_EXCEPTION;
}

/* jce.get_axis - shape: value_return */
static JSValue js_jce_get_axis(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    const char *name = NULL, *name_cs = NULL;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    name_cs = JS_ToCString(ctx, argv[0]);
    if (!name_cs) goto fail;
    name = name_cs;
    float v = (s->have_host && s->host.action_axis)
                ? s->host.action_axis(s->host.user, name) : 0.0f;
    ret = JS_NewFloat64(ctx, (double)v);
    if (name_cs) JS_FreeCString(ctx, name_cs);
    return ret;
fail:
    if (name_cs) JS_FreeCString(ctx, name_cs);
    return JS_EXCEPTION;
}

/* jce.get_pointer_delta - shape: void_out_array */
static JSValue js_jce_get_pointer_delta(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    float out_xy[2] = { 0.0f, 0.0f };
    if (s->have_host && s->host.pointer_delta)
        s->host.pointer_delta(s->host.user, out_xy);
    ret = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, ret, 0, JS_NewFloat64(ctx, (double)out_xy[0]));
    JS_SetPropertyUint32(ctx, ret, 1, JS_NewFloat64(ctx, (double)out_xy[1]));
    return ret;
}

/* jce.get_pointer_wheel - shape: value_return */
static JSValue js_jce_get_pointer_wheel(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    float v = (s->have_host && s->host.pointer_wheel)
                ? s->host.pointer_wheel(s->host.user) : 0.0f;
    ret = JS_NewFloat64(ctx, (double)v);
    return ret;
}

/* jce.is_pointer_down - shape: value_return */
static JSValue js_jce_is_pointer_down(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double button_d = 0.0;
    if (JS_ToFloat64(ctx, &button_d, argv[0]) < 0) goto fail;
    int button = (int)button_d;
    bool v = (s->have_host && s->host.pointer_button)
                ? s->host.pointer_button(s->host.user, button) : false;
    ret = JS_NewBool(ctx, v);
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.get_touch_count - shape: value_return
 * A host returning a negative count is clamped to 0 so `for i = 1, jce.get_touch_count()` cannot underflow. */
static JSValue js_jce_get_touch_count(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    int v = (s->have_host && s->host.touch_count)
                ? s->host.touch_count(s->host.user) : 0;
    if (v < 0)
        v = 0;
    ret = JS_NewInt32(ctx, (int32_t)v);
    return ret;
}

/* jce.get_touch - shape: fallible_out
 * 1-based Lua index mapped to 0-based C; an index below 1 returns nil without calling the host. */
static JSValue js_jce_get_touch(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double js_index_d = 0.0;
    if (JS_ToFloat64(ctx, &js_index_d, argv[0]) < 0) goto fail;
    long js_index = (long)js_index_d;
    uint64_t id = 0;
    float x = 0.0f;
    float y = 0.0f;
    float pressure = 0.0f;
    if (js_index < 1 || !s->have_host || !s->host.touch_get ||
        !s->host.touch_get(s->host.user, (int)(js_index - 1), &id, &x, &y, &pressure)) {
        ret = JS_NULL;   /* miss */
    } else {
        ret = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, ret, 0, JS_NewFloat64(ctx, (double)id));
        JS_SetPropertyUint32(ctx, ret, 1, JS_NewFloat64(ctx, (double)x));
        JS_SetPropertyUint32(ctx, ret, 2, JS_NewFloat64(ctx, (double)y));
        JS_SetPropertyUint32(ctx, ret, 3, JS_NewFloat64(ctx, (double)pressure));
    }
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.tr - shape: value_return
 * Passthrough is the contract, not a fallback: an unlocalized build shows readable keys instead of blank UI. */
static JSValue js_jce_tr(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    const char *key = NULL, *key_cs = NULL;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    key_cs = JS_ToCString(ctx, argv[0]);
    if (!key_cs) goto fail;
    key = key_cs;
    const char * v = (s->have_host && s->host.loc_translate)
                ? s->host.loc_translate(s->host.user, key) : key;
    ret = JS_NewString(ctx, v ? v : key);
    if (key_cs) JS_FreeCString(ctx, key_cs);
    return ret;
fail:
    if (key_cs) JS_FreeCString(ctx, key_cs);
    return JS_EXCEPTION;
}

/* jce.get_locale - shape: value_return */
static JSValue js_jce_get_locale(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    const char * v = (s->have_host && s->host.loc_get_locale)
                ? s->host.loc_get_locale(s->host.user) : "";
    ret = JS_NewString(ctx, v ? v : "");
    return ret;
}

/* jce.set_locale - shape: void_call */
static JSValue js_jce_set_locale(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    const char *locale = NULL, *locale_cs = NULL;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    locale_cs = JS_ToCString(ctx, argv[0]);
    if (!locale_cs) goto fail;
    locale = locale_cs;
    if (s->have_host && s->host.loc_set_locale)
        s->host.loc_set_locale(s->host.user, locale);
    ret = JS_UNDEFINED;
    if (locale_cs) JS_FreeCString(ctx, locale_cs);
    return ret;
fail:
    if (locale_cs) JS_FreeCString(ctx, locale_cs);
    return JS_EXCEPTION;
}

/* jce.get_velocity - shape: fallible_out */
static JSValue js_jce_get_velocity(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    float out[3];
    if (s->have_host && s->host.get_velocity &&
        s->host.get_velocity(s->host.user, e, out)) {
        ret = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, ret, 0, JS_NewFloat64(ctx, (double)out[0]));
        JS_SetPropertyUint32(ctx, ret, 1, JS_NewFloat64(ctx, (double)out[1]));
        JS_SetPropertyUint32(ctx, ret, 2, JS_NewFloat64(ctx, (double)out[2]));
    } else {
        ret = JS_NULL;   /* miss */
    }
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.vehicle_set_input - shape: void_call */
static JSValue js_jce_vehicle_set_input(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    double throttle_d = 0.0;
    if (JS_ToFloat64(ctx, &throttle_d, argv[1]) < 0) goto fail;
    float throttle = (float)throttle_d;
    double brake_d = 0.0;
    if (JS_ToFloat64(ctx, &brake_d, argv[2]) < 0) goto fail;
    float brake = (float)brake_d;
    double steer_d = 0.0;
    if (JS_ToFloat64(ctx, &steer_d, argv[3]) < 0) goto fail;
    float steer = (float)steer_d;
    if (s->have_host && s->host.vehicle_set_input)
        s->host.vehicle_set_input(s->host.user, e, throttle, brake, steer);
    ret = JS_UNDEFINED;
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.vehicle_get_speed - shape: value_return */
static JSValue js_jce_vehicle_get_speed(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    float v = (s->have_host && s->host.vehicle_get_speed)
                ? s->host.vehicle_get_speed(s->host.user, e) : 0.0f;
    ret = JS_NewFloat64(ctx, (double)v);
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.get_move - shape: void_out_array */
static JSValue js_jce_get_move(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    float out[3] = { 0.0f, 0.0f, 0.0f };
    if (s->have_host && s->host.get_move)
        s->host.get_move(s->host.user, out);
    ret = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, ret, 0, JS_NewFloat64(ctx, (double)out[0]));
    JS_SetPropertyUint32(ctx, ret, 1, JS_NewFloat64(ctx, (double)out[1]));
    JS_SetPropertyUint32(ctx, ret, 2, JS_NewFloat64(ctx, (double)out[2]));
    return ret;
}

/* jce.ui_get_slider - shape: fallible_out */
static JSValue js_jce_ui_get_slider(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    float out = 0.0f;
    if (s->have_host && s->host.ui_get_slider &&
        s->host.ui_get_slider(s->host.user, e, &out)) {
        ret = JS_NewFloat64(ctx, (double)out);
    } else {
        ret = JS_NULL;   /* miss */
    }
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.ui_set_slider - shape: void_call */
static JSValue js_jce_ui_set_slider(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    double v_d = 0.0;
    if (JS_ToFloat64(ctx, &v_d, argv[1]) < 0) goto fail;
    float v = (float)v_d;
    if (s->have_host && s->host.ui_set_slider)
        s->host.ui_set_slider(s->host.user, e, v);
    ret = JS_UNDEFINED;
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.ui_get_progress - shape: fallible_out */
static JSValue js_jce_ui_get_progress(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    float out = 0.0f;
    if (s->have_host && s->host.ui_get_progress &&
        s->host.ui_get_progress(s->host.user, e, &out)) {
        ret = JS_NewFloat64(ctx, (double)out);
    } else {
        ret = JS_NULL;   /* miss */
    }
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.ui_set_progress - shape: void_call */
static JSValue js_jce_ui_set_progress(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    double v_d = 0.0;
    if (JS_ToFloat64(ctx, &v_d, argv[1]) < 0) goto fail;
    float v = (float)v_d;
    if (s->have_host && s->host.ui_set_progress)
        s->host.ui_set_progress(s->host.user, e, v);
    ret = JS_UNDEFINED;
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.ui_get_toggle - shape: fallible_out */
static JSValue js_jce_ui_get_toggle(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    bool out = false;
    if (s->have_host && s->host.ui_get_toggle &&
        s->host.ui_get_toggle(s->host.user, e, &out)) {
        ret = JS_NewBool(ctx, out);
    } else {
        ret = JS_NULL;   /* miss */
    }
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.ui_set_toggle - shape: void_call */
static JSValue js_jce_ui_set_toggle(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    bool v = JS_ToBool(ctx, argv[1]) != 0;
    if (s->have_host && s->host.ui_set_toggle)
        s->host.ui_set_toggle(s->host.user, e, v);
    ret = JS_UNDEFINED;
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.ui_set_text - shape: void_call */
static JSValue js_jce_ui_set_text(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    const char *txt = NULL, *txt_cs = NULL;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    txt_cs = JS_ToCString(ctx, argv[1]);
    if (!txt_cs) goto fail;
    txt = txt_cs;
    if (s->have_host && s->host.ui_set_text)
        s->host.ui_set_text(s->host.user, e, txt);
    ret = JS_UNDEFINED;
    if (txt_cs) JS_FreeCString(ctx, txt_cs);
    return ret;
fail:
    if (txt_cs) JS_FreeCString(ctx, txt_cs);
    return JS_EXCEPTION;
}

/* jce.send_message - shape: void_call */
static JSValue js_jce_send_message(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    const char *msg = NULL, *msg_cs = NULL;
    const char *str_arg = NULL, *str_arg_cs = NULL;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double target_d = 0.0;
    if (JS_ToFloat64(ctx, &target_d, argv[0]) < 0) goto fail;
    JceScriptEntity target = (JceScriptEntity)target_d;
    msg_cs = JS_ToCString(ctx, argv[1]);
    if (!msg_cs) goto fail;
    msg = msg_cs;
    double number_arg_d = 0.0;
    if (argc > 2 && !JS_IsUndefined(argv[2]) &&
        JS_ToFloat64(ctx, &number_arg_d, argv[2]) < 0) goto fail;
    double number_arg = number_arg_d;
    if (argc > 3 && !JS_IsUndefined(argv[3]) && !JS_IsNull(argv[3]))
        str_arg_cs = JS_ToCString(ctx, argv[3]);
    str_arg = str_arg_cs;
    if (s->have_host && s->host.send_message)
        s->host.send_message(s->host.user, target, msg, number_arg, str_arg);
    ret = JS_UNDEFINED;
    if (msg_cs) JS_FreeCString(ctx, msg_cs);
    if (str_arg_cs) JS_FreeCString(ctx, str_arg_cs);
    return ret;
fail:
    if (msg_cs) JS_FreeCString(ctx, msg_cs);
    if (str_arg_cs) JS_FreeCString(ctx, str_arg_cs);
    return JS_EXCEPTION;
}

/* jce.broadcast - shape: void_call */
static JSValue js_jce_broadcast(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    const char *msg = NULL, *msg_cs = NULL;
    const char *str_arg = NULL, *str_arg_cs = NULL;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    msg_cs = JS_ToCString(ctx, argv[0]);
    if (!msg_cs) goto fail;
    msg = msg_cs;
    double number_arg_d = 0.0;
    if (argc > 1 && !JS_IsUndefined(argv[1]) &&
        JS_ToFloat64(ctx, &number_arg_d, argv[1]) < 0) goto fail;
    double number_arg = number_arg_d;
    if (argc > 2 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2]))
        str_arg_cs = JS_ToCString(ctx, argv[2]);
    str_arg = str_arg_cs;
    if (s->have_host && s->host.broadcast)
        s->host.broadcast(s->host.user, msg, number_arg, str_arg);
    ret = JS_UNDEFINED;
    if (msg_cs) JS_FreeCString(ctx, msg_cs);
    if (str_arg_cs) JS_FreeCString(ctx, str_arg_cs);
    return ret;
fail:
    if (msg_cs) JS_FreeCString(ctx, msg_cs);
    if (str_arg_cs) JS_FreeCString(ctx, str_arg_cs);
    return JS_EXCEPTION;
}

/* jce.has_component - shape: value_return */
static JSValue js_jce_has_component(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    const char *comp_name = NULL, *comp_name_cs = NULL;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    comp_name_cs = JS_ToCString(ctx, argv[1]);
    if (!comp_name_cs) goto fail;
    comp_name = comp_name_cs;
    bool v = (s->have_host && s->host.has_component)
                ? s->host.has_component(s->host.user, e, comp_name) : false;
    ret = JS_NewBool(ctx, v);
    if (comp_name_cs) JS_FreeCString(ctx, comp_name_cs);
    return ret;
fail:
    if (comp_name_cs) JS_FreeCString(ctx, comp_name_cs);
    return JS_EXCEPTION;
}

/* jce.is_component_enabled - shape: value_return */
static JSValue js_jce_is_component_enabled(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    const char *comp_name = NULL, *comp_name_cs = NULL;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    comp_name_cs = JS_ToCString(ctx, argv[1]);
    if (!comp_name_cs) goto fail;
    comp_name = comp_name_cs;
    bool v = (s->have_host && s->host.is_component_enabled)
                ? s->host.is_component_enabled(s->host.user, e, comp_name) : false;
    ret = JS_NewBool(ctx, v);
    if (comp_name_cs) JS_FreeCString(ctx, comp_name_cs);
    return ret;
fail:
    if (comp_name_cs) JS_FreeCString(ctx, comp_name_cs);
    return JS_EXCEPTION;
}

/* jce.set_component_enabled - shape: void_call */
static JSValue js_jce_set_component_enabled(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    const char *comp_name = NULL, *comp_name_cs = NULL;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    comp_name_cs = JS_ToCString(ctx, argv[1]);
    if (!comp_name_cs) goto fail;
    comp_name = comp_name_cs;
    bool on = JS_ToBool(ctx, argv[2]) != 0;
    if (s->have_host && s->host.set_component_enabled)
        s->host.set_component_enabled(s->host.user, e, comp_name, on);
    ret = JS_UNDEFINED;
    if (comp_name_cs) JS_FreeCString(ctx, comp_name_cs);
    return ret;
fail:
    if (comp_name_cs) JS_FreeCString(ctx, comp_name_cs);
    return JS_EXCEPTION;
}

/* jce.net_is_server - shape: value_return */
static JSValue js_jce_net_is_server(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    bool v = (s->have_host && s->host.net_is_server)
                ? s->host.net_is_server(s->host.user) : false;
    ret = JS_NewBool(ctx, v);
    return ret;
}

/* jce.net_is_client - shape: value_return */
static JSValue js_jce_net_is_client(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    bool v = (s->have_host && s->host.net_is_client)
                ? s->host.net_is_client(s->host.user) : false;
    ret = JS_NewBool(ctx, v);
    return ret;
}

/* jce.net_spawn - shape: value_return */
static JSValue js_jce_net_spawn(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    const char *prefab_path = NULL, *prefab_path_cs = NULL;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    prefab_path_cs = JS_ToCString(ctx, argv[0]);
    if (!prefab_path_cs) goto fail;
    prefab_path = prefab_path_cs;
    double x_d = 0.0;
    if (JS_ToFloat64(ctx, &x_d, argv[1]) < 0) goto fail;
    float x = (float)x_d;
    double y_d = 0.0;
    if (JS_ToFloat64(ctx, &y_d, argv[2]) < 0) goto fail;
    float y = (float)y_d;
    double z_d = 0.0;
    if (JS_ToFloat64(ctx, &z_d, argv[3]) < 0) goto fail;
    float z = (float)z_d;
    JceScriptEntity v = (s->have_host && s->host.net_spawn)
                ? s->host.net_spawn(s->host.user, prefab_path, x, y, z) : 0;
    ret = JS_NewUint32(ctx, (uint32_t)v);
    if (prefab_path_cs) JS_FreeCString(ctx, prefab_path_cs);
    return ret;
fail:
    if (prefab_path_cs) JS_FreeCString(ctx, prefab_path_cs);
    return JS_EXCEPTION;
}

/* jce.rpc_send - shape: value_return */
static JSValue js_jce_rpc_send(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    const char *event = NULL, *event_cs = NULL;
    const char *payload = NULL, *payload_cs = NULL;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    event_cs = JS_ToCString(ctx, argv[1]);
    if (!event_cs) goto fail;
    event = event_cs;
    double target_d = 0;
    if (argc > 2 && !JS_IsUndefined(argv[2]) &&
        JS_ToFloat64(ctx, &target_d, argv[2]) < 0) goto fail;
    int target = (int)target_d;
    if (argc > 3 && !JS_IsUndefined(argv[3]) && !JS_IsNull(argv[3]))
        payload_cs = JS_ToCString(ctx, argv[3]);
    payload = payload_cs;
    bool v = (s->have_host && s->host.rpc_send)
                ? s->host.rpc_send(s->host.user, e, event, target, payload) : false;
    ret = JS_NewBool(ctx, v);
    if (event_cs) JS_FreeCString(ctx, event_cs);
    if (payload_cs) JS_FreeCString(ctx, payload_cs);
    return ret;
fail:
    if (event_cs) JS_FreeCString(ctx, event_cs);
    if (payload_cs) JS_FreeCString(ctx, payload_cs);
    return JS_EXCEPTION;
}

/* jce.particle_burst - shape: void_call */
static JSValue js_jce_particle_burst(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    double count_d = 0.0;
    if (JS_ToFloat64(ctx, &count_d, argv[1]) < 0) goto fail;
    int count = (int)count_d;
    if (s->have_host && s->host.particle_burst)
        s->host.particle_burst(s->host.user, e, count);
    ret = JS_UNDEFINED;
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.particle_set_emitting - shape: void_call */
static JSValue js_jce_particle_set_emitting(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    bool on = JS_ToBool(ctx, argv[1]) != 0;
    if (s->have_host && s->host.particle_set_emitting)
        s->host.particle_set_emitting(s->host.user, e, on);
    ret = JS_UNDEFINED;
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.particle_set_color - shape: void_call */
static JSValue js_jce_particle_set_color(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    double r_d = 0.0;
    if (JS_ToFloat64(ctx, &r_d, argv[1]) < 0) goto fail;
    float r = (float)r_d;
    double g_d = 0.0;
    if (JS_ToFloat64(ctx, &g_d, argv[2]) < 0) goto fail;
    float g = (float)g_d;
    double b_d = 0.0;
    if (JS_ToFloat64(ctx, &b_d, argv[3]) < 0) goto fail;
    float b = (float)b_d;
    if (s->have_host && s->host.particle_set_color)
        s->host.particle_set_color(s->host.user, e, r, g, b);
    ret = JS_UNDEFINED;
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.find_by_name - shape: first_and_count
 * first-or-nil AND a match count, so a strict scene director can reject duplicate authored names. IDENTICAL C signature to find_by_prefix and a DIFFERENT contract. */
static JSValue js_jce_find_by_name(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    const char *name = NULL, *name_cs = NULL;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    name_cs = JS_ToCString(ctx, argv[0]);
    if (!name_cs) goto fail;
    name = name_cs;
    JceScriptEntity found[2];
    int n = 0;
    if (s->have_host && s->host.find_by_name)
        n = s->host.find_by_name(s->host.user, name, found, 2);
    ret = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, ret, 0,
                         n > 0 ? JS_NewUint32(ctx, (uint32_t)found[0]) : JS_NULL);
    JS_SetPropertyUint32(ctx, ret, 1, JS_NewInt32(ctx, (int32_t)n));
    if (name_cs) JS_FreeCString(ctx, name_cs);
    return ret;
fail:
    if (name_cs) JS_FreeCString(ctx, name_cs);
    return JS_EXCEPTION;
}

/* jce.find_by_prefix - shape: entity_table
 * One Lua array. IDENTICAL C signature to find_by_name and a DIFFERENT contract. */
static JSValue js_jce_find_by_prefix(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    const char *prefix = NULL, *prefix_cs = NULL;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    prefix_cs = JS_ToCString(ctx, argv[0]);
    if (!prefix_cs) goto fail;
    prefix = prefix_cs;
    JceScriptEntity found[1024];
    int n = 0;
    if (s->have_host && s->host.find_by_prefix)
        n = s->host.find_by_prefix(s->host.user, prefix, found, 1024);
    {
        int fi;
        ret = JS_NewArray(ctx);
        for (fi = 0; fi < n; ++fi)
            JS_SetPropertyUint32(ctx, ret, (uint32_t)fi,
                                 JS_NewUint32(ctx, (uint32_t)found[fi]));
    }
    if (prefix_cs) JS_FreeCString(ctx, prefix_cs);
    return ret;
fail:
    if (prefix_cs) JS_FreeCString(ctx, prefix_cs);
    return JS_EXCEPTION;
}

/* jce.comp_get - shape: owned_string_release */
static JSValue js_jce_comp_get(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    const char *type = NULL, *type_cs = NULL;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    type_cs = JS_ToCString(ctx, argv[1]);
    if (!type_cs) goto fail;
    type = type_cs;
    char *owned = NULL;
    if (s->have_host && s->host.comp_get_json)
        owned = s->host.comp_get_json(s->host.user, e, type);
    if (owned) {
        ret = JS_NewString(ctx, owned);
        if (s->host.json_free) s->host.json_free(s->host.user, owned);
    } else {
        ret = JS_NULL;
    }
    if (type_cs) JS_FreeCString(ctx, type_cs);
    return ret;
fail:
    if (type_cs) JS_FreeCString(ctx, type_cs);
    return JS_EXCEPTION;
}

/* jce.comp_set - shape: value_return */
static JSValue js_jce_comp_set(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    const char *type = NULL, *type_cs = NULL;
    const char *json = NULL, *json_cs = NULL;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    type_cs = JS_ToCString(ctx, argv[1]);
    if (!type_cs) goto fail;
    type = type_cs;
    json_cs = JS_ToCString(ctx, argv[2]);
    if (!json_cs) goto fail;
    json = json_cs;
    bool v = (s->have_host && s->host.comp_set_json)
                ? s->host.comp_set_json(s->host.user, e, type, json) : false;
    ret = JS_NewBool(ctx, v);
    if (type_cs) JS_FreeCString(ctx, type_cs);
    if (json_cs) JS_FreeCString(ctx, json_cs);
    return ret;
fail:
    if (type_cs) JS_FreeCString(ctx, type_cs);
    if (json_cs) JS_FreeCString(ctx, json_cs);
    return JS_EXCEPTION;
}

/* jce.render_get - shape: owned_string_release */
static JSValue js_jce_render_get(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    char *owned = NULL;
    if (s->have_host && s->host.render_get_json)
        owned = s->host.render_get_json(s->host.user);
    if (owned) {
        ret = JS_NewString(ctx, owned);
        if (s->host.json_free) s->host.json_free(s->host.user, owned);
    } else {
        ret = JS_NULL;
    }
    return ret;
}

/* jce.render_set - shape: value_return */
static JSValue js_jce_render_set(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    const char *json = NULL, *json_cs = NULL;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    json_cs = JS_ToCString(ctx, argv[0]);
    if (!json_cs) goto fail;
    json = json_cs;
    bool v = (s->have_host && s->host.render_set_json)
                ? s->host.render_set_json(s->host.user, json) : false;
    ret = JS_NewBool(ctx, v);
    if (json_cs) JS_FreeCString(ctx, json_cs);
    return ret;
fail:
    if (json_cs) JS_FreeCString(ctx, json_cs);
    return JS_EXCEPTION;
}

/* jce.audio_set_volume - shape: void_call */
static JSValue js_jce_audio_set_volume(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    double volume_d = 0.0;
    if (JS_ToFloat64(ctx, &volume_d, argv[1]) < 0) goto fail;
    float volume = (float)volume_d;
    if (s->have_host && s->host.audio_set_volume)
        s->host.audio_set_volume(s->host.user, e, volume);
    ret = JS_UNDEFINED;
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.ui_get_dropdown - shape: fallible_out
 * Selected option INDEX of `entity`'s UIDropdown. Absent when the entity has no dropdown, so a script can tell 'no dropdown' from 'a dropdown reading 0'. The index and not the label: branching on which option is the common case, and a label would make it a string compare. */
static JSValue js_jce_ui_get_dropdown(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    int out = 0;
    if (s->have_host && s->host.ui_get_dropdown &&
        s->host.ui_get_dropdown(s->host.user, e, &out)) {
        ret = JS_NewInt32(ctx, (int32_t)out);
    } else {
        ret = JS_NULL;   /* miss */
    }
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.ui_set_dropdown - shape: void_call
 * Select an option by INDEX. Clamped into [0, option_count-1] rather than refused, the way ui_set_progress clamps and the way the scene loader clamps: the draw already clamps, so storing outside the range would make the component and the picture disagree. */
static JSValue js_jce_ui_set_dropdown(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    double index_d = 0.0;
    if (JS_ToFloat64(ctx, &index_d, argv[1]) < 0) goto fail;
    int index = (int)index_d;
    if (s->have_host && s->host.ui_set_dropdown)
        s->host.ui_set_dropdown(s->host.user, e, index);
    ret = JS_UNDEFINED;
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.ui_get_input_text - shape: value_return
 * Current text of `entity`'s UIInputField, or '' when it has none. The string is the component's own buffer and is valid until the next mutation of that entity -- the same contract tr() and get_locale() carry; every binding copies it and none may store it. */
static JSValue js_jce_ui_get_input_text(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    const char * v = (s->have_host && s->host.ui_get_input_text)
                ? s->host.ui_get_input_text(s->host.user, e) : "";
    ret = JS_NewString(ctx, v ? v : "");
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.ui_set_input_text - shape: void_call
 * Replace the UIInputField's text. Truncated to the field's capacity and to char_limit when one is set -- the same cap the canvas applies to typed input, so a script write and a keystroke cannot disagree about what the field holds. A truncation is logged rather than silent. */
static JSValue js_jce_ui_set_input_text(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    const char *text = NULL, *text_cs = NULL;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    text_cs = JS_ToCString(ctx, argv[1]);
    if (!text_cs) goto fail;
    text = text_cs;
    if (s->have_host && s->host.ui_set_input_text)
        s->host.ui_set_input_text(s->host.user, e, text);
    ret = JS_UNDEFINED;
    if (text_cs) JS_FreeCString(ctx, text_cs);
    return ret;
fail:
    if (text_cs) JS_FreeCString(ctx, text_cs);
    return JS_EXCEPTION;
}

/* jce.ui_get_scroll - shape: fallible_out
 * Scroll offset (x, y) of `entity`'s UIScrollView, in REFERENCE units -- what the component stores and what the wheel path clamps, not device px. Absent when the entity has no scroll view. */
static JSValue js_jce_ui_get_scroll(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    float out_xy[2];
    if (s->have_host && s->host.ui_get_scroll &&
        s->host.ui_get_scroll(s->host.user, e, out_xy)) {
        ret = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, ret, 0, JS_NewFloat64(ctx, (double)out_xy[0]));
        JS_SetPropertyUint32(ctx, ret, 1, JS_NewFloat64(ctx, (double)out_xy[1]));
    } else {
        ret = JS_NULL;   /* miss */
    }
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.ui_set_scroll - shape: void_call
 * Set the scroll offset in reference units. A disabled axis is pinned to 0 and each axis is clamped the way the wheel path clamps, so a script cannot push the offset somewhere a wheel could not; the canvas re-clamps against the resolved viewport on the next render. */
static JSValue js_jce_ui_set_scroll(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    double x_d = 0.0;
    if (JS_ToFloat64(ctx, &x_d, argv[1]) < 0) goto fail;
    float x = (float)x_d;
    double y_d = 0.0;
    if (JS_ToFloat64(ctx, &y_d, argv[2]) < 0) goto fail;
    float y = (float)y_d;
    if (s->have_host && s->host.ui_set_scroll)
        s->host.ui_set_scroll(s->host.user, e, x, y);
    ret = JS_UNDEFINED;
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.world_get_hour - shape: value_return
 * Live hour of day in [0, 24) -- what the sky is showing now, NOT the authored tod_hour seed a scene starts from.  Reading the seed would return the level's start-of-day forever while the sky moved. */
static JSValue js_jce_world_get_hour(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    float v = (s->have_host && s->host.world_get_hour)
                ? s->host.world_get_hour(s->host.user) : 0.0f;
    ret = JS_NewFloat64(ctx, (double)v);
    return ret;
}

/* jce.world_set_hour - shape: void_call
 * Move the live clock, wrapping into [0, 24).  For 'sleep until dawn'.  The authored seed is untouched, so reloading the scene still starts where the designer set it. */
static JSValue js_jce_world_set_hour(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double hour_d = 0.0;
    if (JS_ToFloat64(ctx, &hour_d, argv[0]) < 0) goto fail;
    float hour = (float)hour_d;
    if (s->have_host && s->host.world_set_hour)
        s->host.world_set_hour(s->host.user, hour);
    ret = JS_UNDEFINED;
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.world_is_daytime - shape: value_return
 * True while the sun is above the horizon.  THE predicate for 'is it night?' -- every key-light chooser in the engine is required to agree on this one, so a script that rolled its own threshold would disagree with the lighting it can see. */
static JSValue js_jce_world_is_daytime(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    bool v = (s->have_host && s->host.world_is_daytime)
                ? s->host.world_is_daytime(s->host.user) : false;
    ret = JS_NewBool(ctx, v);
    return ret;
}

/* jce.world_get_weather - shape: value_return
 * Authored weather type: 0 clear, 1 rain, 2 snow. */
static JSValue js_jce_world_get_weather(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    int v = (s->have_host && s->host.world_get_weather)
                ? s->host.world_get_weather(s->host.user) : 0;
    ret = JS_NewInt32(ctx, (int32_t)v);
    return ret;
}

/* jce.world_get_weather_intensity - shape: value_return
 * Authored weather intensity in [0, 1]. */
static JSValue js_jce_world_get_weather_intensity(JSContext *ctx, JSValueConst this_val,
                                                  int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    float v = (s->have_host && s->host.world_get_weather_intensity)
                ? s->host.world_get_weather_intensity(s->host.user) : 0.0f;
    ret = JS_NewFloat64(ctx, (double)v);
    return ret;
}

/* jce.world_get_wind_speed - shape: value_return
 * Instantaneous wind speed in m/s -- the sustained speed plus this moment's gust.  Do NOT key a cache on it: it changes every frame by design.  It is the same number the ocean spectrum and the vegetation shader read, so a script cannot disagree with what is on screen. */
static JSValue js_jce_world_get_wind_speed(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    float v = (s->have_host && s->host.world_get_wind_speed)
                ? s->host.world_get_wind_speed(s->host.user) : 0.0f;
    ret = JS_NewFloat64(ctx, (double)v);
    return ret;
}

/* jce.request_scene - shape: value_return */
static JSValue js_jce_request_scene(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    const char *scene_path = NULL, *scene_path_cs = NULL;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    scene_path_cs = JS_ToCString(ctx, argv[0]);
    if (!scene_path_cs) goto fail;
    scene_path = scene_path_cs;
    bool v = (s->have_host && s->host.request_scene)
                ? s->host.request_scene(s->host.user, scene_path) : false;
    ret = JS_NewBool(ctx, v);
    if (scene_path_cs) JS_FreeCString(ctx, scene_path_cs);
    return ret;
fail:
    if (scene_path_cs) JS_FreeCString(ctx, scene_path_cs);
    return JS_EXCEPTION;
}

/* jce.is_transitioning - shape: value_return */
static JSValue js_jce_is_transitioning(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    bool v = (s->have_host && s->host.is_transitioning)
                ? s->host.is_transitioning(s->host.user) : false;
    ret = JS_NewBool(ctx, v);
    return ret;
}

/* jce.audio_play - shape: value_return */
static JSValue js_jce_audio_play(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    bool v = (s->have_host && s->host.audio_play)
                ? s->host.audio_play(s->host.user, e) : false;
    ret = JS_NewBool(ctx, v);
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.audio_stop - shape: value_return */
static JSValue js_jce_audio_stop(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    bool v = (s->have_host && s->host.audio_stop)
                ? s->host.audio_stop(s->host.user, e) : false;
    ret = JS_NewBool(ctx, v);
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.audio_is_playing - shape: value_return */
static JSValue js_jce_audio_is_playing(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    bool v = (s->have_host && s->host.audio_is_playing)
                ? s->host.audio_is_playing(s->host.user, e) : false;
    ret = JS_NewBool(ctx, v);
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.save_game - shape: value_return */
static JSValue js_jce_save_game(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    const char *path = NULL, *path_cs = NULL;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    path_cs = JS_ToCString(ctx, argv[0]);
    if (!path_cs) goto fail;
    path = path_cs;
    bool v = (s->have_host && s->host.save_game)
                ? s->host.save_game(s->host.user, path) : false;
    ret = JS_NewBool(ctx, v);
    if (path_cs) JS_FreeCString(ctx, path_cs);
    return ret;
fail:
    if (path_cs) JS_FreeCString(ctx, path_cs);
    return JS_EXCEPTION;
}

/* jce.load_game - shape: value_return */
static JSValue js_jce_load_game(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    const char *path = NULL, *path_cs = NULL;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    path_cs = JS_ToCString(ctx, argv[0]);
    if (!path_cs) goto fail;
    path = path_cs;
    bool v = (s->have_host && s->host.load_game)
                ? s->host.load_game(s->host.user, path) : false;
    ret = JS_NewBool(ctx, v);
    if (path_cs) JS_FreeCString(ctx, path_cs);
    return ret;
fail:
    if (path_cs) JS_FreeCString(ctx, path_cs);
    return JS_EXCEPTION;
}

/* jce.overlap_sphere - shape: entity_table
 * Entities whose collider overlaps the sphere, as one array. layer_mask 0 means all layers. Triggers are skipped. layer_mask is OPTIONAL: omitting it means every layer, which is what an explosion or a pickup check wants and keeps the common call to its coordinates and its size. */
static JSValue js_jce_overlap_sphere(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double x_d = 0.0;
    if (JS_ToFloat64(ctx, &x_d, argv[0]) < 0) goto fail;
    float x = (float)x_d;
    double y_d = 0.0;
    if (JS_ToFloat64(ctx, &y_d, argv[1]) < 0) goto fail;
    float y = (float)y_d;
    double z_d = 0.0;
    if (JS_ToFloat64(ctx, &z_d, argv[2]) < 0) goto fail;
    float z = (float)z_d;
    double radius_d = 0.0;
    if (JS_ToFloat64(ctx, &radius_d, argv[3]) < 0) goto fail;
    float radius = (float)radius_d;
    double layer_mask_d = 0;
    if (argc > 4 && !JS_IsUndefined(argv[4]) &&
        JS_ToFloat64(ctx, &layer_mask_d, argv[4]) < 0) goto fail;
    uint32_t layer_mask = (uint32_t)layer_mask_d;
    JceScriptEntity found[256];
    int n = 0;
    if (s->have_host && s->host.overlap_sphere)
        n = s->host.overlap_sphere(s->host.user, x, y, z, radius, layer_mask, found, 256);
    {
        int fi;
        ret = JS_NewArray(ctx);
        for (fi = 0; fi < n; ++fi)
            JS_SetPropertyUint32(ctx, ret, (uint32_t)fi,
                                 JS_NewUint32(ctx, (uint32_t)found[fi]));
    }
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.overlap_box - shape: entity_table
 * Entities whose collider overlaps the axis-aligned box (half-extents), as one array. layer_mask 0 means all layers. layer_mask is OPTIONAL: omitting it means every layer, which is what an explosion or a pickup check wants and keeps the common call to its coordinates and its size. */
static JSValue js_jce_overlap_box(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double x_d = 0.0;
    if (JS_ToFloat64(ctx, &x_d, argv[0]) < 0) goto fail;
    float x = (float)x_d;
    double y_d = 0.0;
    if (JS_ToFloat64(ctx, &y_d, argv[1]) < 0) goto fail;
    float y = (float)y_d;
    double z_d = 0.0;
    if (JS_ToFloat64(ctx, &z_d, argv[2]) < 0) goto fail;
    float z = (float)z_d;
    double hx_d = 0.0;
    if (JS_ToFloat64(ctx, &hx_d, argv[3]) < 0) goto fail;
    float hx = (float)hx_d;
    double hy_d = 0.0;
    if (JS_ToFloat64(ctx, &hy_d, argv[4]) < 0) goto fail;
    float hy = (float)hy_d;
    double hz_d = 0.0;
    if (JS_ToFloat64(ctx, &hz_d, argv[5]) < 0) goto fail;
    float hz = (float)hz_d;
    double layer_mask_d = 0;
    if (argc > 6 && !JS_IsUndefined(argv[6]) &&
        JS_ToFloat64(ctx, &layer_mask_d, argv[6]) < 0) goto fail;
    uint32_t layer_mask = (uint32_t)layer_mask_d;
    JceScriptEntity found[256];
    int n = 0;
    if (s->have_host && s->host.overlap_box)
        n = s->host.overlap_box(s->host.user, x, y, z, hx, hy, hz, layer_mask, found, 256);
    {
        int fi;
        ret = JS_NewArray(ctx);
        for (fi = 0; fi < n; ++fi)
            JS_SetPropertyUint32(ctx, ret, (uint32_t)fi,
                                 JS_NewUint32(ctx, (uint32_t)found[fi]));
    }
    return ret;
fail:
    return JS_EXCEPTION;
}

/* jce.get_param - shape: fallible_out
 * Returns kind, number, entity for an AUTHORED script parameter -- Unity's [SerializeField], Godot's @export.  Returns nil when the entity has no script component, when no parameter of that name is authored, or when the name is empty: three absences a script cannot act differently on, so `jce.get_param(e, 'speed') or 3.0` reads the way an author expects. */
static JSValue js_jce_get_param(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    const char *name = NULL, *name_cs = NULL;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    name_cs = JS_ToCString(ctx, argv[1]);
    if (!name_cs) goto fail;
    name = name_cs;
    int out_kind = 0;
    double out_number = 0.0;
    JceScriptEntity out_entity = 0;
    if (s->have_host && s->host.get_script_param &&
        s->host.get_script_param(s->host.user, e, name, &out_kind, &out_number, &out_entity)) {
        ret = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, ret, 0, JS_NewInt32(ctx, (int32_t)out_kind));
        JS_SetPropertyUint32(ctx, ret, 1, JS_NewFloat64(ctx, (double)out_number));
        JS_SetPropertyUint32(ctx, ret, 2, JS_NewUint32(ctx, (uint32_t)out_entity));
    } else {
        ret = JS_NULL;   /* miss */
    }
    if (name_cs) JS_FreeCString(ctx, name_cs);
    return ret;
fail:
    if (name_cs) JS_FreeCString(ctx, name_cs);
    return JS_EXCEPTION;
}

/* jce.get_param_text - shape: value_return
 * The TEXT value of an authored script parameter, or '' when the entity has no script component, no parameter of that name, or one that is not text. Empty rather than nil for the same reason ui_get_input_text is empty: a script comparing strings should not have to test for nil first. The string is the component's own buffer -- copy it if you keep it. */
static JSValue js_jce_get_param_text(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    const char *name = NULL, *name_cs = NULL;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    double e_d = 0.0;
    if (JS_ToFloat64(ctx, &e_d, argv[0]) < 0) goto fail;
    JceScriptEntity e = (JceScriptEntity)e_d;
    name_cs = JS_ToCString(ctx, argv[1]);
    if (!name_cs) goto fail;
    name = name_cs;
    const char * v = (s->have_host && s->host.get_script_param_text)
                ? s->host.get_script_param_text(s->host.user, e, name) : "";
    ret = JS_NewString(ctx, v ? v : "");
    if (name_cs) JS_FreeCString(ctx, name_cs);
    return ret;
fail:
    if (name_cs) JS_FreeCString(ctx, name_cs);
    return JS_EXCEPTION;
}

/* jce.curve_eval - shape: fallible_out
 * Sample an AUTHORED curve -- the documents the editor's Curve Editor writes, which nothing could read until this binding existed. Unity's AnimationCurve shape: the curve is a designer-authored function and the script decides what it means, so the engine never has to invent what a curve DRIVES. Returns nil when the path does not resolve, the document does not parse, the named channel is absent, or that channel has no keys -- so a curve that genuinely evaluates to 0 and a curve that is not there are never one reading, and `jce.curve_eval(p, 'kick', t) or 0.0` reads the way an author expects. An empty channel name means the FIRST channel, which is a different request from a name that is not there. The parsed curve is cached per runtime, so a call inside on_update costs a name compare, not a JSON parse. */
static JSValue js_jce_curve_eval(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    const char *path = NULL, *path_cs = NULL;
    const char *channel = NULL, *channel_cs = NULL;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    path_cs = JS_ToCString(ctx, argv[0]);
    if (!path_cs) goto fail;
    path = path_cs;
    channel_cs = JS_ToCString(ctx, argv[1]);
    if (!channel_cs) goto fail;
    channel = channel_cs;
    double t_d = 0.0;
    if (JS_ToFloat64(ctx, &t_d, argv[2]) < 0) goto fail;
    double t = t_d;
    double out_value = 0.0;
    if (s->have_host && s->host.curve_eval &&
        s->host.curve_eval(s->host.user, path, channel, t, &out_value)) {
        ret = JS_NewFloat64(ctx, (double)out_value);
    } else {
        ret = JS_NULL;   /* miss */
    }
    if (path_cs) JS_FreeCString(ctx, path_cs);
    if (channel_cs) JS_FreeCString(ctx, channel_cs);
    return ret;
fail:
    if (path_cs) JS_FreeCString(ctx, path_cs);
    if (channel_cs) JS_FreeCString(ctx, channel_cs);
    return JS_EXCEPTION;
}

/* jce.vcam_activate - shape: value_return
 * Cut to the virtual camera with this name, ahead of priority.  Returns 1 when the name resolves to a camera that is active and enabled, 0 otherwise -- the request is recorded either way, so naming a camera in a streaming cell that has not loaded yet does not silently become 'whatever priority says'.  Pass an empty string to clear it and hand the decision back to priority.  It does NOT rewrite the authored components: the override lives in the vcam system, so a cutscene cannot bake its camera choice into the level file. */
static JSValue js_jce_vcam_activate(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    JceJsScript *s = (JceJsScript *)JS_GetContextOpaque(ctx);
    JSValue ret = JS_UNDEFINED;
    const char *name = NULL, *name_cs = NULL;
    (void)this_val; (void)argc; (void)argv;
    if (!s) return JS_UNDEFINED;
    name_cs = JS_ToCString(ctx, argv[0]);
    if (!name_cs) goto fail;
    name = name_cs;
    int v = (s->have_host && s->host.vcam_activate)
                ? s->host.vcam_activate(s->host.user, name) : 0;
    ret = JS_NewInt32(ctx, (int32_t)v);
    if (name_cs) JS_FreeCString(ctx, name_cs);
    return ret;
fail:
    if (name_cs) JS_FreeCString(ctx, name_cs);
    return JS_EXCEPTION;
}

const char *const JCE_SCRIPT_JS_BINDING_NAMES[] = {
    "get_position",
    "set_position",
    "get_rotation",
    "set_rotation",
    "get_scale",
    "get_world_position",
    "set_scale",
    "set_parent",
    "get_parent",
    "is_key_down",
    "find_with_tag",
    "destroy",
    "spawn",
    "move_axis",
    "jump_pressed",
    "sprint",
    "attack_pressed",
    "set_time_scale",
    "pause",
    "shake_camera",
    "music_set_intensity",
    "music_get_intensity",
    "music_request_transition",
    "gas_activate",
    "gas_get",
    "gas_apply",
    "raycast",
    "raycast_filtered",
    "raycast_all",
    "apply_impulse",
    "set_velocity",
    "anim_set_float",
    "anim_set_int",
    "anim_set_bool",
    "anim_set_trigger",
    "is_action_down",
    "is_action_pressed",
    "get_axis",
    "get_pointer_delta",
    "get_pointer_wheel",
    "is_pointer_down",
    "get_touch_count",
    "get_touch",
    "tr",
    "get_locale",
    "set_locale",
    "get_velocity",
    "vehicle_set_input",
    "vehicle_get_speed",
    "get_move",
    "ui_get_slider",
    "ui_set_slider",
    "ui_get_progress",
    "ui_set_progress",
    "ui_get_toggle",
    "ui_set_toggle",
    "ui_set_text",
    "send_message",
    "broadcast",
    "has_component",
    "is_component_enabled",
    "set_component_enabled",
    "net_is_server",
    "net_is_client",
    "net_spawn",
    "rpc_send",
    "particle_burst",
    "particle_set_emitting",
    "particle_set_color",
    "find_by_name",
    "find_by_prefix",
    "comp_get",
    "comp_set",
    "render_get",
    "render_set",
    "audio_set_volume",
    "ui_get_dropdown",
    "ui_set_dropdown",
    "ui_get_input_text",
    "ui_set_input_text",
    "ui_get_scroll",
    "ui_set_scroll",
    "world_get_hour",
    "world_set_hour",
    "world_is_daytime",
    "world_get_weather",
    "world_get_weather_intensity",
    "world_get_wind_speed",
    "request_scene",
    "is_transitioning",
    "audio_play",
    "audio_stop",
    "audio_is_playing",
    "save_game",
    "load_game",
    "overlap_sphere",
    "overlap_box",
    "get_param",
    "get_param_text",
    "curve_eval",
    "vcam_activate",
};

static const JSCFunctionListEntry k_jce_fns[] = {
    JS_CFUNC_DEF("get_position", 1, js_jce_get_position),
    JS_CFUNC_DEF("set_position", 4, js_jce_set_position),
    JS_CFUNC_DEF("get_rotation", 1, js_jce_get_rotation),
    JS_CFUNC_DEF("set_rotation", 4, js_jce_set_rotation),
    JS_CFUNC_DEF("get_scale", 1, js_jce_get_scale),
    JS_CFUNC_DEF("get_world_position", 1, js_jce_get_world_position),
    JS_CFUNC_DEF("set_scale", 4, js_jce_set_scale),
    JS_CFUNC_DEF("set_parent", 3, js_jce_set_parent),
    JS_CFUNC_DEF("get_parent", 1, js_jce_get_parent),
    JS_CFUNC_DEF("is_key_down", 1, js_jce_is_key_down),
    JS_CFUNC_DEF("find_with_tag", 1, js_jce_find_with_tag),
    JS_CFUNC_DEF("destroy", 1, js_jce_destroy),
    JS_CFUNC_DEF("spawn", 4, js_jce_spawn),
    JS_CFUNC_DEF("move_axis", 0, js_jce_move_axis),
    JS_CFUNC_DEF("jump_pressed", 1, js_jce_jump_pressed),
    JS_CFUNC_DEF("sprint", 1, js_jce_sprint),
    JS_CFUNC_DEF("attack_pressed", 1, js_jce_attack_pressed),
    JS_CFUNC_DEF("set_time_scale", 1, js_jce_set_time_scale),
    JS_CFUNC_DEF("pause", 1, js_jce_pause),
    JS_CFUNC_DEF("shake_camera", 1, js_jce_shake_camera),
    JS_CFUNC_DEF("music_set_intensity", 1, js_jce_music_set_intensity),
    JS_CFUNC_DEF("music_get_intensity", 0, js_jce_music_get_intensity),
    JS_CFUNC_DEF("music_request_transition", 1, js_jce_music_request_transition),
    JS_CFUNC_DEF("gas_activate", 2, js_jce_gas_activate),
    JS_CFUNC_DEF("gas_get", 2, js_jce_gas_get),
    JS_CFUNC_DEF("gas_apply", 5, js_jce_gas_apply),
    JS_CFUNC_DEF("raycast", 3, js_jce_raycast),
    JS_CFUNC_DEF("raycast_filtered", 5, js_jce_raycast_filtered),
    JS_CFUNC_DEF("raycast_all", 5, js_jce_raycast_all),
    JS_CFUNC_DEF("apply_impulse", 4, js_jce_apply_impulse),
    JS_CFUNC_DEF("set_velocity", 4, js_jce_set_velocity),
    JS_CFUNC_DEF("anim_set_float", 3, js_jce_anim_set_float),
    JS_CFUNC_DEF("anim_set_int", 3, js_jce_anim_set_int),
    JS_CFUNC_DEF("anim_set_bool", 3, js_jce_anim_set_bool),
    JS_CFUNC_DEF("anim_set_trigger", 2, js_jce_anim_set_trigger),
    JS_CFUNC_DEF("is_action_down", 1, js_jce_is_action_down),
    JS_CFUNC_DEF("is_action_pressed", 1, js_jce_is_action_pressed),
    JS_CFUNC_DEF("get_axis", 1, js_jce_get_axis),
    JS_CFUNC_DEF("get_pointer_delta", 0, js_jce_get_pointer_delta),
    JS_CFUNC_DEF("get_pointer_wheel", 0, js_jce_get_pointer_wheel),
    JS_CFUNC_DEF("is_pointer_down", 1, js_jce_is_pointer_down),
    JS_CFUNC_DEF("get_touch_count", 0, js_jce_get_touch_count),
    JS_CFUNC_DEF("get_touch", 1, js_jce_get_touch),
    JS_CFUNC_DEF("tr", 1, js_jce_tr),
    JS_CFUNC_DEF("get_locale", 0, js_jce_get_locale),
    JS_CFUNC_DEF("set_locale", 1, js_jce_set_locale),
    JS_CFUNC_DEF("get_velocity", 1, js_jce_get_velocity),
    JS_CFUNC_DEF("vehicle_set_input", 4, js_jce_vehicle_set_input),
    JS_CFUNC_DEF("vehicle_get_speed", 1, js_jce_vehicle_get_speed),
    JS_CFUNC_DEF("get_move", 0, js_jce_get_move),
    JS_CFUNC_DEF("ui_get_slider", 1, js_jce_ui_get_slider),
    JS_CFUNC_DEF("ui_set_slider", 2, js_jce_ui_set_slider),
    JS_CFUNC_DEF("ui_get_progress", 1, js_jce_ui_get_progress),
    JS_CFUNC_DEF("ui_set_progress", 2, js_jce_ui_set_progress),
    JS_CFUNC_DEF("ui_get_toggle", 1, js_jce_ui_get_toggle),
    JS_CFUNC_DEF("ui_set_toggle", 2, js_jce_ui_set_toggle),
    JS_CFUNC_DEF("ui_set_text", 2, js_jce_ui_set_text),
    JS_CFUNC_DEF("send_message", 4, js_jce_send_message),
    JS_CFUNC_DEF("broadcast", 3, js_jce_broadcast),
    JS_CFUNC_DEF("has_component", 2, js_jce_has_component),
    JS_CFUNC_DEF("is_component_enabled", 2, js_jce_is_component_enabled),
    JS_CFUNC_DEF("set_component_enabled", 3, js_jce_set_component_enabled),
    JS_CFUNC_DEF("net_is_server", 0, js_jce_net_is_server),
    JS_CFUNC_DEF("net_is_client", 0, js_jce_net_is_client),
    JS_CFUNC_DEF("net_spawn", 4, js_jce_net_spawn),
    JS_CFUNC_DEF("rpc_send", 4, js_jce_rpc_send),
    JS_CFUNC_DEF("particle_burst", 2, js_jce_particle_burst),
    JS_CFUNC_DEF("particle_set_emitting", 2, js_jce_particle_set_emitting),
    JS_CFUNC_DEF("particle_set_color", 4, js_jce_particle_set_color),
    JS_CFUNC_DEF("find_by_name", 1, js_jce_find_by_name),
    JS_CFUNC_DEF("find_by_prefix", 1, js_jce_find_by_prefix),
    JS_CFUNC_DEF("comp_get", 2, js_jce_comp_get),
    JS_CFUNC_DEF("comp_set", 3, js_jce_comp_set),
    JS_CFUNC_DEF("render_get", 0, js_jce_render_get),
    JS_CFUNC_DEF("render_set", 1, js_jce_render_set),
    JS_CFUNC_DEF("audio_set_volume", 2, js_jce_audio_set_volume),
    JS_CFUNC_DEF("ui_get_dropdown", 1, js_jce_ui_get_dropdown),
    JS_CFUNC_DEF("ui_set_dropdown", 2, js_jce_ui_set_dropdown),
    JS_CFUNC_DEF("ui_get_input_text", 1, js_jce_ui_get_input_text),
    JS_CFUNC_DEF("ui_set_input_text", 2, js_jce_ui_set_input_text),
    JS_CFUNC_DEF("ui_get_scroll", 1, js_jce_ui_get_scroll),
    JS_CFUNC_DEF("ui_set_scroll", 3, js_jce_ui_set_scroll),
    JS_CFUNC_DEF("world_get_hour", 0, js_jce_world_get_hour),
    JS_CFUNC_DEF("world_set_hour", 1, js_jce_world_set_hour),
    JS_CFUNC_DEF("world_is_daytime", 0, js_jce_world_is_daytime),
    JS_CFUNC_DEF("world_get_weather", 0, js_jce_world_get_weather),
    JS_CFUNC_DEF("world_get_weather_intensity", 0, js_jce_world_get_weather_intensity),
    JS_CFUNC_DEF("world_get_wind_speed", 0, js_jce_world_get_wind_speed),
    JS_CFUNC_DEF("request_scene", 1, js_jce_request_scene),
    JS_CFUNC_DEF("is_transitioning", 0, js_jce_is_transitioning),
    JS_CFUNC_DEF("audio_play", 1, js_jce_audio_play),
    JS_CFUNC_DEF("audio_stop", 1, js_jce_audio_stop),
    JS_CFUNC_DEF("audio_is_playing", 1, js_jce_audio_is_playing),
    JS_CFUNC_DEF("save_game", 1, js_jce_save_game),
    JS_CFUNC_DEF("load_game", 1, js_jce_load_game),
    JS_CFUNC_DEF("overlap_sphere", 5, js_jce_overlap_sphere),
    JS_CFUNC_DEF("overlap_box", 7, js_jce_overlap_box),
    JS_CFUNC_DEF("get_param", 2, js_jce_get_param),
    JS_CFUNC_DEF("get_param_text", 2, js_jce_get_param_text),
    JS_CFUNC_DEF("curve_eval", 3, js_jce_curve_eval),
    JS_CFUNC_DEF("vcam_activate", 1, js_jce_vcam_activate),
};

bool jce_script_js_install_bindings(JceJsScript *s)
{
    JSValue global, jce;
    if (!s || !s->ctx) return false;
    global = JS_GetGlobalObject(s->ctx);
    jce = JS_NewObject(s->ctx);
    if (JS_IsException(jce)) {
        JS_FreeValue(s->ctx, jce);
        JS_FreeValue(s->ctx, global);
        return false;
    }
    JS_SetPropertyFunctionList(s->ctx, jce, k_jce_fns,
                               (int)(sizeof k_jce_fns / sizeof k_jce_fns[0]));
    /* json_null: lightuserdata_sentinel - not a function, so no
     * JS_CFUNC_DEF and no registration regex can see it. */
    JS_SetPropertyStr(s->ctx, jce, "json_null", JS_NULL);
    JS_SetPropertyStr(s->ctx, global, "jce", jce);
    JS_FreeValue(s->ctx, global);
    return true;
}
