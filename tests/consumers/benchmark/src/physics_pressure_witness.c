/* Public-runtime physics observations; never creates or steps another world. */
#include "physics_pressure_witness.h"
#include <jce/api_core.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define LOG_TAG "benchmark_physics_pressure"
#define PRESSURE_COUNT 2000u

struct BenchPressureBody {
    JceEntity entity;
    JceBodyHandle handle;
    unsigned touched_frame;
    bool present;
};

struct BenchPressureHandle {
    uint32_t handle;
    unsigned index;
};

bool bench_physics_pressure_requested(void)
{
    return getenv("JCE_BENCH_PHYSICS_PRESSURE") != NULL;
}

bool bench_physics_pressure_enabled(void)
{
    const char *off = getenv("JCE_BENCH_PHYSICS_DISABLED");
    return bench_physics_pressure_requested() && !(off && !strcmp(off, "1"));
}

static int compare_handle(const void *a, const void *b)
{
    const BenchPressureHandle *x = a, *y = b;
    return x->handle < y->handle ? -1 : x->handle > y->handle;
}

static void touched(BenchPhysicsPressure *s, JceBodyHandle handle)
{
    unsigned lo = 0, hi = s->count;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (s->handles[mid].handle < handle.idx)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < s->count && s->handles[lo].handle == handle.idx)
        s->bodies[s->handles[lo].index].touched_frame = s->frame;
}

static void contact(const JceContactEvent *event, void *user)
{
    BenchPhysicsPressure *s = user;
    if (event->is_trigger) return;
    if (event->type == JCE_CONTACT_END) {
        ++s->end_events;
        return;
    }
    if (event->type == JCE_CONTACT_BEGIN)
        ++s->begin_events;
    else if (event->type == JCE_CONTACT_STAY)
        ++s->persist_events;
    touched(s, event->body_a);
    touched(s, event->body_b);
}

static void collect(JceScene *scene, JceEntity entity, void *user)
{
    BenchPhysicsPressure *s = user;
    const JceRigidBodyComponent *rb = jce_scene_get_rigidbody(scene, entity);
    if (!rb) return;
    ++s->rigidbodies;
    const char *name = jce_scene_entity_name(scene, entity);
    if (!name || strncmp(name, "PBody", 5)) {
        ++s->statics;
        return;
    }
    char *end = NULL;
    unsigned long index = strtoul(name + 5, &end, 10);
    if (!end || *end || index >= s->count || s->bodies[index].present) {
        s->valid = false;
        return;
    }
    BenchPressureBody *body = &s->bodies[index];
    body->entity = entity;
    body->handle.idx = rb->body_handle_idx;
    body->present = true;
    if (s->enabled && (!jce_body_valid(body->handle) ||
                      !jce_physics_body_is_dynamic(s->world, body->handle))) {
        if (s->valid)
            LOG_ERROR(LOG_TAG,
                      "physics-pressure-setup: invalid dynamic name=%s handle=%u valid=%u dynamic=%u",
                      name, body->handle.idx, jce_body_valid(body->handle) ? 1u : 0u,
                      jce_physics_body_is_dynamic(s->world, body->handle) ? 1u : 0u);
        s->valid = false;
    }
    ++s->collected;
}

