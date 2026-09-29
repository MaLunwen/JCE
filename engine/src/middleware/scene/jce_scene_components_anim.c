/*
 * jce_scene_components_anim.c  Scene component (de)serialize module
 * for the anim domain (split from jce_scene_components_json.c).
 *
 * Pure move from the monolith: the shared JSON/Euler helpers live as
 * static inline in jce_scene_components_internal.h; the registry-referenced
 * parse_<x>/serw_<x> are external (declared in that header's shared section)
 * so the REG table can take their address; ser_<x> writers stay file-static.
 */

#include "jce_scene_components_internal.h"

void parse_sprite_animator(JceScene *s, JceEntity e, const cJSON *c)
{
    JceSpriteAnimatorComponent sa;
    memset(&sa, 0, sizeof(sa));
    copy_str(sa.sheet_path,    sizeof(sa.sheet_path),    j_str(c, "sheetPath", ""));
    copy_str(sa.atlas_path,    sizeof(sa.atlas_path),    j_str(c, "atlasPath", ""));
    sa.frame_width  = (int)j_num(c, "frameWidth",  0);
    sa.frame_height = (int)j_num(c, "frameHeight", 0);
    copy_str(sa.current_anim, sizeof(sa.current_anim), j_str(c, "currentAnim", ""));
    sa.speed   = (float)j_num(c, "speed", 1.0);
    sa.loop    = j_bool(c, "loop", true);
    sa.playing = j_bool(c, "playing", false);
    jce_scene_set_sprite_animator(s, e, &sa);
}

void parse_skeletal_animator(JceScene *s, JceEntity e, const cJSON *c)
{
    JceSkeletalAnimatorComponent sk;
    memset(&sk, 0, sizeof(sk));
    copy_str(sk.skeleton_path, sizeof(sk.skeleton_path), j_str(c, "skeletonPath", ""));
    /* Optional retarget source skeleton (absent ⇒ empty ⇒ no retargeting). */
    copy_str(sk.retarget_source_skeleton, sizeof(sk.retarget_source_skeleton),
             j_str(c, "retargetSource", ""));
    copy_str(sk.sm_path, sizeof(sk.sm_path), j_str(c, "stateMachine", ""));
    sk.retarget_effector_ik = j_bool(c, "retargetEffectorIk", false);
    sk.retarget_twist = (float)j_num(c, "retargetTwist", 0.0);
    sk.use_blend_tree = j_bool(c, "useBlendTree", false);
    sk.blend_param    = (float)j_num(c, "blendParam", 0.0);
    sk.blend_mode     = (int)j_num(c, "blendMode", 0);
    if (sk.blend_mode < 0 || sk.blend_mode > 2) sk.blend_mode = 0;
    sk.blend_param_y  = (float)j_num(c, "blendParamY", 0.0);
    sk.auto_speed     = j_bool(c, "autoSpeed", false);
    {
        const cJSON *bts = cJSON_GetObjectItemCaseSensitive(c, "blendThresholds");
        if (cJSON_IsArray(bts)) {
            int bn = cJSON_GetArraySize(bts);
            if (bn > 8) bn = 8;
            for (int i = 0; i < bn; i++) {
                const cJSON *it = cJSON_GetArrayItem(bts, i);
                if (cJSON_IsNumber(it))
                    sk.blend_thresholds[i] = (float)it->valuedouble;
            }
        }
        const cJSON *bys = cJSON_GetObjectItemCaseSensitive(c, "blendPosY");
        if (cJSON_IsArray(bys)) {
            int bn = cJSON_GetArraySize(bys);
            if (bn > 8) bn = 8;
            for (int i = 0; i < bn; i++) {
                const cJSON *it = cJSON_GetArrayItem(bys, i);
                if (cJSON_IsNumber(it))
                    sk.blend_pos_y[i] = (float)it->valuedouble;
            }
        }
    }
    sk.speed       = (float)j_num(c, "speed", 1.0);
    sk.loop        = j_bool(c, "loop", true);
    sk.playing     = j_bool(c, "playing", false);
    sk.active_clip = (int)j_num(c, "activeClip", -1);

    const cJSON *clips = cJSON_GetObjectItemCaseSensitive(c, "clipNames");
    if (cJSON_IsArray(clips)) {
        int n = cJSON_GetArraySize(clips);
        if (n > 8) n = 8;
        sk.clip_count = n;
        for (int i = 0; i < n; i++) {
            const cJSON *it = cJSON_GetArrayItem(clips, i);
            if (cJSON_IsString(it))
                copy_str(sk.clip_names[i], sizeof(sk.clip_names[i]), it->valuestring);
        }
    }
    jce_scene_set_skeletal_animator(s, e, &sk);
}

