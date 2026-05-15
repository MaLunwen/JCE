/*
 * jce_xr.c  XR session state + action table.
 */

#include <jce/middleware/xr/jce_xr.h>

#include <string.h>

void jce_xr_session_init(JceXrSession *s)
{
    if (!s) return;
    memset(s, 0, sizeof(*s));
    s->state = JCE_XR_STATE_INACTIVE;
}

void jce_xr_set_head_pose(JceXrSession *s, const JceXrPose *p)
{
    if (!s || !p) return;
    s->head_pose = *p;
}

void jce_xr_set_eye_pose(JceXrSession *s, JceXrEye eye, const JceXrPose *p)
{
    if (!s || !p || eye >= JCE_XR_EYE_COUNT) return;
    s->eye_pose[eye] = *p;
}

void jce_xr_push_controller_pose(JceXrSession *s, bool right,
                                  const JceXrPose *p)
{
    if (!s || !p) return;
    if (right) {
        s->controller_right[s->right_head % JCE_XR_POSE_RING_SIZE] = *p;
        s->right_head++;
    } else {
        s->controller_left[s->left_head % JCE_XR_POSE_RING_SIZE] = *p;
        s->left_head++;
    }
}

const JceXrPose *jce_xr_latest_controller_pose(const JceXrSession *s, bool right)
{
    if (!s) return NULL;
    uint32_t head = right ? s->right_head : s->left_head;
    if (head == 0) return NULL;
    const JceXrPose *ring = right ? s->controller_right : s->controller_left;
    return &ring[(head - 1) % JCE_XR_POSE_RING_SIZE];
}

static int find_action(const JceXrSession *s, const char *name)
{
    for (int i = 0; i < JCE_XR_ACTION_TABLE_SIZE; ++i)
        if (s->actions[i].active &&
            strncmp(s->actions[i].name, name, sizeof(s->actions[i].name)) == 0)
            return i;
    return -1;
}

JceXrAction *jce_xr_register_action(JceXrSession *s, const char *name,
                                      JceXrActionKind kind)
{
    if (!s || !name) return NULL;
    int existing = find_action(s, name);
    int slot = existing >= 0 ? existing : -1;
    if (slot < 0) {
        for (int i = 0; i < JCE_XR_ACTION_TABLE_SIZE; ++i) {
            if (!s->actions[i].active) { slot = i; break; }
        }
    }
    if (slot < 0) return NULL;
    JceXrAction *a = &s->actions[slot];
    if (!a->active) {
        memset(a, 0, sizeof(*a));
        strncpy(a->name, name, sizeof(a->name) - 1);
    }
    a->kind = kind;
    a->active = true;
    return a;
}

JceXrAction *jce_xr_find_action(JceXrSession *s, const char *name)
{
    if (!s || !name) return NULL;
    int i = find_action(s, name);
    return i >= 0 ? &s->actions[i] : NULL;
}

uint32_t jce_xr_action_count(const JceXrSession *s)
{
    if (!s) return 0;
    uint32_t n = 0;
    for (int i = 0; i < JCE_XR_ACTION_TABLE_SIZE; ++i)
        if (s->actions[i].active) n++;
    return n;
}
