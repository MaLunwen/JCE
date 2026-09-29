/* jce_script_py_mock.gen.c -- GENERATED. DO NOT EDIT.
 *
 *   python tools/scriptgen/gen_script_bindings.py --write
 *
 * ONE recording mock host, loaded by BOTH sides of the cross-language
 * differential:
 *
 *   the Lua side   tests/scripting/python/lua_case_runner.c installs
 *                  jce_mock_host() into jce_script_create_sized and runs a
 *                  generated Lua chunk;
 *   the Python side tests/scripting/python/differential.py passes the same
 *                  table to jce_script_api_open through ctypes.
 *
 * Sharing the object is the point.  Two mocks written to agree would be a
 * second two-sided contract, and this repository's standing failure is exactly
 * that: two sides that agree with each other and are both wrong.
 *
 * EVERY call appends one line to a single ordered buffer that jce.log also
 * writes to, so the recorded stream interleaves what the script asked for with
 * what the host was asked to do.  Comparing those two streams IS the
 * differential: a missing call, an extra call, a permuted argument list, a
 * wrong capacity, a free that did not happen and a free that happened in the
 * wrong order are all a text difference.
 *
 * Canned answers are a pure function of the MEMBER NAME (an FNV-1a hash) and
 * the mode, never of call order, so a test that runs one case in isolation
 * sees the same values as a test that runs all of them.
 */

#include <jce/middleware/script/jce_script.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define JCE_MOCK_EXPORT __declspec(dllexport)
#else
#  define JCE_MOCK_EXPORT __attribute__((visibility("default")))
#endif

/* Modes.  Kept in sync with differential.py's MODES by that file's
 * check_mock_modes, which reads this list and that dict. */
#define JCE_MOCK_MODE_OK        0   /* every member present and answering */
#define JCE_MOCK_MODE_MISS      1   /* present, but answering "no" */
#define JCE_MOCK_MODE_ABSENT    2   /* every member NULL but log */
#define JCE_MOCK_MODE_NORELEASE 3   /* OK, except json_free is NULL */

#define JCE_MOCK_TRACE_CAP (1u << 20)

static char   g_trace[JCE_MOCK_TRACE_CAP];
static size_t g_trace_len;
static int    g_mode;
static int    g_live_strings;
static int    g_alloc_seq;

/* Allocation identity, so the trace can say WHICH string was released rather
 * than that some pointer was.  A raw %p would differ between the two runs and
 * make every trace comparison fail for a reason that is not a defect. */
#define JCE_MOCK_MAX_ALLOCS 256
static char *g_allocs[JCE_MOCK_MAX_ALLOCS];
static int   g_alloc_ids[JCE_MOCK_MAX_ALLOCS];
static int   g_alloc_count;

static void mock_emit(const char *text)
{
    size_t n = strlen(text);
    if (g_trace_len + n + 2u >= JCE_MOCK_TRACE_CAP)
        return;                      /* the count assertion catches truncation */
    memcpy(g_trace + g_trace_len, text, n);
    g_trace_len += n;
    g_trace[g_trace_len++] = '\n';
    g_trace[g_trace_len] = '\0';
}

static unsigned mock_hash(const char *s)
{
    unsigned h = 2166136261u;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h % 97u + 1u;
}

static const char *mock_str(const char *s)
{
    return s ? s : "<null>";
}

/* jce.log is the wire the Lua side writes CASE / RESULT lines through, and
 * jce_mock_note is the same wire for the Python side.  Both append verbatim,
 * so the two streams are directly comparable. */
static void mock_log(void *user, const char *msg)
{
    (void)user;
    mock_emit(mock_str(msg));
}

JCE_MOCK_EXPORT void jce_mock_note(const char *text)
{
    mock_emit(mock_str(text));
}

JCE_MOCK_EXPORT const char *jce_mock_trace_text(void)
{
    return g_trace;
}

JCE_MOCK_EXPORT void jce_mock_reset(int mode)
{
    g_trace_len = 0u;
    g_trace[0] = '\0';
    g_mode = mode;
    g_live_strings = 0;
    g_alloc_seq = 0;
    g_alloc_count = 0;
    memset(g_allocs, 0, sizeof g_allocs);
}

/* Owned strings still held by nobody.  The differential asserts this is 0
 * after every owned_string_release case on BOTH sides -- which is how the
 * release is SEEN rather than assumed. */
JCE_MOCK_EXPORT int jce_mock_live_strings(void)
{
    return g_live_strings;
}

JCE_MOCK_EXPORT size_t jce_mock_host_size(void)
{
    return sizeof(JceScriptHost);
}

JCE_MOCK_EXPORT size_t jce_mock_sizeof_raycast_hit(void)
{
    return sizeof(JceScriptRaycastHit);
}

static char *mock_alloc_string(const char *member, size_t extra)
{
    char  *s;
    size_t head;
    size_t want = strlen(member) + extra + 64u;

    s = (char *)malloc(want);
    if (!s)
        return NULL;
    snprintf(s, want, "{\"member\":\"%s\",\"n\":%u,\"pad\":\"", member,
             mock_hash(member));
    head = strlen(s);
    if (head + extra + 3u < want) {
        memset(s + head, '.', extra);
        s[head + extra] = '"';
        s[head + extra + 1] = '}';
        s[head + extra + 2] = '\0';
    }
    if (g_alloc_count < JCE_MOCK_MAX_ALLOCS) {
        g_allocs[g_alloc_count] = s;
        g_alloc_ids[g_alloc_count] = ++g_alloc_seq;
        g_alloc_count++;
    }
    g_live_strings++;
    return s;
}

/* Find the id of a live allocation and RETIRE the slot.
 *
 * Retiring matters: malloc is free to hand the next allocation the address it
 * just took back, and a table that kept dead entries would then report the
 * OLD id for the NEW string.  Whether that happens depends on each process's
 * allocator history, so the two sides of the differential would disagree at
 * random -- a flaky trace difference that looks exactly like a real defect. */
static int mock_alloc_id(const char *p)
{
    int i;
    if (!p)
        return -1;               /* or NULL would match the first retired slot */
    for (i = 0; i < g_alloc_count; i++) {
        if (g_allocs[i] == p) {
            g_allocs[i] = NULL;
            return g_alloc_ids[i];
        }
    }
    return -1;
}


/* Owned strings are made longer than OWNED_STRING_INITIAL_CAPACITY in one mode, to reach the C ABI's
 * copy-out retry path from the Python side. */