void parse_sequence_player(JceScene *s, JceEntity e, const cJSON *c)
{
    JceSequencePlayerComponent sp; memset(&sp, 0, sizeof sp);
    copy_str(sp.seq_path, sizeof(sp.seq_path), j_str(c, "seqPath", ""));
    sp.play_on_awake = j_bool(c, "playOnAwake", false);
    sp.loop_override = j_bool(c, "loopOverride", false);
    sp.override_loop = j_bool(c, "overrideLoop", false);
    sp.speed         = (float)j_num(c, "speed", 1.0);
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(c, "bindings");
    if (cJSON_IsArray(arr)) {
        int n = cJSON_GetArraySize(arr);
        if (n > JCE_SEQ_PLAYER_MAX_BINDINGS) n = JCE_SEQ_PLAYER_MAX_BINDINGS;
        for (int i = 0; i < n; i++) {
            const cJSON *it = cJSON_GetArrayItem(arr, i);
            sp.bindings[sp.binding_count++] =
                cJSON_IsNumber(it) ? (uint64_t)it->valuedouble : 0;
        }
    }
    /* Runtime fields (seq/prev_time/started/opened_hash) stay zeroed. */
    jce_scene_set_sequence_player(s, e, &sp);
}

void parse_morph_weights(JceScene *s, JceEntity e, const cJSON *c)
{
    JceMorphWeightsComponent mw;
    const cJSON *arr;
    memset(&mw, 0, sizeof mw);
    mw.override_mask = (uint32_t)j_num(c, "overrideMask", 0.0);
    arr = cJSON_GetObjectItemCaseSensitive(c, "weights");
    if (cJSON_IsArray(arr)) {
        int n = cJSON_GetArraySize(arr);
        if (n > JCE_MORPH_MAX_WEIGHTS) n = JCE_MORPH_MAX_WEIGHTS;
        for (int i = 0; i < n; i++) {
            const cJSON *it = cJSON_GetArrayItem(arr, i);
            mw.weights[i] = cJSON_IsNumber(it) ? (float)it->valuedouble : 0.0f;
        }
        mw.count = n;
    }
    /* `count` may be explicitly authored (sliders shown without all weights
     * non-zero); honor it but clamp to the array we actually read. */
    {
        int authored = (int)j_num(c, "count", (double)mw.count);
        if (authored < 0) authored = 0;
        if (authored > JCE_MORPH_MAX_WEIGHTS) authored = JCE_MORPH_MAX_WEIGHTS;
        if (authored > mw.count) mw.count = authored;
    }
    jce_scene_set_morph_weights(s, e, &mw);
}

