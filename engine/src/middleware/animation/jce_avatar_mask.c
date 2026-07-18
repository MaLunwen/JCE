/*
 * jce_avatar_mask.c  Real per-bone blend weight mask.
 *
 * Stores a dense float array of per-bone weights.  Unset bones (beyond the
 * stored range) read back as the mask's `def` weight.  Used by the additive /
 * layered blend path to scale how much of a layer's delta each bone takes.
 * See jce_avatar_mask.h for the .mask asset format.
 */

#include <jce/middleware/animation/jce_avatar_mask.h>
#include <jce/middleware/animation/jce_skeleton.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>
#include "os/core/jce_memory.h"

#include <stddef.h>
#include <string.h>   /* strlen — mask_load_impl_mem len==0 path */

#define LOG_TAG "jce_avatar_mask"

struct JceAvatarMask {
    float   *weights;   /* [count], or NULL when count == 0 */
    uint32_t count;
    float    def;       /* fill value for unset / grown bones */
};

static float clamp01(float w)
{
    if (w < 0.0f) return 0.0f;
    if (w > 1.0f) return 1.0f;
    return w;
}

/* Ensure storage covers index `n` (count >= n), filling new slots with `def`. */
static bool mask_grow(JceAvatarMask *m, uint32_t n)
{
    if (n <= m->count) return true;
    float *nw = (float *)JCE_REALLOC(m->weights, n * sizeof(float));
    if (!nw) return false;
    for (uint32_t i = m->count; i < n; i++) nw[i] = m->def;
    m->weights = nw;
    m->count   = n;
    return true;
}

JceAvatarMask *JCE_CALL jce_avatar_mask_create(uint32_t bone_count,
                                               float    default_weight)
{
    JceAvatarMask *m = JCE_NEW(JceAvatarMask);
    if (!m) return NULL;
    m->def = clamp01(default_weight);
    if (bone_count > 0) {
        m->weights = (float *)JCE_MALLOC(bone_count * sizeof(float));
        if (!m->weights) { JCE_FREE(m); return NULL; }
        for (uint32_t i = 0; i < bone_count; i++) m->weights[i] = m->def;
        m->count = bone_count;
    }
    return m;
}

void JCE_CALL jce_avatar_mask_unload(JceAvatarMask *m)
{
    if (!m) return;
    JCE_FREE(m->weights);
    JCE_FREE(m);
}

uint32_t JCE_CALL jce_avatar_mask_count(const JceAvatarMask *m)
{
    return m ? m->count : 0u;
}

float JCE_CALL jce_avatar_mask_weight(const JceAvatarMask *m, uint32_t bone_index)
{
    if (!m) return 1.0f;
    if (bone_index >= m->count) return m->def;
    return m->weights[bone_index];
}

void JCE_CALL jce_avatar_mask_set_weight(JceAvatarMask *m, uint32_t bone_index,
                                         float w)
{
    if (!m) return;
    if (!mask_grow(m, bone_index + 1)) return;
    m->weights[bone_index] = clamp01(w);
}

/* Build a mask from an already-parsed JSON root (does NOT free `root`).
 * `skel` may be NULL (name entries are then skipped). */
static JceAvatarMask *mask_from_root(JceJson *root, const JceSkeleton *skel)
{
    if (!root) return NULL;

    uint32_t skel_joints = skel ? jce_skeleton_joint_count(skel) : 0u;
    float    def = (float)jce_json_get_number(root, "default", 1.0);

    JceAvatarMask *m = jce_avatar_mask_create(skel_joints, def);
    if (!m) { jce_json_free(root); return NULL; }

    JceJson *arr = jce_json_get(root, "weights");
    if (jce_json_is_array(arr)) {
        for (JceJson *it = jce_json_first_child(arr); it;
             it = jce_json_next_sibling(it)) {
            float    w  = (float)jce_json_get_number(it, "weight", 1.0);
            int      bi = -1;

            if (jce_json_has(it, "index")) {
                bi = jce_json_get_int(it, "index", -1);
            } else if (skel) {
                const char *bn = jce_json_get_string(it, "bone", NULL);
                if (bn && bn[0])
                    bi = jce_skeleton_find_joint(skel, bn);
            }
            /* Name entries with no skeleton (bi stays -1) are silently
             * skipped — load() is the index-only entry point by design. */
            if (bi >= 0)
                jce_avatar_mask_set_weight(m, (uint32_t)bi, w);
        }
    }

    LOG_DEBUG(LOG_TAG, "loaded mask: %u bones (default %.3f)",
              m->count, (double)m->def);
    return m;
}

/* Host-file entry (editor / loose cooked tree). */
static JceAvatarMask *mask_load_impl(const char *path, const JceSkeleton *skel)
{
    if (!path || !path[0]) return NULL;
    JceJson *root = jce_json_parse_file(path);
    if (!root) return NULL;
    JceAvatarMask *m = mask_from_root(root, skel);
    jce_json_free(root);
    return m;
}

/* In-memory entry (single-exe: bytes decompressed from the embedded PAK). */
static JceAvatarMask *mask_load_impl_mem(const char *text, size_t len,
                                         const JceSkeleton *skel)
{
    if (!text) return NULL;
    if (len == 0) len = strlen(text);
    JceJson *root = jce_json_parse(text, len);
    if (!root) return NULL;
    JceAvatarMask *m = mask_from_root(root, skel);
    jce_json_free(root);
    return m;
}

JceAvatarMask *jce_avatar_mask_load(const char *path)
{
    return mask_load_impl(path, NULL);
}

JceAvatarMask *JCE_CALL jce_avatar_mask_load_for_skeleton(const char *path,
                                                          const JceSkeleton *skel)
{
    return mask_load_impl(path, skel);
}

JceAvatarMask *JCE_CALL jce_avatar_mask_load_for_skeleton_mem(
    const char *text, size_t len, const JceSkeleton *skel)
{
    return mask_load_impl_mem(text, len, skel);
}