#define OWNED_LONG_EXTRA 9000u
static int g_long_strings;

JCE_MOCK_EXPORT void jce_mock_set_long_strings(int on)
{
    g_long_strings = on;
}

static bool mock_get_position(void *user, JceScriptEntity e, float out_xyz[3])
{
    char line[1024];
    unsigned h = mock_hash("get_position");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL get_position(e=%llu)",
             (unsigned long long)e);
    mock_emit(line);
    if (g_mode == JCE_MOCK_MODE_MISS)
        return false;
    out_xyz[0] = (float)(h + 0u) + 0.25f;
    out_xyz[1] = (float)(h + 1u) + 0.25f;
    out_xyz[2] = (float)(h + 2u) + 0.25f;
    return true;
}

static void mock_set_position(void *user, JceScriptEntity e, float x, float y, float z)
{
    char line[1024];
    unsigned h = mock_hash("set_position");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL set_position(e=%llu, x=%.9g, y=%.9g, z=%.9g)",
             (unsigned long long)e, (double)x, (double)y, (double)z);
    mock_emit(line);
}

static bool mock_get_rotation(void *user, JceScriptEntity e, float out_euler_deg[3])
{
    char line[1024];
    unsigned h = mock_hash("get_rotation");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL get_rotation(e=%llu)",
             (unsigned long long)e);
    mock_emit(line);
    if (g_mode == JCE_MOCK_MODE_MISS)
        return false;
    out_euler_deg[0] = (float)(h + 0u) + 0.25f;
    out_euler_deg[1] = (float)(h + 1u) + 0.25f;
    out_euler_deg[2] = (float)(h + 2u) + 0.25f;
    return true;
}

static void mock_set_rotation(void *user, JceScriptEntity e, float x, float y, float z)
{
    char line[1024];
    unsigned h = mock_hash("set_rotation");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL set_rotation(e=%llu, x=%.9g, y=%.9g, z=%.9g)",
             (unsigned long long)e, (double)x, (double)y, (double)z);
    mock_emit(line);
}

static bool mock_get_scale(void *user, JceScriptEntity e, float out_xyz[3])
{
    char line[1024];
    unsigned h = mock_hash("get_scale");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL get_scale(e=%llu)",
             (unsigned long long)e);
    mock_emit(line);
    if (g_mode == JCE_MOCK_MODE_MISS)
        return false;
    out_xyz[0] = (float)(h + 0u) + 0.25f;
    out_xyz[1] = (float)(h + 1u) + 0.25f;
    out_xyz[2] = (float)(h + 2u) + 0.25f;
    return true;
}

static bool mock_get_world_position(void *user, JceScriptEntity e, float out_xyz[3])
{
    char line[1024];
    unsigned h = mock_hash("get_world_position");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL get_world_position(e=%llu)",
             (unsigned long long)e);
    mock_emit(line);
    if (g_mode == JCE_MOCK_MODE_MISS)
        return false;
    out_xyz[0] = (float)(h + 0u) + 0.25f;
    out_xyz[1] = (float)(h + 1u) + 0.25f;
    out_xyz[2] = (float)(h + 2u) + 0.25f;
    return true;
}

static void mock_set_scale(void *user, JceScriptEntity e, float x, float y, float z)
{
    char line[1024];
    unsigned h = mock_hash("set_scale");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL set_scale(e=%llu, x=%.9g, y=%.9g, z=%.9g)",
             (unsigned long long)e, (double)x, (double)y, (double)z);
    mock_emit(line);
}

static bool mock_set_parent(void *user, JceScriptEntity child, JceScriptEntity parent, bool preserve_world)
{
    char line[1024];
    unsigned h = mock_hash("set_parent");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL set_parent(child=%llu, parent=%llu, preserve_world=%s)",
             (unsigned long long)child, (unsigned long long)parent, (preserve_world ? "true" : "false"));
    mock_emit(line);
    return g_mode != JCE_MOCK_MODE_MISS;
}

static JceScriptEntity mock_get_parent(void *user, JceScriptEntity child)
{
    char line[1024];
    unsigned h = mock_hash("get_parent");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL get_parent(child=%llu)",
             (unsigned long long)child);
    mock_emit(line);
    if (g_mode == JCE_MOCK_MODE_MISS)
        return 0;
    return (JceScriptEntity)h;
}

static bool mock_is_key_down(void *user, int keycode)
{
    char line[1024];
    unsigned h = mock_hash("is_key_down");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL is_key_down(keycode=%d)",
             (int)keycode);
    mock_emit(line);
    return g_mode != JCE_MOCK_MODE_MISS;
}

static JceScriptEntity mock_find_with_tag(void *user, const char * tag)
{
    char line[1024];
    unsigned h = mock_hash("find_with_tag");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL find_with_tag(tag=%s)",
             mock_str(tag));
    mock_emit(line);
    if (g_mode == JCE_MOCK_MODE_MISS)
        return 0;
    return (JceScriptEntity)h;
}

static void mock_destroy_entity(void *user, JceScriptEntity e)
{
    char line[1024];
    unsigned h = mock_hash("destroy_entity");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL destroy_entity(e=%llu)",
             (unsigned long long)e);
    mock_emit(line);
}

static JceScriptEntity mock_spawn(void *user, const char * prefab_path, float x, float y, float z)
{
    char line[1024];
    unsigned h = mock_hash("spawn");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL spawn(prefab_path=%s, x=%.9g, y=%.9g, z=%.9g)",
             mock_str(prefab_path), (double)x, (double)y, (double)z);
    mock_emit(line);
    if (g_mode == JCE_MOCK_MODE_MISS)
        return 0;
    return (JceScriptEntity)h;
}

static void mock_move_axis(void *user, float out_xz[2])
{
    char line[1024];
    unsigned h = mock_hash("move_axis");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL move_axis()");
    mock_emit(line);
    out_xz[0] = (float)(h + 0u) + 0.25f;
    out_xz[1] = (float)(h + 1u) + 0.25f;
}

static bool mock_input_button(void *user, int button)
{
    char line[1024];
    unsigned h = mock_hash("input_button");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL input_button(button=%d)",
             (int)button);
    mock_emit(line);
    return g_mode != JCE_MOCK_MODE_MISS;
}

static void mock_set_time_scale(void *user, float scale)
{
    char line[1024];
    unsigned h = mock_hash("set_time_scale");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL set_time_scale(scale=%.9g)",
             (double)scale);
    mock_emit(line);
}