void parse_ik_constraints(JceScene *s, JceEntity e, const cJSON *c)
{
    JceIkConstraintComponent ik; memset(&ik, 0, sizeof ik);
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(c, "constraints");
    if (cJSON_IsArray(arr)) {
        int n = cJSON_GetArraySize(arr);
        int cap = (int)(sizeof(ik.constraints) / sizeof(ik.constraints[0]));
        for (int i = 0; i < n && ik.count < cap; i++) {
            const cJSON *o = cJSON_GetArrayItem(arr, i);
            if (!cJSON_IsObject(o)) continue;
            JceIkConstraint *k = &ik.constraints[ik.count++];
            k->kind    = (int)j_num(o, "kind", 0.0);
            copy_str(k->name, sizeof(k->name), j_str(o, "name", ""));
            k->weight  = (float)j_num(o, "weight", 1.0);
            k->enabled = j_bool(o, "enabled", true);
            copy_str(k->root_bone, sizeof(k->root_bone), j_str(o, "rootBone", ""));
            copy_str(k->mid_bone,  sizeof(k->mid_bone),  j_str(o, "midBone", ""));
            copy_str(k->end_bone,  sizeof(k->end_bone),  j_str(o, "endBone", ""));
            k->target_entity  = (uint32_t)j_num(o, "targetEntity", 0.0);
            k->pole_entity    = (uint32_t)j_num(o, "poleEntity", 0.0);
            k->pole_offset[0] = (float)j_num(o, "poleOffsetX", 0.0);
            k->pole_offset[1] = (float)j_num(o, "poleOffsetY", 0.0);
            k->pole_offset[2] = (float)j_num(o, "poleOffsetZ", 0.0);
        }
    }
    jce_scene_set_ik_constraints(s, e, &ik);
}

void parse_foot_ik(JceScene *s, JceEntity e, const cJSON *c)
{
    JceFootIkComponent f; memset(&f, 0, sizeof f);
    f.enabled          = j_bool(c, "enabled", false);
    copy_str(f.pelvis_bone, sizeof(f.pelvis_bone), j_str(c, "pelvisBone", ""));
    copy_str(f.hip_bone[0],   sizeof(f.hip_bone[0]),   j_str(c, "hipBoneL",   ""));
    copy_str(f.hip_bone[1],   sizeof(f.hip_bone[1]),   j_str(c, "hipBoneR",   ""));
    copy_str(f.knee_bone[0],  sizeof(f.knee_bone[0]),  j_str(c, "kneeBoneL",  ""));
    copy_str(f.knee_bone[1],  sizeof(f.knee_bone[1]),  j_str(c, "kneeBoneR",  ""));
    copy_str(f.ankle_bone[0], sizeof(f.ankle_bone[0]), j_str(c, "ankleBoneL", ""));
    copy_str(f.ankle_bone[1], sizeof(f.ankle_bone[1]), j_str(c, "ankleBoneR", ""));
    f.max_step_height  = (float)j_num(c, "maxStepHeight", 0.5);
    f.foot_offset      = (float)j_num(c, "footOffset", 0.02);
    f.cast_up          = (float)j_num(c, "castUp", 0.5);
    f.cast_down        = (float)j_num(c, "castDown", 0.6);
    f.rotate_to_normal = j_bool(c, "rotateToNormal", true);
    f.blend            = (float)j_num(c, "blend", 1.0);
    jce_scene_set_foot_ik(s, e, &f);
}

void parse_full_body_ik(JceScene *s, JceEntity e, const cJSON *c)
{
    JceFullBodyIkComponent f; memset(&f, 0, sizeof f);
    f.enabled    = j_bool(c, "enabled", false);
    f.iterations = (int)j_num(c, "iterations", 10.0);
    f.blend      = (float)j_num(c, "blend", 1.0);
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(c, "effectors");
    if (cJSON_IsArray(arr)) {
        int n = cJSON_GetArraySize(arr);
        int cap = (int)(sizeof(f.effectors) / sizeof(f.effectors[0]));
        for (int i = 0; i < n && f.effector_count < cap; i++) {
            const cJSON *o = cJSON_GetArrayItem(arr, i);
            if (!cJSON_IsObject(o)) continue;
            JceFullBodyIkEffector *k = &f.effectors[f.effector_count++];
            copy_str(k->bone, sizeof(k->bone), j_str(o, "bone", ""));
            k->target.x = (float)j_num(o, "targetX", 0.0);
            k->target.y = (float)j_num(o, "targetY", 0.0);
            k->target.z = (float)j_num(o, "targetZ", 0.0);
            k->weight   = (float)j_num(o, "weight", 1.0);
        }
    }
    jce_scene_set_full_body_ik(s, e, &f);
}