bool bench_physics_pressure_initialize(BenchPhysicsPressure *s,
                                      JceScene *scene, JceRuntime *runtime)
{
    if (!bench_physics_pressure_requested()) return true;
    s->requested = true;
    s->scene = scene;
    s->kind = getenv("JCE_BENCH_PHYSICS_PRESSURE");
    if (strcmp(s->kind, "boxes") && strcmp(s->kind, "spheres") &&
        strcmp(s->kind, "mixed") && strcmp(s->kind, "falling"))
        return false;
    const char *disabled = getenv("JCE_BENCH_PHYSICS_DISABLED");
    const char *observe = getenv("JCE_BENCH_PHYSICS_OBSERVE");
    if ((disabled && strcmp(disabled, "0") && strcmp(disabled, "1")) ||
        !observe || (strcmp(observe, "0") && strcmp(observe, "1")))
        return false;
    s->enabled = bench_physics_pressure_enabled();
    s->observe = !strcmp(observe, "1");
    s->count = PRESSURE_COUNT;
    s->output = getenv("JCE_BENCH_PHYSICS_RECORDS");
    if (s->observe && (!s->output || !*s->output)) return false;
    s->world = jce_runtime_physics(runtime);
    LOG_INFO(LOG_TAG,
             "physics-pressure-setup: kind=%s enabled=%u observer=%u registry_rigidbody=%d registry_box=%d registry_sphere=%d world=%u",
             s->kind, s->enabled ? 1u : 0u, s->observe ? 1u : 0u,
             jce_component_find("Rigidbody"), jce_component_find("BoxCollider"),
             jce_component_find("SphereCollider"), s->world != NULL ? 1u : 0u);
    s->valid = s->enabled ? s->world != NULL : s->world == NULL;
    s->bodies = jce_malloc(s->count * sizeof(*s->bodies));
    s->handles = jce_malloc(s->count * sizeof(*s->handles));
    if (s->bodies) memset(s->bodies, 0, s->count * sizeof(*s->bodies));
    if (s->handles) memset(s->handles, 0, s->count * sizeof(*s->handles));
    if (!s->bodies || !s->handles || !s->valid) {
        LOG_ERROR(LOG_TAG,
                  "physics-pressure-setup: allocation or world-role failure bodies=%u handles=%u enabled=%u world=%u",
                  s->bodies != NULL ? 1u : 0u, s->handles != NULL ? 1u : 0u,
                  s->enabled ? 1u : 0u, s->world != NULL ? 1u : 0u);
        return false;
    }
    jce_scene_each_entity(scene, collect, s);
    LOG_INFO(LOG_TAG,
             "physics-pressure-setup: loaded dynamic=%u rigidbodies=%u static=%u valid=%u initial_bodies=%u",
             s->collected, s->rigidbodies, s->statics, s->valid ? 1u : 0u,
             s->world ? jce_physics_body_count(s->world) : 0);
    unsigned expected_static = strcmp(s->kind, "falling") ? 1u : 0u;
    if (!s->valid || s->collected != s->count ||
        s->statics != expected_static || s->rigidbodies != s->count + expected_static) {
        LOG_ERROR(LOG_TAG, "physics-pressure-setup: loaded component population mismatch");
        return false;
    }
    for (unsigned i = 0; i < s->count; ++i) {
        s->handles[i].handle = s->bodies[i].handle.idx;
        s->handles[i].index = i;
    }
    qsort(s->handles, s->count, sizeof(*s->handles), compare_handle);
    if (s->enabled)
        for (unsigned i = 1; i < s->count; ++i)
            if (s->handles[i - 1].handle == s->handles[i].handle)
                return false;
    if (s->observe && s->enabled) {
        s->listener = jce_physics_add_contact_listener(s->world, contact, s);
        if (!s->listener) return false;
    }
    /* Static colliders may be deferred by the normal runtime proximity policy.
     * Actual body-count authority is checked after its first regular step. */
    return true;
}

void bench_physics_pressure_before(BenchPhysicsPressure *s, unsigned frame)
{
    if (s->requested) s->frame = frame;
}