static void mock_set_paused(void *user, bool paused)
{
    char line[1024];
    unsigned h = mock_hash("set_paused");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL set_paused(paused=%s)",
             (paused ? "true" : "false"));
    mock_emit(line);
}

static void mock_shake_camera(void *user, float amount)
{
    char line[1024];
    unsigned h = mock_hash("shake_camera");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL shake_camera(amount=%.9g)",
             (double)amount);
    mock_emit(line);
}

static void mock_music_set_intensity(void *user, float intensity)
{
    char line[1024];
    unsigned h = mock_hash("music_set_intensity");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL music_set_intensity(intensity=%.9g)",
             (double)intensity);
    mock_emit(line);
}

static float mock_music_get_intensity(void *user)
{
    char line[1024];
    unsigned h = mock_hash("music_get_intensity");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL music_get_intensity()");
    mock_emit(line);
    if (g_mode == JCE_MOCK_MODE_MISS)
        return -1.5f;
    return (float)h + 0.5f;
}

static float mock_music_request_transition(void *user, int to_segment)
{
    char line[1024];
    unsigned h = mock_hash("music_request_transition");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL music_request_transition(to_segment=%d)",
             (int)to_segment);
    mock_emit(line);
    if (g_mode == JCE_MOCK_MODE_MISS)
        return -1.5f;
    return (float)h + 0.5f;
}

static bool mock_gas_activate(void *user, JceScriptEntity e, uint32_t ability_id)
{
    char line[1024];
    unsigned h = mock_hash("gas_activate");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL gas_activate(e=%llu, ability_id=%u)",
             (unsigned long long)e, (unsigned)ability_id);
    mock_emit(line);
    return g_mode != JCE_MOCK_MODE_MISS;
}

static bool mock_gas_get(void *user, JceScriptEntity e, const char * attr_name, float *out_value)
{
    char line[1024];
    unsigned h = mock_hash("gas_get");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL gas_get(e=%llu, attr_name=%s)",
             (unsigned long long)e, mock_str(attr_name));
    mock_emit(line);
    if (g_mode == JCE_MOCK_MODE_MISS)
        return false;
    *out_value = (float)(h + 0u) + 0.25f;
    return true;
}

static bool mock_gas_apply(void *user, JceScriptEntity e, const char * attr_name, int op, float magnitude, float duration_seconds)
{
    char line[1024];
    unsigned h = mock_hash("gas_apply");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL gas_apply(e=%llu, attr_name=%s, op=%d, magnitude=%.9g, duration_seconds=%.9g)",
             (unsigned long long)e, mock_str(attr_name), (int)op, (double)magnitude, (double)duration_seconds);
    mock_emit(line);
    return g_mode != JCE_MOCK_MODE_MISS;
}

static bool mock_raycast(void *user, const float origin[3], const float dir[3], float max_dist, JceScriptRaycastHit *out)
{
    char line[1024];
    unsigned h = mock_hash("raycast");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL raycast(origin=[%.9g,%.9g,%.9g], dir=[%.9g,%.9g,%.9g], max_dist=%.9g)",
             (double)origin[0], (double)origin[1], (double)origin[2], (double)dir[0], (double)dir[1], (double)dir[2], (double)max_dist);
    mock_emit(line);
    if (g_mode == JCE_MOCK_MODE_MISS)
        return false;
    out->entity = (JceScriptEntity)(h + 0u);
    out->point[0] = (float)(h + 1u) + 0.25f;
    out->point[1] = (float)(h + 2u) + 0.25f;
    out->point[2] = (float)(h + 3u) + 0.25f;
    out->normal[0] = (float)(h + 4u) + 0.25f;
    out->normal[1] = (float)(h + 5u) + 0.25f;
    out->normal[2] = (float)(h + 6u) + 0.25f;
    out->distance = (float)(h + 7u) + 0.25f;
    return true;
}

static bool mock_raycast_filtered(void *user, const float origin[3], const float dir[3], float max_dist, uint32_t layer_mask, bool hit_triggers, JceScriptRaycastHit *out)
{
    char line[1024];
    unsigned h = mock_hash("raycast_filtered");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL raycast_filtered(origin=[%.9g,%.9g,%.9g], dir=[%.9g,%.9g,%.9g], max_dist=%.9g, layer_mask=%u, hit_triggers=%s)",
             (double)origin[0], (double)origin[1], (double)origin[2], (double)dir[0], (double)dir[1], (double)dir[2], (double)max_dist, (unsigned)layer_mask, (hit_triggers ? "true" : "false"));
    mock_emit(line);
    if (g_mode == JCE_MOCK_MODE_MISS)
        return false;
    out->entity = (JceScriptEntity)(h + 0u);
    out->point[0] = (float)(h + 1u) + 0.25f;
    out->point[1] = (float)(h + 2u) + 0.25f;
    out->point[2] = (float)(h + 3u) + 0.25f;
    out->normal[0] = (float)(h + 4u) + 0.25f;
    out->normal[1] = (float)(h + 5u) + 0.25f;
    out->normal[2] = (float)(h + 6u) + 0.25f;
    out->distance = (float)(h + 7u) + 0.25f;
    return true;
}

static int mock_raycast_all(void *user, const float origin[3], const float dir[3], float max_dist, uint32_t layer_mask, bool hit_triggers, JceScriptEntity *out, int max)
{
    char line[1024];
    int n, i;
    unsigned h = mock_hash("raycast_all");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL raycast_all(origin=[%.9g,%.9g,%.9g], dir=[%.9g,%.9g,%.9g], max_dist=%.9g, layer_mask=%u, hit_triggers=%s, max=%d)",
             (double)origin[0], (double)origin[1], (double)origin[2], (double)dir[0], (double)dir[1], (double)dir[2], (double)max_dist, (unsigned)layer_mask, (hit_triggers ? "true" : "false"), (int)max);
    mock_emit(line);
    if (g_mode == JCE_MOCK_MODE_MISS)
        return -3;
    n = (max < 3) ? max : 3;
    for (i = 0; i < n; i++)
        out[i] = (JceScriptEntity)(h * 100u + (unsigned)i);
    return n;
}

static void mock_apply_impulse(void *user, JceScriptEntity e, float x, float y, float z)
{
    char line[1024];
    unsigned h = mock_hash("apply_impulse");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL apply_impulse(e=%llu, x=%.9g, y=%.9g, z=%.9g)",
             (unsigned long long)e, (double)x, (double)y, (double)z);
    mock_emit(line);
}