void parse_avatar(JceScene *s, JceEntity e, const cJSON *props)
{
    JceAvatarComponent cc; memset(&cc, 0, sizeof cc);
    copy_str(cc.avatar_path,         sizeof cc.avatar_path,         j_str(props, "avatarPath",         ""));
    copy_str(cc.mask_path,           sizeof cc.mask_path,           j_str(props, "maskPath",           ""));
    copy_str(cc.override_controller, sizeof cc.override_controller, j_str(props, "overrideController", ""));
    cc.apply_root_motion = j_bool(props, "applyRootMotion", false);
    cc.human_rig         = j_bool(props, "humanRig",        true);
    {
        const cJSON *layers = cJSON_GetObjectItemCaseSensitive(props, "layers");
        if (cJSON_IsArray(layers)) {
            int ln = cJSON_GetArraySize(layers);
            if (ln > JCE_AVATAR_MAX_LAYERS) ln = JCE_AVATAR_MAX_LAYERS;
            cc.layer_count = ln;
            for (int i = 0; i < ln; i++) {
                const cJSON *L = cJSON_GetArrayItem(layers, i);
                if (!cJSON_IsObject(L)) continue;
                copy_str(cc.layers[i].clip,      sizeof cc.layers[i].clip,
                         j_str(L, "clip", ""));
                copy_str(cc.layers[i].mask_path, sizeof cc.layers[i].mask_path,
                         j_str(L, "maskPath", ""));
                cc.layers[i].weight = (float)j_num(L, "weight", 1.0);
                cc.layers[i].mode   = (int)j_num(L, "mode", 0);
                if (cc.layers[i].mode < 0 || cc.layers[i].mode > 1)
                    cc.layers[i].mode = 0;
            }
        }
    }
    jce_scene_set_avatar(s, e, &cc);
}

/* ser_animator is GONE with the component's retirement.  It wrote four keys
 * nothing reads back any more (the registry row's serialize is NULL), which
 * check_component_serializer_roundtrip correctly calls authoring that
 * disappears on reload.  The retired VfxGraph is not written either. */
static void ser_sequence_player(const JceSequencePlayerComponent *sp, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "SequencePlayer");
    cJSON_AddStringToObject(o, "seqPath", sp->seq_path);
    cJSON_AddBoolToObject  (o, "playOnAwake",  sp->play_on_awake);
    cJSON_AddBoolToObject  (o, "loopOverride", sp->loop_override);
    cJSON_AddBoolToObject  (o, "overrideLoop", sp->override_loop);
    cJSON_AddNumberToObject(o, "speed", sp->speed);
    cJSON *list = cJSON_AddArrayToObject(o, "bindings");
    int n = sp->binding_count;
    if (n < 0) n = 0;
    if (n > JCE_SEQ_PLAYER_MAX_BINDINGS) n = JCE_SEQ_PLAYER_MAX_BINDINGS;
    for (int i = 0; i < n; i++)
        cJSON_AddItemToArray(list, cJSON_CreateNumber((double)sp->bindings[i]));
    /* Runtime fields (seq/prev_time/started/opened_hash) are NOT persisted. */
    cJSON_AddItemToArray(arr, o);
}

static void ser_morph_weights(const JceMorphWeightsComponent *mw, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    int n = mw->count;
    cJSON *list;
    int i;
    cJSON_AddStringToObject(o, "type", "MorphWeights");
    if (n < 0) n = 0;
    if (n > JCE_MORPH_MAX_WEIGHTS) n = JCE_MORPH_MAX_WEIGHTS;
    cJSON_AddNumberToObject(o, "count", n);
    cJSON_AddNumberToObject(o, "overrideMask", (double)mw->override_mask);
    list = cJSON_AddArrayToObject(o, "weights");
    for (i = 0; i < n; i++)
        cJSON_AddItemToArray(list, cJSON_CreateNumber((double)mw->weights[i]));
    cJSON_AddItemToArray(arr, o);
}

