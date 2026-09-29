/*
 * jce_avatar.c  A skeleton's humanoid rig mapping, as an asset.
 * See jce_avatar.h.
 */

#include <jce/middleware/animation/jce_avatar.h>

#include <jce/middleware/animation/jce_skeleton.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>

#include "os/core/jce_memory.h"

#include <string.h>

#define LOG_TAG "avatar"

#define JCE_AVATAR_JOINT_NAME_MAX 64

struct JceAvatarAsset {
    int            ref;
    JceHumanoidMap map;
    /* The joint NAME per role -- what the file stores and what bind() looks
     * up.  An index means nothing outside the skeleton it came from. */
    char           joint_name[JCE_HB_COUNT][JCE_AVATAR_JOINT_NAME_MAX];
};

static JceAvatarAsset *avatar_alloc(void)
{
    JceAvatarAsset *a = (JceAvatarAsset *)JCE_CALLOC(1, sizeof(*a));
    if (!a) return NULL;
    a->ref = 1;
    for (int b = 0; b < JCE_HB_COUNT; b++) {
        a->map.joint[b] = -1;
        a->map.rest_model[b] = jce_q_identity();
    }
    return a;
}

JceAvatarAsset *jce_avatar_build(const JceSkeleton *skel)
{
    if (!skel) return NULL;
    JceAvatarAsset *a = avatar_alloc();
    if (!a) return NULL;
    if (!jce_humanoid_map_build(skel, &a->map)) {
        JCE_FREE(a);
        return NULL;
    }
    for (int b = 0; b < JCE_HB_COUNT; b++) {
        if (a->map.joint[b] < 0) continue;
        const char *nm = jce_skeleton_joint_name(skel, (uint32_t)a->map.joint[b]);
        if (nm) {
            strncpy(a->joint_name[b], nm, JCE_AVATAR_JOINT_NAME_MAX - 1);
            a->joint_name[b][JCE_AVATAR_JOINT_NAME_MAX - 1] = '\0';
        }
    }
    LOG_INFO(LOG_TAG, "built: %u of %d humanoid role(s) mapped",
             a->map.mapped_count, (int)JCE_HB_COUNT);
    return a;
}

JceAvatarAsset *jce_avatar_load(const char *path)
{
    /* NULL on a missing or unparseable file, NOT an empty asset.  This
     * function used to JCE_CALLOC one and hand it back, which made every
     * caller's `if (!asset)` pass and turned "there is no such file" into
     * something indistinguishable from success -- the rigging panel printed
     * "<path> : 0" as though it had read a bone count out of the user's file. */
    if (!path || !path[0]) return NULL;
    JceJson *root = jce_json_parse_file(path);
    if (!root) {
        LOG_WARN(LOG_TAG, "cannot read '%s': %s", path, jce_json_last_error());
        return NULL;
    }
    JceJson *bones = jce_json_get(root, "bones");
    if (!bones || !jce_json_is_object(bones)) {
        LOG_WARN(LOG_TAG, "'%s' has no \"bones\" object", path);
        jce_json_free(root);
        return NULL;
    }

    JceAvatarAsset *a = avatar_alloc();
    if (!a) { jce_json_free(root); return NULL; }

    for (int b = 0; b < JCE_HB_COUNT; b++) {
        JceJson *v = jce_json_get(bones, jce_humanoid_bone_name((JceHumanoidBone)b));
        const char *nm = v ? jce_json_string_value(v, NULL) : NULL;
        if (!nm || !nm[0]) continue;
        strncpy(a->joint_name[b], nm, JCE_AVATAR_JOINT_NAME_MAX - 1);
        a->joint_name[b][JCE_AVATAR_JOINT_NAME_MAX - 1] = '\0';
    }
    jce_json_free(root);
    /* The indices stay -1 until jce_avatar_bind names a skeleton: a file has
     * joint names, and only a skeleton can turn those into indices. */
    return a;
}