static void mock_set_velocity(void *user, JceScriptEntity e, float x, float y, float z)
{
    char line[1024];
    unsigned h = mock_hash("set_velocity");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL set_velocity(e=%llu, x=%.9g, y=%.9g, z=%.9g)",
             (unsigned long long)e, (double)x, (double)y, (double)z);
    mock_emit(line);
}

static void mock_anim_set_float(void *user, JceScriptEntity e, const char * name, float v)
{
    char line[1024];
    unsigned h = mock_hash("anim_set_float");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL anim_set_float(e=%llu, name=%s, v=%.9g)",
             (unsigned long long)e, mock_str(name), (double)v);
    mock_emit(line);
}

static void mock_anim_set_int(void *user, JceScriptEntity e, const char * name, int v)
{
    char line[1024];
    unsigned h = mock_hash("anim_set_int");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL anim_set_int(e=%llu, name=%s, v=%d)",
             (unsigned long long)e, mock_str(name), (int)v);
    mock_emit(line);
}

static void mock_anim_set_bool(void *user, JceScriptEntity e, const char * name, bool v)
{
    char line[1024];
    unsigned h = mock_hash("anim_set_bool");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL anim_set_bool(e=%llu, name=%s, v=%s)",
             (unsigned long long)e, mock_str(name), (v ? "true" : "false"));
    mock_emit(line);
}

static void mock_anim_set_trigger(void *user, JceScriptEntity e, const char * name)
{
    char line[1024];
    unsigned h = mock_hash("anim_set_trigger");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL anim_set_trigger(e=%llu, name=%s)",
             (unsigned long long)e, mock_str(name));
    mock_emit(line);
}

static bool mock_action_down(void *user, const char * name)
{
    char line[1024];
    unsigned h = mock_hash("action_down");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL action_down(name=%s)",
             mock_str(name));
    mock_emit(line);
    return g_mode != JCE_MOCK_MODE_MISS;
}

static bool mock_action_pressed(void *user, const char * name)
{
    char line[1024];
    unsigned h = mock_hash("action_pressed");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL action_pressed(name=%s)",
             mock_str(name));
    mock_emit(line);
    return g_mode != JCE_MOCK_MODE_MISS;
}

static float mock_action_axis(void *user, const char * name)
{
    char line[1024];
    unsigned h = mock_hash("action_axis");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL action_axis(name=%s)",
             mock_str(name));
    mock_emit(line);
    if (g_mode == JCE_MOCK_MODE_MISS)
        return -1.5f;
    return (float)h + 0.5f;
}

static void mock_pointer_delta(void *user, float out_xy[2])
{
    char line[1024];
    unsigned h = mock_hash("pointer_delta");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL pointer_delta()");
    mock_emit(line);
    out_xy[0] = (float)(h + 0u) + 0.25f;
    out_xy[1] = (float)(h + 1u) + 0.25f;
}

static float mock_pointer_wheel(void *user)
{
    char line[1024];
    unsigned h = mock_hash("pointer_wheel");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL pointer_wheel()");
    mock_emit(line);
    if (g_mode == JCE_MOCK_MODE_MISS)
        return -1.5f;
    return (float)h + 0.5f;
}

static bool mock_pointer_button(void *user, int button)
{
    char line[1024];
    unsigned h = mock_hash("pointer_button");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL pointer_button(button=%d)",
             (int)button);
    mock_emit(line);
    return g_mode != JCE_MOCK_MODE_MISS;
}

static int mock_touch_count(void *user)
{
    char line[1024];
    unsigned h = mock_hash("touch_count");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL touch_count()");
    mock_emit(line);
    return (g_mode == JCE_MOCK_MODE_MISS) ? -3 : (int)h;
}

static bool mock_touch_get(void *user, int index, uint64_t *id, float *x, float *y, float *pressure)
{
    char line[1024];
    unsigned h = mock_hash("touch_get");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL touch_get(index=%d)",
             (int)index);
    mock_emit(line);
    if (g_mode == JCE_MOCK_MODE_MISS)
        return false;
    *id = (uint64_t)(h + 0u);
    *x = (float)(h + 1u) + 0.25f;
    *y = (float)(h + 2u) + 0.25f;
    *pressure = (float)(h + 3u) + 0.25f;
    return true;
}

static const char *mock_loc_translate(void *user, const char * key)
{
    char line[1024];
    unsigned h = mock_hash("loc_translate");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL loc_translate(key=%s)",
             mock_str(key));
    mock_emit(line);
    if (g_mode == JCE_MOCK_MODE_MISS)
        return NULL;
    return "loc_translate/" "\xc3\xa9" "\xe4\xb8\xad";
}

static const char *mock_loc_get_locale(void *user)
{
    char line[1024];
    unsigned h = mock_hash("loc_get_locale");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL loc_get_locale()");
    mock_emit(line);
    if (g_mode == JCE_MOCK_MODE_MISS)
        return NULL;
    return "loc_get_locale/" "\xc3\xa9" "\xe4\xb8\xad";
}

static void mock_loc_set_locale(void *user, const char * locale)
{
    char line[1024];
    unsigned h = mock_hash("loc_set_locale");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL loc_set_locale(locale=%s)",
             mock_str(locale));
    mock_emit(line);
}

static bool mock_get_velocity(void *user, JceScriptEntity e, float out[3])
{
    char line[1024];
    unsigned h = mock_hash("get_velocity");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL get_velocity(e=%llu)",
             (unsigned long long)e);
    mock_emit(line);
    if (g_mode == JCE_MOCK_MODE_MISS)
        return false;
    out[0] = (float)(h + 0u) + 0.25f;
    out[1] = (float)(h + 1u) + 0.25f;
    out[2] = (float)(h + 2u) + 0.25f;
    return true;
}

static void mock_vehicle_set_input(void *user, JceScriptEntity e, float throttle, float brake, float steer)
{
    char line[1024];
    unsigned h = mock_hash("vehicle_set_input");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL vehicle_set_input(e=%llu, throttle=%.9g, brake=%.9g, steer=%.9g)",
             (unsigned long long)e, (double)throttle, (double)brake, (double)steer);
    mock_emit(line);
}

static float mock_vehicle_get_speed(void *user, JceScriptEntity e)
{
    char line[1024];
    unsigned h = mock_hash("vehicle_get_speed");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL vehicle_get_speed(e=%llu)",
             (unsigned long long)e);
    mock_emit(line);
    if (g_mode == JCE_MOCK_MODE_MISS)
        return -1.5f;
    return (float)h + 0.5f;
}