static void ser_ik_constraints(const JceIkConstraintComponent *ik, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "IkConstraints");
    cJSON *list = cJSON_AddArrayToObject(o, "constraints");
    int cap = (int)(sizeof(ik->constraints) / sizeof(ik->constraints[0]));
    int n = ik->count;
    if (n < 0)   n = 0;
    if (n > cap) n = cap;
    for (int i = 0; i < n; i++) {
        const JceIkConstraint *k = &ik->constraints[i];
        cJSON *co = cJSON_CreateObject();
        cJSON_AddNumberToObject(co, "kind",   k->kind);
        cJSON_AddStringToObject(co, "name",   k->name);
        cJSON_AddNumberToObject(co, "weight", k->weight);
        cJSON_AddBoolToObject  (co, "enabled", k->enabled);
        cJSON_AddStringToObject(co, "rootBone", k->root_bone);
        cJSON_AddStringToObject(co, "midBone",  k->mid_bone);
        cJSON_AddStringToObject(co, "endBone",  k->end_bone);
        cJSON_AddNumberToObject(co, "targetEntity", (double)k->target_entity);
        cJSON_AddNumberToObject(co, "poleEntity",   (double)k->pole_entity);
        cJSON_AddNumberToObject(co, "poleOffsetX",  k->pole_offset[0]);
        cJSON_AddNumberToObject(co, "poleOffsetY",  k->pole_offset[1]);
        cJSON_AddNumberToObject(co, "poleOffsetZ",  k->pole_offset[2]);
        cJSON_AddItemToArray(list, co);
    }
    cJSON_AddItemToArray(arr, o);
}

static void ser_foot_ik(const JceFootIkComponent *f, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "FootIk");
    cJSON_AddBoolToObject  (o, "enabled", f->enabled);
    cJSON_AddStringToObject(o, "pelvisBone", f->pelvis_bone);
    cJSON_AddStringToObject(o, "hipBoneL",   f->hip_bone[0]);
    cJSON_AddStringToObject(o, "hipBoneR",   f->hip_bone[1]);
    cJSON_AddStringToObject(o, "kneeBoneL",  f->knee_bone[0]);
    cJSON_AddStringToObject(o, "kneeBoneR",  f->knee_bone[1]);
    cJSON_AddStringToObject(o, "ankleBoneL", f->ankle_bone[0]);
    cJSON_AddStringToObject(o, "ankleBoneR", f->ankle_bone[1]);
    cJSON_AddNumberToObject(o, "maxStepHeight",  f->max_step_height);
    cJSON_AddNumberToObject(o, "footOffset",     f->foot_offset);
    cJSON_AddNumberToObject(o, "castUp",         f->cast_up);
    cJSON_AddNumberToObject(o, "castDown",       f->cast_down);
    cJSON_AddBoolToObject  (o, "rotateToNormal", f->rotate_to_normal);
    cJSON_AddNumberToObject(o, "blend",          f->blend);
    cJSON_AddItemToArray(arr, o);
}

static void ser_full_body_ik(const JceFullBodyIkComponent *f, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "FullBodyIk");
    cJSON_AddBoolToObject  (o, "enabled",    f->enabled);
    cJSON_AddNumberToObject(o, "iterations", f->iterations);
    cJSON_AddNumberToObject(o, "blend",      f->blend);
    cJSON *list = cJSON_AddArrayToObject(o, "effectors");
    int cap = (int)(sizeof(f->effectors) / sizeof(f->effectors[0]));
    int n = f->effector_count;
    if (n < 0)   n = 0;
    if (n > cap) n = cap;
    for (int i = 0; i < n; i++) {
        const JceFullBodyIkEffector *k = &f->effectors[i];
        cJSON *eo = cJSON_CreateObject();
        cJSON_AddStringToObject(eo, "bone",    k->bone);
        cJSON_AddNumberToObject(eo, "targetX", k->target.x);
        cJSON_AddNumberToObject(eo, "targetY", k->target.y);
        cJSON_AddNumberToObject(eo, "targetZ", k->target.z);
        cJSON_AddNumberToObject(eo, "weight",  k->weight);
        cJSON_AddItemToArray(list, eo);
    }
    cJSON_AddItemToArray(arr, o);
}