bool bench_physics_pressure_after(BenchPhysicsPressure *s, unsigned frame)
{
    if (!s->requested) return true;
    if (frame == 1) {
        unsigned actual = s->world ? jce_physics_body_count(s->world) : 0;
        unsigned expected = s->enabled ? s->rigidbodies : 0;
        if (actual != expected) {
            LOG_ERROR(LOG_TAG,
                      "physics-pressure-setup: first-step body-count mismatch actual=%u expected=%u",
                      actual, expected);
            return false;
        }
        LOG_INFO(LOG_TAG,
                 "physics-pressure-inventory: frame=1 kind=%s dynamic=%u rigidbodies=%u static=%u "
                 "actual_bodies=%u enabled=%u observer=%u fixed_hz=60 gravity=-9.81 "
                 "solver_iterations=10 ccd=discrete sleep=unavailable",
                 s->kind, s->collected, s->rigidbodies, s->statics,
                 s->world ? jce_physics_body_count(s->world) : 0,
                 s->enabled ? 1u : 0u, s->observe ? 1u : 0u);
    }
    if (!s->observe || (frame != 1 && frame != 120 && frame != 600 && frame != 900))
        return true;
    JceJson *root = jce_json_object(), *bodies = jce_json_array();
    if (!root || !bodies) {
        jce_json_free(root);
        jce_json_free(bodies);
        return false;
    }
    bool correct = true;
    unsigned stationary = 0, current_contact = 0;
    jce_json_set_number(root, "frame", frame);
    jce_json_set_string(root, "kind", s->kind);
    jce_json_set_bool(root, "enabled", s->enabled);
    jce_json_set_number(root, "actual_bodies", s->world ? jce_physics_body_count(s->world) : 0);
    jce_json_set_number(root, "begin_events", (double)s->begin_events);
    jce_json_set_number(root, "persist_events", (double)s->persist_events);
    jce_json_set_number(root, "end_events", (double)s->end_events);
    jce_json_set_string(root, "sleep_transition", "UNAVAILABLE_PUBLIC_API");
    for (unsigned i = 0; i < s->count; ++i) {
        BenchPressureBody *body = &s->bodies[i];
        jce_vec3 pos, vel = jce_v3(0, 0, 0), angular = jce_v3(0, 0, 0);
        jce_quat rot;
        const JceTransform *transform = jce_scene_get_transform(s->scene, body->entity);
        if (!transform) {
            correct = false;
            break;
        }
        pos = transform->position;
        rot = transform->rotation;
        if (s->enabled) {
            jce_physics_body_get_transform(s->world, body->handle, &pos, &rot);
            vel = jce_physics_body_get_velocity(s->world, body->handle);
            angular = jce_physics_body_get_angular_velocity(s->world, body->handle);
        }
        if (!isfinite(pos.x) || !isfinite(pos.y) || !isfinite(pos.z) ||
            !isfinite(vel.x) || !isfinite(vel.y) || !isfinite(vel.z)) {
            correct = false;
            break;
        }
        bool quiet = jce_v3_len(vel) < .05f && jce_v3_len(angular) < .05f;
        stationary += quiet ? 1u : 0u;
        current_contact += body->touched_frame == frame ? 1u : 0u;
        JceJson *row = jce_json_object();
        if (!row) {
            correct = false;
            break;
        }
        jce_json_set_number(row, "index", i);
        char entity_text[32];
        snprintf(entity_text, sizeof(entity_text), "%llu", (unsigned long long)body->entity);
        jce_json_set_string(row, "entity", entity_text);
        jce_json_set_number(row, "handle", body->handle.idx);
        JceJson *p = jce_json_array(), *v = jce_json_array();
        JceJson *a = jce_json_array(), *q = jce_json_array();
        if (!p || !v || !a || !q) {
            jce_json_free(row);
            jce_json_free(p);
            jce_json_free(v);
            jce_json_free(a);
            jce_json_free(q);
            correct = false;
            break;
        }
        jce_json_array_push_number(p, pos.x);
        jce_json_array_push_number(p, pos.y);
        jce_json_array_push_number(p, pos.z);
        jce_json_array_push_number(v, vel.x);
        jce_json_array_push_number(v, vel.y);
        jce_json_array_push_number(v, vel.z);
        jce_json_array_push_number(a, angular.x);
        jce_json_array_push_number(a, angular.y);
        jce_json_array_push_number(a, angular.z);
        jce_json_array_push_number(q, rot.x);
        jce_json_array_push_number(q, rot.y);
        jce_json_array_push_number(q, rot.z);
        jce_json_array_push_number(q, rot.w);
        jce_json_set_child(row, "position", p);
        jce_json_set_child(row, "velocity", v);
        jce_json_set_child(row, "angular_velocity", a);
        jce_json_set_child(row, "rotation", q);
        jce_json_set_bool(row, "contact_this_step", body->touched_frame == frame);
        jce_json_array_push(bodies, row);
    }
    jce_json_set_number(root, "stationary_velocity_proxy", stationary);
    jce_json_set_number(root, "contacted_this_step", current_contact);
    jce_json_set_child(root, "bodies", bodies);
    jce_json_set_bool(root, "finite", correct);
    char *text = jce_json_print(root, false);
    bool written = false;
    if (text) {
        size_t length = strlen(text);
        text[length] = '\n';
        written = frame == 1 ? jce_fs_host_write_all(s->output, text, length + 1)
                             : jce_fs_host_append(s->output, text, length + 1);
    }
    jce_json_free_string(text);
    jce_json_free(root);
    LOG_INFO(LOG_TAG,
             "physics-pressure-state: frame=%u bodies=%u finite=%u "
             "stationary_proxy=%u contacts_this_step=%u saved=%u",
             frame, s->world ? jce_physics_body_count(s->world) : 0,
             correct ? 1u : 0u, stationary, current_contact, written ? 1u : 0u);
    return correct && written;
}

void bench_physics_pressure_destroy(BenchPhysicsPressure *s)
{
    if (s->listener && s->world)
        jce_physics_remove_contact_listener(s->world, contact, s);
    jce_free(s->bodies);
    jce_free(s->handles);
    memset(s, 0, sizeof(*s));
}