static void mock_get_move(void *user, float out[3])
{
    char line[1024];
    unsigned h = mock_hash("get_move");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL get_move()");
    mock_emit(line);
    out[0] = (float)(h + 0u) + 0.25f;
    out[1] = (float)(h + 1u) + 0.25f;
    out[2] = (float)(h + 2u) + 0.25f;
}

static bool mock_ui_get_slider(void *user, JceScriptEntity e, float *out)
{
    char line[1024];
    unsigned h = mock_hash("ui_get_slider");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL ui_get_slider(e=%llu)",
             (unsigned long long)e);
    mock_emit(line);
    if (g_mode == JCE_MOCK_MODE_MISS)
        return false;
    *out = (float)(h + 0u) + 0.25f;
    return true;
}

static void mock_ui_set_slider(void *user, JceScriptEntity e, float v)
{
    char line[1024];
    unsigned h = mock_hash("ui_set_slider");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL ui_set_slider(e=%llu, v=%.9g)",
             (unsigned long long)e, (double)v);
    mock_emit(line);
}

static bool mock_ui_get_progress(void *user, JceScriptEntity e, float *out)
{
    char line[1024];
    unsigned h = mock_hash("ui_get_progress");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL ui_get_progress(e=%llu)",
             (unsigned long long)e);
    mock_emit(line);
    if (g_mode == JCE_MOCK_MODE_MISS)
        return false;
    *out = (float)(h + 0u) + 0.25f;
    return true;
}

static void mock_ui_set_progress(void *user, JceScriptEntity e, float v)
{
    char line[1024];
    unsigned h = mock_hash("ui_set_progress");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL ui_set_progress(e=%llu, v=%.9g)",
             (unsigned long long)e, (double)v);
    mock_emit(line);
}

static bool mock_ui_get_toggle(void *user, JceScriptEntity e, bool *out)
{
    char line[1024];
    unsigned h = mock_hash("ui_get_toggle");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL ui_get_toggle(e=%llu)",
             (unsigned long long)e);
    mock_emit(line);
    if (g_mode == JCE_MOCK_MODE_MISS)
        return false;
    *out = true;
    return true;
}

static void mock_ui_set_toggle(void *user, JceScriptEntity e, bool v)
{
    char line[1024];
    unsigned h = mock_hash("ui_set_toggle");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL ui_set_toggle(e=%llu, v=%s)",
             (unsigned long long)e, (v ? "true" : "false"));
    mock_emit(line);
}

static void mock_ui_set_text(void *user, JceScriptEntity e, const char * txt)
{
    char line[1024];
    unsigned h = mock_hash("ui_set_text");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL ui_set_text(e=%llu, txt=%s)",
             (unsigned long long)e, mock_str(txt));
    mock_emit(line);
}

static void mock_send_message(void *user, JceScriptEntity target, const char * msg, double number_arg, const char * str_arg)
{
    char line[1024];
    unsigned h = mock_hash("send_message");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL send_message(target=%llu, msg=%s, number_arg=%.9g, str_arg=%s)",
             (unsigned long long)target, mock_str(msg), (double)number_arg, mock_str(str_arg));
    mock_emit(line);
}

static void mock_broadcast(void *user, const char * msg, double number_arg, const char * str_arg)
{
    char line[1024];
    unsigned h = mock_hash("broadcast");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL broadcast(msg=%s, number_arg=%.9g, str_arg=%s)",
             mock_str(msg), (double)number_arg, mock_str(str_arg));
    mock_emit(line);
}

static bool mock_has_component(void *user, JceScriptEntity e, const char * comp_name)
{
    char line[1024];
    unsigned h = mock_hash("has_component");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL has_component(e=%llu, comp_name=%s)",
             (unsigned long long)e, mock_str(comp_name));
    mock_emit(line);
    return g_mode != JCE_MOCK_MODE_MISS;
}

static bool mock_is_component_enabled(void *user, JceScriptEntity e, const char * comp_name)
{
    char line[1024];
    unsigned h = mock_hash("is_component_enabled");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL is_component_enabled(e=%llu, comp_name=%s)",
             (unsigned long long)e, mock_str(comp_name));
    mock_emit(line);
    return g_mode != JCE_MOCK_MODE_MISS;
}

static void mock_set_component_enabled(void *user, JceScriptEntity e, const char * comp_name, bool on)
{
    char line[1024];
    unsigned h = mock_hash("set_component_enabled");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL set_component_enabled(e=%llu, comp_name=%s, on=%s)",
             (unsigned long long)e, mock_str(comp_name), (on ? "true" : "false"));
    mock_emit(line);
}

static bool mock_net_is_server(void *user)
{
    char line[1024];
    unsigned h = mock_hash("net_is_server");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL net_is_server()");
    mock_emit(line);
    return g_mode != JCE_MOCK_MODE_MISS;
}

static bool mock_net_is_client(void *user)
{
    char line[1024];
    unsigned h = mock_hash("net_is_client");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL net_is_client()");
    mock_emit(line);
    return g_mode != JCE_MOCK_MODE_MISS;
}

static JceScriptEntity mock_net_spawn(void *user, const char * prefab_path, float x, float y, float z)
{
    char line[1024];
    unsigned h = mock_hash("net_spawn");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL net_spawn(prefab_path=%s, x=%.9g, y=%.9g, z=%.9g)",
             mock_str(prefab_path), (double)x, (double)y, (double)z);
    mock_emit(line);
    if (g_mode == JCE_MOCK_MODE_MISS)
        return 0;
    return (JceScriptEntity)h;
}

static bool mock_rpc_send(void *user, JceScriptEntity e, const char * event, int target, const char * payload)
{
    char line[1024];
    unsigned h = mock_hash("rpc_send");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL rpc_send(e=%llu, event=%s, target=%d, payload=%s)",
             (unsigned long long)e, mock_str(event), (int)target, mock_str(payload));
    mock_emit(line);
    return g_mode != JCE_MOCK_MODE_MISS;
}

static void mock_particle_burst(void *user, JceScriptEntity e, int count)
{
    char line[1024];
    unsigned h = mock_hash("particle_burst");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL particle_burst(e=%llu, count=%d)",
             (unsigned long long)e, (int)count);
    mock_emit(line);
}