static void ser_sprite_animator(const JceSpriteAnimatorComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "SpriteAnimator");
    cJSON_AddStringToObject(o, "sheetPath", c->sheet_path);
    cJSON_AddStringToObject(o, "atlasPath", c->atlas_path);
    cJSON_AddNumberToObject(o, "frameWidth", c->frame_width);
    cJSON_AddNumberToObject(o, "frameHeight", c->frame_height);
    cJSON_AddStringToObject(o, "currentAnim", c->current_anim);
    cJSON_AddNumberToObject(o, "speed", c->speed);
    cJSON_AddBoolToObject(o, "loop", c->loop);
    cJSON_AddBoolToObject(o, "playing", c->playing);
    cJSON_AddItemToArray(arr, o);
}

static void ser_skeletal_animator(const JceSkeletalAnimatorComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "SkeletalAnimator");
    cJSON_AddStringToObject(o, "skeletonPath", c->skeleton_path);
    /* Only emit the retarget source when set, so legacy (non-retargeting)
       animators serialize byte-identically to before. */
    if (c->retarget_source_skeleton[0])
        cJSON_AddStringToObject(o, "retargetSource", c->retarget_source_skeleton);
    if (c->sm_path[0])
        cJSON_AddStringToObject(o, "stateMachine", c->sm_path);
    /* Emitted only when ON, so every animator authored before this existed
       serializes byte-identically -- the same rule retargetSource follows two
       lines above. */
    if (c->retarget_effector_ik)
        cJSON_AddBoolToObject(o, "retargetEffectorIk", true);
    if (c->retarget_twist > 0.0f)
        cJSON_AddNumberToObject(o, "retargetTwist", c->retarget_twist);
    if (c->use_blend_tree) {
        cJSON_AddBoolToObject(o, "useBlendTree", true);
        cJSON_AddNumberToObject(o, "blendParam", c->blend_param);
        cJSON *bts = cJSON_CreateArray();
        for (int i = 0; i < 8; i++)
            cJSON_AddItemToArray(bts, cJSON_CreateNumber(c->blend_thresholds[i]));
        cJSON_AddItemToObject(o, "blendThresholds", bts);
        if (c->blend_mode != 0) {
            cJSON_AddNumberToObject(o, "blendMode", c->blend_mode);
            cJSON_AddNumberToObject(o, "blendParamY", c->blend_param_y);
            cJSON *bys = cJSON_CreateArray();
            for (int i = 0; i < 8; i++)
                cJSON_AddItemToArray(bys, cJSON_CreateNumber(c->blend_pos_y[i]));
            cJSON_AddItemToObject(o, "blendPosY", bys);
        }
    }
    if (c->auto_speed) cJSON_AddBoolToObject(o, "autoSpeed", true);
    cJSON_AddNumberToObject(o, "speed", c->speed);
    cJSON_AddBoolToObject(o, "loop", c->loop);
    cJSON_AddBoolToObject(o, "playing", c->playing);
    cJSON_AddNumberToObject(o, "activeClip", c->active_clip);
    /* Defensive clamp: clip_count must be in [0, ARRAY_LEN]. Garbage
     * here would walk into adjacent memory and crash cJSON_strdup. */
    int cc = c->clip_count;
    if (cc < 0) cc = 0;
    if (cc > (int)(sizeof(c->clip_names) / sizeof(c->clip_names[0])))
        cc = (int)(sizeof(c->clip_names) / sizeof(c->clip_names[0]));
    if (cc > 0) {
        cJSON *clips = cJSON_CreateArray();
        for (int i = 0; i < cc; i++)
            cJSON_AddItemToArray(clips, cJSON_CreateString(c->clip_names[i]));
        cJSON_AddItemToObject(o, "clipNames", clips);
    }
    cJSON_AddItemToArray(arr, o);
}

void serw_sprite_animator(JceScene *s, JceEntity e, cJSON *arr)
{
    JceSpriteAnimatorComponent *c = jce_scene_get_sprite_animator(s, e);
    if (c) ser_sprite_animator(c, arr);
}