bool jce_avatar_save(const JceAvatarAsset *a, const char *path)
{
    if (!a || !path || !path[0]) return false;
    JceJson *root = jce_json_object();
    JceJson *bones = jce_json_object();
    if (!root || !bones) {
        jce_json_free(root);
        jce_json_free(bones);
        return false;
    }
    for (int b = 0; b < JCE_HB_COUNT; b++) {
        if (!a->joint_name[b][0]) continue;
        jce_json_set_string(bones, jce_humanoid_bone_name((JceHumanoidBone)b),
                            a->joint_name[b]);
    }
    jce_json_set_child(root, "bones", bones);
    /* take_ownership: the tree is freed either way, which is what
     * this function wants -- there is nothing to inspect afterwards. */
    const bool ok = jce_json_write_file(path, root, true, true);
    if (!ok) LOG_ERROR(LOG_TAG, "cannot write '%s'", path);
    return ok;
}

uint32_t jce_avatar_bind(JceAvatarAsset *a, const JceSkeleton *skel)
{
    if (!a || !skel) return 0;

    /* The rest pose comes from a FRESH auto-map of this skeleton, and then the
     * stored names override which joint plays each role.  Doing it this way
     * rather than recomputing rest rotations by hand keeps one implementation
     * of "what is this joint's rest orientation in model space". */
    JceHumanoidMap fresh;
    if (!jce_humanoid_map_build(skel, &fresh)) return 0;

    for (int b = 0; b < JCE_HB_COUNT; b++) {
        a->map.joint[b] = -1;
        a->map.rest_model[b] = jce_q_identity();
    }
    a->map.mapped_count = 0;
    a->map.hips_height = fresh.hips_height;

    const uint32_t n = jce_skeleton_joint_count(skel);
    for (int b = 0; b < JCE_HB_COUNT; b++) {
        if (!a->joint_name[b][0]) continue;
        const int idx = jce_skeleton_find_joint(skel, a->joint_name[b]);
        if (idx < 0 || (uint32_t)idx >= n) {
            /* A bone whose joint is gone comes back UNMAPPED rather than
             * pointing at whatever now occupies that slot. */
            LOG_WARN(LOG_TAG, "%s: no joint named '%s' in this skeleton",
                     jce_humanoid_bone_name((JceHumanoidBone)b),
                     a->joint_name[b]);
            continue;
        }
        a->map.joint[b] = idx;
        /* The rest rotation of THAT joint: take it from the fresh map when the
         * auto-mapper agreed, otherwise recompute is not needed -- the fresh
         * map holds rest rotations only for roles it matched, so a hand-bound
         * joint the matcher missed keeps identity and is reported. */
        if (fresh.joint[b] == idx)
            a->map.rest_model[b] = fresh.rest_model[b];
        a->map.mapped_count++;
    }
    /* Hips height belongs to the joint this avatar actually calls the hips. */
    if (a->map.joint[JCE_HB_HIPS] >= 0 &&
        fresh.joint[JCE_HB_HIPS] != a->map.joint[JCE_HB_HIPS]) {
        JceHumanoidMap only;
        if (jce_humanoid_map_build(skel, &only))
            a->map.hips_height = only.hips_height;
    }
    return a->map.mapped_count;
}

const JceHumanoidMap *jce_avatar_map(const JceAvatarAsset *a)
{
    return a ? &a->map : NULL;
}

uint32_t jce_avatar_mapped_count(const JceAvatarAsset *a)
{
    return a ? a->map.mapped_count : 0;
}

const char *jce_avatar_joint_name(const JceAvatarAsset *a, JceHumanoidBone bone)
{
    if (!a || (unsigned)bone >= (unsigned)JCE_HB_COUNT) return NULL;
    return a->joint_name[bone][0] ? a->joint_name[bone] : NULL;
}

void jce_avatar_unload(JceAvatarAsset *a)
{
    if (!a) return;
    if (--a->ref <= 0) JCE_FREE(a);
}

/* ── Legacy shape ─────────────────────────────────────────────────── */

uint32_t jce_avatar_bone_count(const JceAvatarAsset *a)
{
    if (!a) return 0;
    uint32_t n = 0;
    for (int b = 0; b < JCE_HB_COUNT; b++)
        if (a->joint_name[b][0]) n++;
    return n;
}

const char *jce_avatar_bone_name(const JceAvatarAsset *a, uint32_t i)
{
    if (!a) return NULL;
    uint32_t n = 0;
    for (int b = 0; b < JCE_HB_COUNT; b++) {
        if (!a->joint_name[b][0]) continue;
        if (n == i) return jce_humanoid_bone_name((JceHumanoidBone)b);
        n++;
    }
    return NULL;
}