static void mock_particle_set_emitting(void *user, JceScriptEntity e, bool on)
{
    char line[1024];
    unsigned h = mock_hash("particle_set_emitting");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL particle_set_emitting(e=%llu, on=%s)",
             (unsigned long long)e, (on ? "true" : "false"));
    mock_emit(line);
}

static void mock_particle_set_color(void *user, JceScriptEntity e, float r, float g, float b)
{
    char line[1024];
    unsigned h = mock_hash("particle_set_color");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL particle_set_color(e=%llu, r=%.9g, g=%.9g, b=%.9g)",
             (unsigned long long)e, (double)r, (double)g, (double)b);
    mock_emit(line);
}

static int mock_find_by_name(void *user, const char * name, JceScriptEntity *out, int max)
{
    char line[1024];
    int n, i;
    unsigned h = mock_hash("find_by_name");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL find_by_name(name=%s, max=%d)",
             mock_str(name), (int)max);
    mock_emit(line);
    if (g_mode == JCE_MOCK_MODE_MISS)
        return -3;
    n = (max < 3) ? max : 3;
    for (i = 0; i < n; i++)
        out[i] = (JceScriptEntity)(h * 100u + (unsigned)i);
    return n;
}

static int mock_find_by_prefix(void *user, const char * prefix, JceScriptEntity *out, int max)
{
    char line[1024];
    int n, i;
    unsigned h = mock_hash("find_by_prefix");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL find_by_prefix(prefix=%s, max=%d)",
             mock_str(prefix), (int)max);
    mock_emit(line);
    if (g_mode == JCE_MOCK_MODE_MISS)
        return -3;
    n = (max < 3) ? max : 3;
    for (i = 0; i < n; i++)
        out[i] = (JceScriptEntity)(h * 100u + (unsigned)i);
    return n;
}

static char *mock_comp_get_json(void *user, JceScriptEntity e, const char * type)
{
    char line[1024];
    unsigned h = mock_hash("comp_get_json");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL comp_get_json(e=%llu, type=%s)",
             (unsigned long long)e, mock_str(type));
    mock_emit(line);
    if (g_mode == JCE_MOCK_MODE_MISS)
        return NULL;
    return mock_alloc_string("comp_get_json", g_long_strings ? OWNED_LONG_EXTRA : 0u);
}

static bool mock_comp_set_json(void *user, JceScriptEntity e, const char * type, const char * json)
{
    char line[1024];
    unsigned h = mock_hash("comp_set_json");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL comp_set_json(e=%llu, type=%s, json=%s)",
             (unsigned long long)e, mock_str(type), mock_str(json));
    mock_emit(line);
    return g_mode != JCE_MOCK_MODE_MISS;
}

static char *mock_render_get_json(void *user)
{
    char line[1024];
    unsigned h = mock_hash("render_get_json");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL render_get_json()");
    mock_emit(line);
    if (g_mode == JCE_MOCK_MODE_MISS)
        return NULL;
    return mock_alloc_string("render_get_json", g_long_strings ? OWNED_LONG_EXTRA : 0u);
}

static bool mock_render_set_json(void *user, const char * json)
{
    char line[1024];
    unsigned h = mock_hash("render_set_json");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL render_set_json(json=%s)",
             mock_str(json));
    mock_emit(line);
    return g_mode != JCE_MOCK_MODE_MISS;
}

static void mock_audio_set_volume(void *user, JceScriptEntity e, float volume)
{
    char line[1024];
    unsigned h = mock_hash("audio_set_volume");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL audio_set_volume(e=%llu, volume=%.9g)",
             (unsigned long long)e, (double)volume);
    mock_emit(line);
}

static bool mock_ui_get_dropdown(void *user, JceScriptEntity e, int *out)
{
    char line[1024];
    unsigned h = mock_hash("ui_get_dropdown");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL ui_get_dropdown(e=%llu)",
             (unsigned long long)e);
    mock_emit(line);
    if (g_mode == JCE_MOCK_MODE_MISS)
        return false;
    *out = (int)(h + 0u);
    return true;
}

static void mock_ui_set_dropdown(void *user, JceScriptEntity e, int index)
{
    char line[1024];
    unsigned h = mock_hash("ui_set_dropdown");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL ui_set_dropdown(e=%llu, index=%d)",
             (unsigned long long)e, (int)index);
    mock_emit(line);
}

static const char *mock_ui_get_input_text(void *user, JceScriptEntity e)
{
    char line[1024];
    unsigned h = mock_hash("ui_get_input_text");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL ui_get_input_text(e=%llu)",
             (unsigned long long)e);
    mock_emit(line);
    if (g_mode == JCE_MOCK_MODE_MISS)
        return NULL;
    return "ui_get_input_text/" "\xc3\xa9" "\xe4\xb8\xad";
}

static void mock_ui_set_input_text(void *user, JceScriptEntity e, const char * text)
{
    char line[1024];
    unsigned h = mock_hash("ui_set_input_text");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL ui_set_input_text(e=%llu, text=%s)",
             (unsigned long long)e, mock_str(text));
    mock_emit(line);
}

static bool mock_ui_get_scroll(void *user, JceScriptEntity e, float out_xy[2])
{
    char line[1024];
    unsigned h = mock_hash("ui_get_scroll");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL ui_get_scroll(e=%llu)",
             (unsigned long long)e);
    mock_emit(line);
    if (g_mode == JCE_MOCK_MODE_MISS)
        return false;
    out_xy[0] = (float)(h + 0u) + 0.25f;
    out_xy[1] = (float)(h + 1u) + 0.25f;
    return true;
}

static void mock_ui_set_scroll(void *user, JceScriptEntity e, float x, float y)
{
    char line[1024];
    unsigned h = mock_hash("ui_set_scroll");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL ui_set_scroll(e=%llu, x=%.9g, y=%.9g)",
             (unsigned long long)e, (double)x, (double)y);
    mock_emit(line);
}

static float mock_world_get_hour(void *user)
{
    char line[1024];
    unsigned h = mock_hash("world_get_hour");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL world_get_hour()");
    mock_emit(line);
    if (g_mode == JCE_MOCK_MODE_MISS)
        return -1.5f;
    return (float)h + 0.5f;
}

static void mock_world_set_hour(void *user, float hour)
{
    char line[1024];
    unsigned h = mock_hash("world_set_hour");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL world_set_hour(hour=%.9g)",
             (double)hour);
    mock_emit(line);
}

static bool mock_world_is_daytime(void *user)
{
    char line[1024];
    unsigned h = mock_hash("world_is_daytime");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL world_is_daytime()");
    mock_emit(line);
    return g_mode != JCE_MOCK_MODE_MISS;
}