/* RETIRED: the simple Animator was a strict SUBSET of SkeletalAnimator
 * (clip_name/speed/loop/playing against clip_names[8] + active_clip + blend
 * trees + state machines + retargeting) and had NO runtime consumer at all --
 * only the serialiser.  The editor said so with a component-wide unwired
 * badge, so it was honest and inert: a component in the Add Component menu
 * that does nothing, which is a trap for exactly the production use this is
 * meant to support.
 *
 * It could not be wired as it stood, either: it carries no skeleton binding,
 * and the runtime needs one (jce_runtime.c loads sa->skeleton_path as a
 * glTF).  MeshRenderer.mesh_path IS that glTF -- the same file supplies the
 * mesh, the skeleton and the clips -- so migrating here does not merely
 * preserve the authored values, it makes them PLAY for the first time.
 *
 * Same shape as the VfxGraph retirement above: parse migrates, serialize is
 * NULL so nothing is ever re-saved as "Animator", and an explicitly authored
 * SkeletalAnimator on the same entity always wins. */
void parse_animator_migrate(JceScene *s, JceEntity e, const cJSON *props)
{
    if (jce_scene_has_skeletal_animator(s, e)) return;

    const char *clip = j_str(props, "clipName", "");

    JceSkeletalAnimatorComponent sa;
    memset(&sa, 0, sizeof sa);
    sa.speed   = (float)j_num(props, "speed", 1.0);
    /* false, matching what parse_animator always read -- the editor
     * writes the key explicitly, so this only affects hand-written JSON. */
    sa.loop    = j_bool(props, "loop", false);
    sa.playing = j_bool(props, "playing", false);
    if (sa.speed <= 0.0f) sa.speed = 1.0f;

    if (clip[0]) {
        copy_str(sa.clip_names[0], sizeof sa.clip_names[0], clip);
        sa.clip_count  = 1;
        sa.active_clip = 0;
    }

    /* The rig comes from the mesh on the same entity.  No MeshRenderer means
     * no skeleton to play against -- the component is still migrated so the
     * authored values survive and become visible in the SkeletalAnimator
     * inspector, where the skeleton field can be filled in. */
    JceMeshRenderer *mr = jce_scene_get_mesh_renderer(s, e);
    if (mr && mr->mesh_path[0])
        copy_str(sa.skeleton_path, sizeof sa.skeleton_path, mr->mesh_path);

    jce_scene_set_skeletal_animator(s, e, &sa);
}

/* One-way legacy migration (consolidation v0.9.9). The orphaned VFX
 * Graph runtime was removed — its `*.vfx.json` key set always parsed
 * identically to `*.particles.json`, so the authored graphPath maps
 * straight onto a ParticleEmitterComponent asset path and the entity
 * joins the standard particle pipeline. Legacy-only knobs
 * (playOnAwake / loop / rateMultiplier / intensity) have no
 * counterpart and are dropped. An explicitly authored ParticleEmitter
 * on the same entity always wins; the component is never re-saved as
 * VfxGraph (its registry row has serialize == NULL). */

void serw_skeletal_animator(JceScene *s, JceEntity e, cJSON *arr)
{
    JceSkeletalAnimatorComponent *c = jce_scene_get_skeletal_animator(s, e);
    if (c) ser_skeletal_animator(c, arr);
}

void serw_ik_constraints(JceScene *s, JceEntity e, cJSON *arr)
{
    JceIkConstraintComponent *c = jce_scene_get_ik_constraints(s, e);
    if (c) ser_ik_constraints(c, arr);
}

void serw_foot_ik(JceScene *s, JceEntity e, cJSON *arr)
{
    JceFootIkComponent *c = jce_scene_get_foot_ik(s, e);
    if (c) ser_foot_ik(c, arr);
}

void serw_full_body_ik(JceScene *s, JceEntity e, cJSON *arr)
{
    JceFullBodyIkComponent *c = jce_scene_get_full_body_ik(s, e);
    if (c) ser_full_body_ik(c, arr);
}

void serw_sequence_player(JceScene *s, JceEntity e, cJSON *arr)
{
    JceSequencePlayerComponent *c = jce_scene_get_sequence_player(s, e);
    if (c) ser_sequence_player(c, arr);
}