static int mock_world_get_weather(void *user)
{
    char line[1024];
    unsigned h = mock_hash("world_get_weather");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL world_get_weather()");
    mock_emit(line);
    return (g_mode == JCE_MOCK_MODE_MISS) ? -3 : (int)h;
}

static float mock_world_get_weather_intensity(void *user)
{
    char line[1024];
    unsigned h = mock_hash("world_get_weather_intensity");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL world_get_weather_intensity()");
    mock_emit(line);
    if (g_mode == JCE_MOCK_MODE_MISS)
        return -1.5f;
    return (float)h + 0.5f;
}

static float mock_world_get_wind_speed(void *user)
{
    char line[1024];
    unsigned h = mock_hash("world_get_wind_speed");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL world_get_wind_speed()");
    mock_emit(line);
    if (g_mode == JCE_MOCK_MODE_MISS)
        return -1.5f;
    return (float)h + 0.5f;
}

static bool mock_request_scene(void *user, const char * scene_path)
{
    char line[1024];
    unsigned h = mock_hash("request_scene");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL request_scene(scene_path=%s)",
             mock_str(scene_path));
    mock_emit(line);
    return g_mode != JCE_MOCK_MODE_MISS;
}

static bool mock_is_transitioning(void *user)
{
    char line[1024];
    unsigned h = mock_hash("is_transitioning");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL is_transitioning()");
    mock_emit(line);
    return g_mode != JCE_MOCK_MODE_MISS;
}

static bool mock_audio_play(void *user, JceScriptEntity e)
{
    char line[1024];
    unsigned h = mock_hash("audio_play");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL audio_play(e=%llu)",
             (unsigned long long)e);
    mock_emit(line);
    return g_mode != JCE_MOCK_MODE_MISS;
}

static bool mock_audio_stop(void *user, JceScriptEntity e)
{
    char line[1024];
    unsigned h = mock_hash("audio_stop");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL audio_stop(e=%llu)",
             (unsigned long long)e);
    mock_emit(line);
    return g_mode != JCE_MOCK_MODE_MISS;
}

static bool mock_audio_is_playing(void *user, JceScriptEntity e)
{
    char line[1024];
    unsigned h = mock_hash("audio_is_playing");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL audio_is_playing(e=%llu)",
             (unsigned long long)e);
    mock_emit(line);
    return g_mode != JCE_MOCK_MODE_MISS;
}

static bool mock_save_game(void *user, const char * path)
{
    char line[1024];
    unsigned h = mock_hash("save_game");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL save_game(path=%s)",
             mock_str(path));
    mock_emit(line);
    return g_mode != JCE_MOCK_MODE_MISS;
}

static bool mock_load_game(void *user, const char * path)
{
    char line[1024];
    unsigned h = mock_hash("load_game");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL load_game(path=%s)",
             mock_str(path));
    mock_emit(line);
    return g_mode != JCE_MOCK_MODE_MISS;
}

static int mock_overlap_sphere(void *user, float x, float y, float z, float radius, uint32_t layer_mask, JceScriptEntity *out, int max)
{
    char line[1024];
    int n, i;
    unsigned h = mock_hash("overlap_sphere");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL overlap_sphere(x=%.9g, y=%.9g, z=%.9g, radius=%.9g, layer_mask=%u, max=%d)",
             (double)x, (double)y, (double)z, (double)radius, (unsigned)layer_mask, (int)max);
    mock_emit(line);
    if (g_mode == JCE_MOCK_MODE_MISS)
        return -3;
    n = (max < 3) ? max : 3;
    for (i = 0; i < n; i++)
        out[i] = (JceScriptEntity)(h * 100u + (unsigned)i);
    return n;
}

static int mock_overlap_box(void *user, float x, float y, float z, float hx, float hy, float hz, uint32_t layer_mask, JceScriptEntity *out, int max)
{
    char line[1024];
    int n, i;
    unsigned h = mock_hash("overlap_box");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL overlap_box(x=%.9g, y=%.9g, z=%.9g, hx=%.9g, hy=%.9g, hz=%.9g, layer_mask=%u, max=%d)",
             (double)x, (double)y, (double)z, (double)hx, (double)hy, (double)hz, (unsigned)layer_mask, (int)max);
    mock_emit(line);
    if (g_mode == JCE_MOCK_MODE_MISS)
        return -3;
    n = (max < 3) ? max : 3;
    for (i = 0; i < n; i++)
        out[i] = (JceScriptEntity)(h * 100u + (unsigned)i);
    return n;
}

static bool mock_get_script_param(void *user, JceScriptEntity e, const char * name, int *out_kind, double *out_number, JceScriptEntity *out_entity)
{
    char line[1024];
    unsigned h = mock_hash("get_script_param");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL get_script_param(e=%llu, name=%s)",
             (unsigned long long)e, mock_str(name));
    mock_emit(line);
    if (g_mode == JCE_MOCK_MODE_MISS)
        return false;
    *out_kind = (int)(h + 0u);
    *out_number = (double)(h + 1u) + 0.25;
    *out_entity = (JceScriptEntity)(h + 2u);
    return true;
}

static const char *mock_get_script_param_text(void *user, JceScriptEntity e, const char * name)
{
    char line[1024];
    unsigned h = mock_hash("get_script_param_text");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL get_script_param_text(e=%llu, name=%s)",
             (unsigned long long)e, mock_str(name));
    mock_emit(line);
    if (g_mode == JCE_MOCK_MODE_MISS)
        return NULL;
    return "get_script_param_text/" "\xc3\xa9" "\xe4\xb8\xad";
}

static bool mock_curve_eval(void *user, const char * path, const char * channel, double t, double *out_value)
{
    char line[1024];
    unsigned h = mock_hash("curve_eval");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL curve_eval(path=%s, channel=%s, t=%.9g)",
             mock_str(path), mock_str(channel), (double)t);
    mock_emit(line);
    if (g_mode == JCE_MOCK_MODE_MISS)
        return false;
    *out_value = (double)(h + 0u) + 0.25;
    return true;
}

static int mock_vcam_activate(void *user, const char * name)
{
    char line[1024];
    unsigned h = mock_hash("vcam_activate");

    (void)user;
    (void)h;
    snprintf(line, sizeof line, "CALL vcam_activate(name=%s)",
             mock_str(name));
    mock_emit(line);
    return (g_mode == JCE_MOCK_MODE_MISS) ? -3 : (int)h;
}

static void mock_json_free(void *user, char *s)
{
    char line[1024];
    int id;

    (void)user;
    id = mock_alloc_id(s);
    snprintf(line, sizeof line, "CALL json_free(alloc#%d)", id);
    mock_emit(line);
    free(s);
    g_live_strings--;
}

/* Four host tables, one per mode.  ABSENT keeps only `log`, which is
 * the trace wire and not part of the generated surface. */
static JceScriptHost g_host;

JCE_MOCK_EXPORT const JceScriptHost *jce_mock_host(int mode)
{
    memset(&g_host, 0, sizeof g_host);
    g_host.log = mock_log;
    if (mode == JCE_MOCK_MODE_ABSENT)
        return &g_host;
    g_host.get_position = mock_get_position;
    g_host.set_position = mock_set_position;
    g_host.get_rotation = mock_get_rotation;
    g_host.set_rotation = mock_set_rotation;
    g_host.get_scale = mock_get_scale;
    g_host.get_world_position = mock_get_world_position;
    g_host.set_scale = mock_set_scale;
    g_host.set_parent = mock_set_parent;
    g_host.get_parent = mock_get_parent;
    g_host.is_key_down = mock_is_key_down;
    g_host.find_with_tag = mock_find_with_tag;
    g_host.destroy_entity = mock_destroy_entity;
    g_host.spawn = mock_spawn;
    g_host.move_axis = mock_move_axis;
    g_host.input_button = mock_input_button;
    g_host.set_time_scale = mock_set_time_scale;
    g_host.set_paused = mock_set_paused;
    g_host.shake_camera = mock_shake_camera;
    g_host.music_set_intensity = mock_music_set_intensity;
    g_host.music_get_intensity = mock_music_get_intensity;
    g_host.music_request_transition = mock_music_request_transition;
    g_host.gas_activate = mock_gas_activate;
    g_host.gas_get = mock_gas_get;
    g_host.gas_apply = mock_gas_apply;
    g_host.raycast = mock_raycast;
    g_host.raycast_filtered = mock_raycast_filtered;
    g_host.raycast_all = mock_raycast_all;
    g_host.apply_impulse = mock_apply_impulse;
    g_host.set_velocity = mock_set_velocity;
    g_host.anim_set_float = mock_anim_set_float;
    g_host.anim_set_int = mock_anim_set_int;
    g_host.anim_set_bool = mock_anim_set_bool;
    g_host.anim_set_trigger = mock_anim_set_trigger;
    g_host.action_down = mock_action_down;
    g_host.action_pressed = mock_action_pressed;
    g_host.action_axis = mock_action_axis;
    g_host.pointer_delta = mock_pointer_delta;
    g_host.pointer_wheel = mock_pointer_wheel;
    g_host.pointer_button = mock_pointer_button;
    g_host.touch_count = mock_touch_count;
    g_host.touch_get = mock_touch_get;
    g_host.loc_translate = mock_loc_translate;
    g_host.loc_get_locale = mock_loc_get_locale;
    g_host.loc_set_locale = mock_loc_set_locale;
    g_host.get_velocity = mock_get_velocity;
    g_host.vehicle_set_input = mock_vehicle_set_input;
    g_host.vehicle_get_speed = mock_vehicle_get_speed;
    g_host.get_move = mock_get_move;
    g_host.ui_get_slider = mock_ui_get_slider;
    g_host.ui_set_slider = mock_ui_set_slider;
    g_host.ui_get_progress = mock_ui_get_progress;
    g_host.ui_set_progress = mock_ui_set_progress;
    g_host.ui_get_toggle = mock_ui_get_toggle;
    g_host.ui_set_toggle = mock_ui_set_toggle;
    g_host.ui_set_text = mock_ui_set_text;
    g_host.send_message = mock_send_message;
    g_host.broadcast = mock_broadcast;
    g_host.has_component = mock_has_component;
    g_host.is_component_enabled = mock_is_component_enabled;
    g_host.set_component_enabled = mock_set_component_enabled;
    g_host.net_is_server = mock_net_is_server;
    g_host.net_is_client = mock_net_is_client;
    g_host.net_spawn = mock_net_spawn;
    g_host.rpc_send = mock_rpc_send;
    g_host.particle_burst = mock_particle_burst;
    g_host.particle_set_emitting = mock_particle_set_emitting;
    g_host.particle_set_color = mock_particle_set_color;
    g_host.find_by_name = mock_find_by_name;
    g_host.find_by_prefix = mock_find_by_prefix;
    g_host.comp_get_json = mock_comp_get_json;
    g_host.comp_set_json = mock_comp_set_json;
    g_host.render_get_json = mock_render_get_json;
    g_host.render_set_json = mock_render_set_json;
    g_host.audio_set_volume = mock_audio_set_volume;
    g_host.ui_get_dropdown = mock_ui_get_dropdown;
    g_host.ui_set_dropdown = mock_ui_set_dropdown;
    g_host.ui_get_input_text = mock_ui_get_input_text;
    g_host.ui_set_input_text = mock_ui_set_input_text;
    g_host.ui_get_scroll = mock_ui_get_scroll;
    g_host.ui_set_scroll = mock_ui_set_scroll;
    g_host.world_get_hour = mock_world_get_hour;
    g_host.world_set_hour = mock_world_set_hour;
    g_host.world_is_daytime = mock_world_is_daytime;
    g_host.world_get_weather = mock_world_get_weather;
    g_host.world_get_weather_intensity = mock_world_get_weather_intensity;
    g_host.world_get_wind_speed = mock_world_get_wind_speed;
    g_host.request_scene = mock_request_scene;
    g_host.is_transitioning = mock_is_transitioning;
    g_host.audio_play = mock_audio_play;
    g_host.audio_stop = mock_audio_stop;
    g_host.audio_is_playing = mock_audio_is_playing;
    g_host.save_game = mock_save_game;
    g_host.load_game = mock_load_game;
    g_host.overlap_sphere = mock_overlap_sphere;
    g_host.overlap_box = mock_overlap_box;
    g_host.get_script_param = mock_get_script_param;
    g_host.get_script_param_text = mock_get_script_param_text;
    g_host.curve_eval = mock_curve_eval;
    g_host.vcam_activate = mock_vcam_activate;
    g_host.json_free = mock_json_free;
    if (mode == JCE_MOCK_MODE_NORELEASE) {
        g_host.json_free = NULL;
    }
    return &g_host;
}