void serw_morph_weights(JceScene *s, JceEntity e, cJSON *arr)
{
    JceMorphWeightsComponent *c = jce_scene_get_morph_weights(s, e);
    if (c) ser_morph_weights(c, arr);
}

void serw_avatar(JceScene *s, JceEntity e, cJSON *arr)
{
    JceAvatarComponent *c = jce_scene_get_avatar(s, e);
    if (c) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "type", "Avatar");
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "avatarPath",         c->avatar_path);
        cJSON_AddStringToObject(p, "maskPath",           c->mask_path);
        cJSON_AddStringToObject(p, "overrideController", c->override_controller);
        cJSON_AddBoolToObject  (p, "applyRootMotion",    c->apply_root_motion);
        cJSON_AddBoolToObject  (p, "humanRig",           c->human_rig);
        int lc = c->layer_count;
        if (lc < 0) lc = 0;
        if (lc > JCE_AVATAR_MAX_LAYERS) lc = JCE_AVATAR_MAX_LAYERS;
        if (lc > 0) {
            cJSON *layers = cJSON_CreateArray();
            for (int i = 0; i < lc; i++) {
                cJSON *L = cJSON_CreateObject();
                cJSON_AddStringToObject(L, "clip",     c->layers[i].clip);
                cJSON_AddStringToObject(L, "maskPath", c->layers[i].mask_path);
                cJSON_AddNumberToObject(L, "weight",   c->layers[i].weight);
                cJSON_AddNumberToObject(L, "mode",     c->layers[i].mode);
                cJSON_AddItemToArray(layers, L);
            }
            cJSON_AddItemToObject(p, "layers", layers);
        }
        cJSON_AddItemToObject(o, "properties", p);
        cJSON_AddItemToArray(arr, o);
    }
}

void parse_bone_attachment(JceScene *s, JceEntity e, const cJSON *c)
{
    JceBoneAttachmentComponent a; memset(&a, 0, sizeof a);
    a.target = (uint64_t)j_num(c, "target", 0.0);
    const char *bone = j_str(c, "bone", "");
    snprintf(a.bone, sizeof a.bone, "%s", bone ? bone : "");
    a.offset[0] = (float)j_num(c, "offsetX", 0.0);
    a.offset[1] = (float)j_num(c, "offsetY", 0.0);
    a.offset[2] = (float)j_num(c, "offsetZ", 0.0);
    /* All-zero is IDENTITY, not a degenerate quaternion -- see the struct.
     * A scene file without these keys must leave the attachment pointing the
     * way the bone does, and memset already produces that reading. */
    a.rotation_offset[0] = (float)j_num(c, "rotX", 0.0);
    a.rotation_offset[1] = (float)j_num(c, "rotY", 0.0);
    a.rotation_offset[2] = (float)j_num(c, "rotZ", 0.0);
    a.rotation_offset[3] = (float)j_num(c, "rotW", 0.0);
    jce_scene_set_bone_attachment(s, e, &a);
}

void serw_bone_attachment(JceScene *s, JceEntity e, cJSON *arr)
{
    JceBoneAttachmentComponent *a = jce_scene_get_bone_attachment(s, e);
    if (!a) return;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "BoneAttachment");
    cJSON_AddNumberToObject(o, "target", (double)a->target);
    cJSON_AddStringToObject(o, "bone", a->bone);
    cJSON_AddNumberToObject(o, "offsetX", a->offset[0]);
    cJSON_AddNumberToObject(o, "offsetY", a->offset[1]);
    cJSON_AddNumberToObject(o, "offsetZ", a->offset[2]);
    cJSON_AddNumberToObject(o, "rotX", a->rotation_offset[0]);
    cJSON_AddNumberToObject(o, "rotY", a->rotation_offset[1]);
    cJSON_AddNumberToObject(o, "rotZ", a->rotation_offset[2]);
    cJSON_AddNumberToObject(o, "rotW", a->rotation_offset[3]);
    cJSON_AddItemToArray(arr, o);
}
