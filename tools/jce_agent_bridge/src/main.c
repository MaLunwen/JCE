/*
 * jce_agent_bridge -- the engine side of the agent layer, as one SDK consumer.
 *
 * WHY A BINARY AND NOT A PYTHON MODULE.  Three questions an agent has to be
 * able to ask can only be answered by the engine itself:
 *
 *   compile-recipe   does this bounded recipe compile, and to what plan?
 *                    The compiler is deterministic C (jce_scene_compile), and
 *                    the whole point of REQ-SCN-02 / ADR-06 is that the
 *                    DESIGN-TIME answer comes from the same compiler the
 *                    runtime uses.  A Python reimplementation would be a
 *                    second compiler, and "the agent designed something the
 *                    runtime materialises differently" is exactly what the
 *                    single-compiler rule exists to prevent.
 *
 *   physics-probe    does this scene behave?  Bullet and Box2D are the only
 *                    things entitled to answer (ADR-03, CN-03), so this steps
 *                    the REAL solver through the REAL runtime -- which is also
 *                    what wires scene components to bodies.  Building a second
 *                    scene-to-physics path here to measure the first one would
 *                    measure the second one.
 *
 *   introspect       what does the engine accept?  (api_introspect.h.)
 *
 * WHY AN SDK CONSUMER.  It compiles against the PACKAGED headers and links the
 * PACKAGED library, like tests/sdk_smoke and unlike anything built in-tree.
 * That is the only arrangement that shows these capabilities survive SDK
 * installation -- this repository has shipped a header that worked in-tree and
 * broke every consumer on the first line of api.h.  It also means this builds
 * into its OWN build directory and never contends for the shared one.
 *
 * THE PROBE MEASURES THE SCENE, NOT THE SOLVER'S INTERNALS.  There is no public
 * way to enumerate body handles, and inventing one would be a new ABI for a
 * tool's convenience.  The runtime writes each step's result back into every
 * entity's Transform, so positions are read from there and velocities are
 * finite-differenced from them; penetration comes from the contact callback,
 * which carries depth and needs no enumeration.  That is also the more honest
 * measurement: it is what the scene, the renderer and the player see.
 *
 * ARGUMENTS ARRIVE IN THE ENVIRONMENT because JCE_MAIN owns main(), and
 * because it is the convention every other engine-driving tool in this tree
 * already uses (JCE_STARTUP_SCENE, JCE_CAPTURE_FRAME, JCE_MAX_FRAMES):
 *
 *   JCE_BRIDGE_VERB       introspect | compile-recipe | physics-probe
 *   JCE_BRIDGE_OUT        where the JSON result goes (required)
 *   JCE_BRIDGE_ARG        subject / recipe path / scene path
 *   JCE_BRIDGE_DURATION   probe seconds (default 10)
 *   JCE_BRIDGE_SETTLE_EPS speed below which a body counts as settled (m/s)
 *
 * Exit code is 0 when the VERB RAN, whatever it found.  "The scene penetrates"
 * is a result, not a failure of this program, and a caller must be able to
 * tell those apart.
 */

#include <jce/api.h>
#include <jce/application/jce_main.h>
#include <jce/jce_version.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BRIDGE_MAX_BODIES 4096

static const char *env_or(const char *k, const char *d)
{
    const char *v = getenv(k);
    return (v && v[0]) ? v : d;
}

static double env_num(const char *k, double d)
{
    const char *v = getenv(k);
    if (!v || !v[0])
        return d;
    return atof(v);
}

/* ── output ──────────────────────────────────────────────────────── */

static void emit(JceJson *root)
{
    const char *out = env_or("JCE_BRIDGE_OUT", "");
    char *text = jce_json_print(root, true);

    jce_json_free(root);
    if (!text)
        return;
    if (out[0]) {
        /* A FILE, never stdout.  stdout carries the engine's own startup log,
         * so a caller reading a JSON document from it would have to find the
         * document inside a log stream -- which is a parser that works until
         * the engine logs something new. */
        FILE *f = fopen(out, "wb");
        if (f) {
            fwrite(text, 1, strlen(text), f);
            fputc('\n', f);
            fclose(f);
        }
    } else {
        printf("%s\n", text);
    }
    jce_json_free_string(text);
    fflush(stdout);
}

static void emit_error(const char *verb, const char *message)
{
    JceJson *root = jce_json_object();
    jce_json_set_string(root, "verb", verb ? verb : "");
    jce_json_set_bool(root, "ok", false);
    jce_json_set_string(root, "error", message ? message : "");
    emit(root);
}

/* ── introspect ──────────────────────────────────────────────────── */

static void verb_introspect(void)
{
    const char *subject = env_or("JCE_BRIDGE_ARG", "components");
    size_t need;
    char *buf;

    if (strcmp(subject, "components") == 0)
        need = jce_introspect_components_json(NULL, 0);
    else if (strcmp(subject, "stats") == 0)
        need = jce_introspect_stats_json(NULL, NULL, 0);
    else {
        emit_error("introspect", "unknown subject (components|stats)");
        return;
    }
    buf = (char *)malloc(need + 1u);
    if (!buf) {
        emit_error("introspect", "out of memory");
        return;
    }
    if (strcmp(subject, "components") == 0)
        jce_introspect_components_json(buf, need + 1u);
    else
        jce_introspect_stats_json(NULL, buf, need + 1u);

    {
        const char *out = env_or("JCE_BRIDGE_OUT", "");
        if (out[0]) {
            FILE *f = fopen(out, "wb");
            if (f) { fwrite(buf, 1, strlen(buf), f); fputc('\n', f); fclose(f); }
        } else {
            printf("%s\n", buf);
        }
    }
    free(buf);
}

/* ── compile-recipe ──────────────────────────────────────────────── */

static char *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *buf;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    buf = (char *)malloc((size_t)n + 1u);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    buf[n] = '\0';
    if (out_len)
        *out_len = (size_t)n;
    return buf;
}

static const char *recipe_status_name(JceSceneRecipeStatus s)
{
    switch (s) {
    case JCE_SCENE_RECIPE_OK:                  return "OK";
    case JCE_SCENE_RECIPE_INVALID_ARGUMENT:    return "INVALID_ARGUMENT";
    case JCE_SCENE_RECIPE_INVALID_JSON:        return "INVALID_JSON";
    case JCE_SCENE_RECIPE_UNKNOWN_FIELD:       return "UNKNOWN_FIELD";
    case JCE_SCENE_RECIPE_UNSUPPORTED_VERSION: return "UNSUPPORTED_VERSION";
    case JCE_SCENE_RECIPE_CAPACITY_EXCEEDED:   return "CAPACITY_EXCEEDED";
    case JCE_SCENE_RECIPE_INVALID_TOKEN:       return "INVALID_TOKEN";
    case JCE_SCENE_RECIPE_DUPLICATE_ROLE:      return "DUPLICATE_ROLE";
    case JCE_SCENE_RECIPE_INVALID_RANGE:       return "INVALID_RANGE";
    case JCE_SCENE_RECIPE_INVALID_NUMBER:      return "INVALID_NUMBER";
    case JCE_SCENE_RECIPE_INVALID_HIERARCHY:   return "INVALID_HIERARCHY";
    case JCE_SCENE_RECIPE_INVALID_CATALOG:     return "INVALID_CATALOG";
    }
    return "UNKNOWN";
}

static const char *compile_status_name(JceSceneCompileStatus s)
{
    switch (s) {
    case JCE_SCENE_COMPILE_OK:                 return "OK";
    case JCE_SCENE_COMPILE_INVALID_ARGUMENT:   return "INVALID_ARGUMENT";
    case JCE_SCENE_COMPILE_INVALID_RECIPE:     return "INVALID_RECIPE";
    case JCE_SCENE_COMPILE_INVALID_CATALOG:    return "INVALID_CATALOG";
    case JCE_SCENE_COMPILE_UNKNOWN_CAPABILITY: return "UNKNOWN_CAPABILITY";
    case JCE_SCENE_COMPILE_OPERATION_CAPACITY: return "OPERATION_CAPACITY";
    case JCE_SCENE_COMPILE_PLACEMENT_FAILED:   return "PLACEMENT_FAILED";
    case JCE_SCENE_COMPILE_INVALID_HIERARCHY:  return "INVALID_HIERARCHY";
    }
    return "UNKNOWN";
}

/* The catalog: what the compiler is allowed to place.
 *
 * Read from JCE_BRIDGE_CATALOG when given.  Otherwise SYNTHESISED from the
 * recipe's own capabilities -- one enabled entry per distinct capability --
 * so that a caller who wants to know "does this recipe compile at all" gets
 * that answer without first having to author an asset catalog.  It is reported
 * as synthesised in the result, because a plan compiled against a made-up
 * catalog names made-up assets: the placement arithmetic is real, the asset
 * ids are not, and nothing downstream may treat them as real. */
static bool build_catalog(const JceSceneRecipe *recipe, const char *path,
                          JceSceneCatalog *out, bool *out_synth,
                          const char **out_err)
{
    jce_scene_catalog_init(out);
    *out_synth = false;
    if (path && path[0]) {
        size_t len = 0;
        char *text = read_file(path, &len);
        JceSceneRecipeError err;
        JceSceneRecipeStatus st;
        if (!text) { *out_err = "catalog file unreadable"; return false; }
        st = jce_scene_catalog_parse_json(text, len, out, &err);
        free(text);
        if (st != JCE_SCENE_RECIPE_OK) {
            *out_err = recipe_status_name(st);
            return false;
        }
        return true;
    }
    *out_synth = true;
    for (uint32_t i = 0; i < recipe->role_count; ++i) {
        const char *cap = recipe->roles[i].capability;
        bool seen = false;
        uint32_t j;
        for (j = 0; j < out->entry_count; ++j)
            if (strcmp(out->entries[j].capability, cap) == 0) { seen = true; break; }
        if (seen || out->entry_count >= JCE_SCENE_CATALOG_MAX_ENTRIES)
            continue;
        {
            JceSceneCatalogEntry *e = &out->entries[out->entry_count++];
            memset(e, 0, sizeof(*e));
            snprintf(e->asset_id, sizeof(e->asset_id), "synthetic.%s", cap);
            snprintf(e->capability, sizeof(e->capability), "%s", cap);
            /* NON-ZERO, and DERIVED FROM THE CAPABILITY.  Non-zero because
             * jce_scene_catalog_validate refuses a zero content hash at both
             * the entry and the catalog level -- a zero hash means "identity
             * unknown", and a placement compiled against an unknown asset is
             * not reproducible.  Derived rather than counted so that the same
             * recipe synthesises the same catalog every time: the plan hash is
             * a function of the catalog, so a counter here would make two
             * compilations of one recipe disagree, which is the one property
             * the FrozenPlan exists to have.  FNV-1a, inline, because pulling
             * a hash dependency into a tool for eight lines would be worse. */
            uint64_t h = 1469598103934665603ull;
            const unsigned char *p = (const unsigned char *)e->asset_id;
            while (*p) { h ^= *p++; h *= 1099511628211ull; }
            e->content_hash = h ? h : 1ull;
            e->weight = 1u;
            e->enabled = true;
        }
    }
    out->schema_version = JCE_SCENE_CATALOG_SCHEMA_VERSION;
    {
        /* The catalog's own hash, over its entries' hashes in order.  Also
         * non-zero by the same rule, and also deterministic. */
        uint64_t h = 1469598103934665603ull;
        uint32_t i;
        for (i = 0; i < out->entry_count; ++i) {
            uint64_t v = out->entries[i].content_hash;
            int b;
            for (b = 0; b < 8; ++b) {
                h ^= (unsigned char)((v >> (b * 8)) & 0xFFu);
                h *= 1099511628211ull;
            }
        }
        out->content_hash = h ? h : 1ull;
    }
    return true;
}

static void verb_compile_recipe(void)
{
    const char *path = env_or("JCE_BRIDGE_ARG", "");
    JceJson *root = jce_json_object();
    size_t len = 0;
    char *text;
    JceSceneRecipe recipe;
    JceSceneCatalog catalog;
    JceSceneRecipeError rerr;
    JceSceneCompileError cerr;
    JceSceneCompileOptions opts;
    JceSceneFrozenPlan plan;
    JceSceneRecipeStatus rst;
    JceSceneCompileStatus cst;
    bool synth = false;
    const char *cat_err = NULL;

    jce_json_set_string(root, "verb", "compile-recipe");
    jce_json_set_string(root, "engine_version", jce_api_version_string());
    jce_json_set_int(root, "compiler_version", (int)JCE_SCENE_COMPILER_VERSION);
    jce_json_set_int(root, "recipe_schema_version",
                     (int)JCE_SCENE_RECIPE_SCHEMA_VERSION);

    if (!path[0]) { jce_json_set_bool(root, "ok", false);
                    jce_json_set_string(root, "error", "no recipe path");
                    emit(root); return; }
    text = read_file(path, &len);
    if (!text) { jce_json_set_bool(root, "ok", false);
                 jce_json_set_string(root, "error", "recipe file unreadable");
                 emit(root); return; }

    jce_scene_recipe_init(&recipe);
    rst = jce_scene_recipe_parse_json(text, len, &recipe, &rerr);
    free(text);
    jce_json_set_string(root, "parse_status", recipe_status_name(rst));
    if (rst != JCE_SCENE_RECIPE_OK) {
        /* The engine's STRICT parser refused it.  Its field name is carried
         * through verbatim: "the recipe is invalid" is not actionable and
         * "roles[2].placement" is. */
        jce_json_set_bool(root, "ok", false);
        jce_json_set_string(root, "error_field", rerr.field);
        jce_json_set_int(root, "error_item_index", (int)rerr.item_index);
        emit(root);
        return;
    }
    jce_json_set_int(root, "role_count", (int)recipe.role_count);

    if (!build_catalog(&recipe, getenv("JCE_BRIDGE_CATALOG"), &catalog,
                       &synth, &cat_err)) {
        jce_json_set_bool(root, "ok", false);
        jce_json_set_string(root, "error", cat_err ? cat_err : "bad catalog");
        emit(root);
        return;
    }
    jce_json_set_bool(root, "catalog_synthesised", synth);
    if (synth)
        jce_json_set_string(root, "catalog_note",
                            "no catalog was given, so one was synthesised with "
                            "a single entry per capability. The placement "
                            "arithmetic is real; the asset ids are NOT, and "
                            "nothing may treat them as real assets.");
    jce_json_set_int(root, "catalog_entries", (int)catalog.entry_count);

    jce_scene_compile_options_default(&opts);
    memset(&plan, 0, sizeof(plan));
    cst = jce_scene_compile(&recipe, &catalog, &opts, &plan, &cerr);
    jce_json_set_string(root, "compile_status", compile_status_name(cst));
    if (cst != JCE_SCENE_COMPILE_OK) {
        jce_json_set_bool(root, "ok", false);
        jce_json_set_int(root, "error_role_index", (int)cerr.role_index);
        jce_json_set_int(root, "error_attempts", (int)cerr.attempt_count);
        emit(root);
        return;
    }

    jce_json_set_bool(root, "ok", true);
    jce_json_set_int(root, "operation_count", (int)plan.operation_count);
    /* The plan hash as TEXT.  It is 64 bits and JSON numbers are doubles:
     * emitting it as a number silently loses the low bits, and a hash that is
     * almost right is worse than no hash because it compares unequal for a
     * reason nobody can see. */
    {
        char hex[32];
        snprintf(hex, sizeof hex, "%016llx",
                 (unsigned long long)jce_scene_frozen_plan_hash(&plan));
        jce_json_set_string(root, "plan_hash", hex);
        snprintf(hex, sizeof hex, "%016llx", (unsigned long long)plan.seed);
        jce_json_set_string(root, "seed", hex);
    }
    jce_json_set_bool(root, "graph_valid",
                      jce_scene_frozen_plan_graph_validate(&plan));

    {
        JceJson *ops = jce_json_array();
        uint32_t i;
        for (i = 0; i < plan.operation_count && i < 64u; ++i) {
            const JceScenePlanOperation *o = &plan.operations[i];
            JceJson *row = jce_json_object();
            char idhex[32];
            jce_json_set_string(row, "stable_role", o->stable_role);
            jce_json_set_string(row, "asset_id", o->asset_id);
            snprintf(idhex, sizeof idhex, "%016llx",
                     (unsigned long long)o->stable_entity_id);
            jce_json_set_string(row, "stable_entity_id", idhex);
            /* Quantised integers, exactly as the plan carries them: the whole
             * point of the FrozenPlan is that the numbers are fixed-point and
             * reproduce bit for bit, and printing them as floats would undo
             * that at the last step. */
            jce_json_set_int(row, "position_mm_x", o->position_mm[0]);
            jce_json_set_int(row, "position_mm_y", o->position_mm[1]);
            jce_json_set_int(row, "position_mm_z", o->position_mm[2]);
            jce_json_set_int(row, "rotation_mdeg_y", o->rotation_mdeg[1]);
            jce_json_set_int(row, "scale_milli_x", o->scale_milli[0]);
            jce_json_array_push(ops, row);
        }
        jce_json_set_child(root, "operations", ops);
        if (plan.operation_count > 64u)
            jce_json_set_int(root, "operations_elided",
                             (int)(plan.operation_count - 64u));
    }
    emit(root);
}

/* ── physics-probe ───────────────────────────────────────────────── */

typedef struct {
    JceEntity e;
    float     mass;
    jce_vec3  prev;
    jce_vec3  vel;
    bool      have_prev;
    /* Rebound: the lowest point the body has reached, and the highest it has
     * reached SINCE.  That difference is what "does the crate bounce" means,
     * and it is the only one of these quantities that restitution moves --
     * penetration is impact-depth, settle time is a speed threshold, jitter is
     * post-settle residue.  Measured: restitution 0.0 against 0.9 left all
     * three byte-identical, which reads as "the knob is dead" when what it
     * actually meant was that nothing here was looking at the knob's effect.
     * §6.4's own worked example is "a 30 kg crate that does not bounce
     * absurdly", so this is the quantity that acceptance is written in. */
    float     y_min;
    float     y_max_after_min;
    bool      have_min;
    /* Rollover: the largest angle this body's own up axis ever made with the
     * world's.  §6.4 grades a vehicle on "no rollover", and none of the other
     * metrics can see it -- a car lying on its roof has settled, has no
     * penetration, no NaN and no jitter.  It is a perfectly healthy
     * simulation of a crashed car. */
    float     max_tilt_deg;
} ProbeBody;

typedef struct {
    ProbeBody bodies[BRIDGE_MAX_BODIES];
    int       count;
    double    max_penetration;
    long      contact_events;
    long      trigger_contacts;
    long      nan_events;
    double    settle_time;
    bool      settled;
    double    energy_first;
    double    energy_last;
    double    jitter_accum;
    long      jitter_samples;
    /* Jitter as a SPEED, not as an energy.
     *
     * Kinetic energy scales with mass, so a fixed energy budget flags every
     * heavy object and passes every light one: the 1003 kg vehicle settles to
     * 7.449 J of residual suspension motion against a 0.05 budget written for
     * a 30 kg crate -- 33x the mass, and the threshold said the car was
     * broken.  Speed is mass-independent and is also what the reader means:
     * "is it still moving, and how fast".  The energy stays reported beside
     * it because it is what the drift ratio is computed from. */
    double    jitter_speed_accum;
} Probe;

static Probe g_probe;

static void on_contact(const JceContactEvent *ev, void *ud)
{
    (void)ud;
    if (!ev)
        return;
    g_probe.contact_events++;
    /* A TRIGGER OVERLAP IS NOT PENETRATION, AND COUNTING IT AS SUCH FAILED
     * EVERY SCENE THAT HAS A TRIGGER IN IT.  A trigger reports contacts and
     * the solver deliberately does not push the bodies apart, so the overlap
     * grows to whatever the volume's size is and simply stays there -- that
     * is the feature working.  This callback recorded its depth anyway, and
     * `ev->is_trigger` has been on JceContactEvent all along, populated the
     * whole way down (scene JSON `isTrigger` -> JceBoxColliderDesc ->
     * jce_physics.c -> the event); nothing here ever read it.
     *
     * Measured on the north-star scene, which authors a door with a trigger
     * volume around it -- the most ordinary thing a trigger is for:
     *
     *     no trigger                  max_penetration_m 0.02948
     *     the same box, isTrigger=1   max_penetration_m 0.52945   verdict FAIL
     *     the same box, isTrigger=0   max_penetration_m 0.00273
     *
     * The middle row is the one that is wrong, and it is wrong in the
     * direction that makes a HEALTHY scene fail: marking the box solid, so
     * the solver really does resolve it, gives the SMALLEST number of the
     * three.  The verdict was anti-correlated with the physics.
     *
     * The count is kept and reported rather than dropped, because "the probe
     * saw 412 contacts and ignored them" and "there were no contacts" must
     * not look the same in the record. */
    if (ev->is_trigger) {
        g_probe.trigger_contacts++;
        return;
    }
    /* Depth is the manifold's penetration.  Sign convention differs between
     * backends, so magnitude is what is recorded -- a probe that reported
     * "-0.4 m of penetration, within the 0.01 budget" would pass every scene
     * that is broken in the direction the sign happened to take. */
    {
        double d = (double)ev->depth;
        if (d < 0.0) d = -d;
        if (d > g_probe.max_penetration)
            g_probe.max_penetration = d;
    }
}

static void probe_collect(JceScene *s, JceEntity e, void *ud)
{
    Probe *p = (Probe *)ud;
    if (!jce_scene_has_rigidbody(s, e))
        return;
    if (p->count >= BRIDGE_MAX_BODIES)
        return;
    {
        JceRigidBodyComponent *rb = jce_scene_get_rigidbody(s, e);
        ProbeBody *b = &p->bodies[p->count++];
        memset(b, 0, sizeof(*b));
        b->e = e;
        b->mass = (rb && rb->mass > 0.0f) ? rb->mass : 1.0f;
    }
}

static int finite3(jce_vec3 v)
{
    return isfinite(v.x) && isfinite(v.y) && isfinite(v.z);
}

/* ── Ragdoll measurement ──────────────────────────────────────────────
 *
 * A ragdoll's bodies are NOT scene entities, so every metric above is blind to
 * one: probe_collect walks entities carrying a Rigidbody, and a ragdoll entity
 * carries none.  Its bodies live only in the physics world, and the runtime
 * publishes their resolved per-bone LOCAL transforms into the scene's
 * transient relay after each step (rt_ragdoll_sync_to ->
 * jce_scene_set_ragdoll_pose).  That relay is public, so reading it needs no
 * new ABI -- the same rule the rest of this file follows.
 *
 * WHAT IS MEASURED.  The length of each bone's local translation, which at the
 * bind pose is that bone's offset from its parent.  The GENERIC6DOF
 * constraints are meant to hold those offsets; when capsules spawn inside one
 * another the solver pushes them apart and the offsets grow without bound.  So
 * the maximum over every bone and every step separates "a ragdoll" from "a
 * ragdoll leaving the screen", and it becomes dimensionless the moment the
 * caller divides by the bind bone length it already measured.
 *
 * WHY NOT WORLD POSITIONS.  Recovering them means walking the parent chain,
 * and the relay carries no parents -- the bridge would need a second copy of
 * the skeleton.  The local offset is the quantity the constraint actually
 * governs, so it is both cheaper and closer to the thing under test.
 *
 * THE JUDGEMENT IS NOT MADE HERE.  This reports raw maxima.  The bind lengths
 * live in the plan, on the Python side; comparing there keeps the C half from
 * carrying a second skeleton measurement that could disagree with the first.
 *
 * `published_a_pose` IS PART OF THE REPORT, not a detail.  Seven separate
 * conditions gate the ragdoll spawn in jce_runtime.c and every one of them
 * skips SILENTLY.  A scene whose ragdoll never spawned reports zero bones
 * moving, zero NaN and a perfectly stable chain -- which is byte-identical to
 * a ragdoll that behaved impeccably.  Without this flag the healthy reading
 * and the never-ran reading cannot be told apart. */
#define BRIDGE_MAX_RAGDOLLS 16

typedef struct {
    JceEntity e;
    uint32_t  joints;
    double    first_max_offset;   /* the first published pose, as authored  */
    double    max_offset;         /* over every NON-ROOT bone, every step   */
    double    last_max_offset;
    /* The root is measured separately because its "local" transform is its
     * WORLD transform: jce_ragdoll_sync_to_pose writes root = world directly
     * and converts only the children to parent-relative.  Folding it into the
     * maximum reports the ragdoll's HEIGHT as a constraint stretch -- measured
     * here as 1.083 m against a longest bind bone of 0.42 m, which reads as
     * 2.6x stretched on a chain that was behaving perfectly.  Its own useful
     * question is different and worth asking: how far did the body fall. */
    double    root_y_min;
    double    root_y_final;
    bool      root_seen;
    long      nan_bones;
    bool      sampled;
    /* The spawn conditions, read from the SAME scene the runtime read.
     * jce_runtime.c skips the whole ragdoll block when any of these is false
     * and logs nothing, so a caller otherwise has a silent no-op and five
     * candidate causes.  Reported per entity, not summarised: "something was
     * missing" is not actionable and naming it is. */
    bool      comp_enable_field;
    bool      comp_not_disabled;
    bool      has_skeletal_animator;
    char      skeleton_path[256];
} RagdollEnt;

static RagdollEnt g_ragdolls[BRIDGE_MAX_RAGDOLLS];
static int        g_ragdoll_count;

static void ragdoll_collect(JceScene *s, JceEntity e, void *ud)
{
    (void)ud;
    if (g_ragdoll_count >= BRIDGE_MAX_RAGDOLLS)
        return;
    if (!jce_scene_get_ragdoll(s, e))
        return;
    {
        RagdollEnt *re = &g_ragdolls[g_ragdoll_count];
        JceRagdollComponent *rc = jce_scene_get_ragdoll(s, e);
        int cid = jce_component_find("Ragdoll");
        memset(re, 0, sizeof(*re));
        re->e = e;
        re->comp_enable_field = rc && rc->enable;
        re->comp_not_disabled = (cid < 0) || jce_scene_comp_enabled(s, e, cid);
        re->has_skeletal_animator = jce_scene_has_skeletal_animator(s, e);
        if (re->has_skeletal_animator) {
            JceSkeletalAnimatorComponent *sa =
                jce_scene_get_skeletal_animator(s, e);
            if (sa) {
                size_t n = strlen(sa->skeleton_path);
                if (n >= sizeof(re->skeleton_path))
                    n = sizeof(re->skeleton_path) - 1;
                memcpy(re->skeleton_path, sa->skeleton_path, n);
                re->skeleton_path[n] = 0;
            }
        }
        g_ragdoll_count++;
    }
}

/* Sample every ragdoll's relay once.  Called after each step. */
static void ragdoll_sample(JceScene *s)
{
    static jce_mat4 locals[JCE_MAX_BONES];
    int r;
    for (r = 0; r < g_ragdoll_count; ++r) {
        RagdollEnt *re = &g_ragdolls[r];
        uint32_t cnt = 0, j;
        double worst = 0.0;
        if (!jce_scene_get_ragdoll_pose(s, re->e, locals, &cnt))
            continue;
        if (cnt > (uint32_t)JCE_MAX_BONES) cnt = (uint32_t)JCE_MAX_BONES;
        re->joints = cnt;
        for (j = 0; j < cnt; ++j) {
            /* jce_mat4 is a union of raw[column][row] and col[4]; the
             * translation is column 3. */
            jce_vec4 t = locals[j].col[3];
            double d;
            if (!isfinite(t.x) || !isfinite(t.y) || !isfinite(t.z)) {
                re->nan_bones++;
                continue;
            }
            if (j == 0) {                      /* root: local IS world */
                if (!re->root_seen || (double)t.y < re->root_y_min)
                    re->root_y_min = (double)t.y;
                re->root_y_final = (double)t.y;
                re->root_seen = true;
                continue;
            }
            d = sqrt((double)t.x * t.x + (double)t.y * t.y +
                     (double)t.z * t.z);
            if (d > worst) worst = d;
        }
        if (!re->sampled) {
            re->first_max_offset = worst;
            re->sampled = true;
        }
        re->last_max_offset = worst;
        if (worst > re->max_offset) re->max_offset = worst;
    }
}

static void ragdoll_report(JceJson *root)
{
    JceJson *arr;
    int r;
    jce_json_set_int(root, "ragdoll_count", g_ragdoll_count);
    if (g_ragdoll_count == 0)
        return;
    arr = jce_json_array();
    for (r = 0; r < g_ragdoll_count; ++r) {
        RagdollEnt *re = &g_ragdolls[r];
        JceJson *o = jce_json_object();
        jce_json_set_number(o, "entity", (double)re->e);
        jce_json_set_int(o, "joints", (int)re->joints);
        jce_json_set_bool(o, "published_a_pose", re->sampled);
        jce_json_set_number(o, "first_max_local_offset_m",
                            re->first_max_offset);
        jce_json_set_number(o, "max_local_offset_m", re->max_offset);
        jce_json_set_number(o, "final_max_local_offset_m",
                            re->last_max_offset);
        jce_json_set_int(o, "nan_bones", (int)re->nan_bones);
        jce_json_set_bool(o, "enable_field", re->comp_enable_field);
        jce_json_set_bool(o, "component_not_disabled", re->comp_not_disabled);
        jce_json_set_bool(o, "has_skeletal_animator",
                          re->has_skeletal_animator);
        jce_json_set_number(o, "root_y_min_m", re->root_y_min);
        jce_json_set_number(o, "root_y_final_m", re->root_y_final);
        jce_json_set_string(o, "skeleton_path", re->skeleton_path);
        jce_json_array_push(arr, o);
    }
    jce_json_set_child(root, "ragdolls", arr);
}

/* Parameter overrides, applied to every rigid body before the runtime wires
 * it, and REPORTED so the caller can check they landed.
 *
 *   JCE_BRIDGE_OVERRIDES="mass=30,drag=0.1,friction=0.5,restitution=0.2"
 *
 * THE REPORTING IS THE PART THAT MATTERS.  physics.tune searches a parameter
 * space by probing once per candidate.  If an override silently failed to
 * reach the solver, every probe would return identical numbers and the search
 * would report whichever noise it liked best as the optimum -- with a curve, a
 * winner and a score, all of it meaningless, and nothing anywhere saying so.
 * So the applied keys come back and the caller refuses a search whose
 * parameters were not applied.
 *
 * An UNKNOWN key is not counted as applied, deliberately: a typo'd parameter
 * name must not read as a successful override of nothing.
 *
 * Applied to every rigid body rather than to one entity: this serves a search
 * that tunes a material, and an entity selector inside an environment variable
 * would be a parser nobody can see. */

typedef struct {
    const char *key;
    double      value;
    int         bodies;
} OverrideCtx;

static void apply_one(JceScene *s, JceEntity e, void *ud)
{
    OverrideCtx *o = (OverrideCtx *)ud;
    JceRigidBodyComponent *rb;

    if (!jce_scene_has_rigidbody(s, e))
        return;
    rb = jce_scene_get_rigidbody(s, e);
    if (!rb)
        return;
    if      (strcmp(o->key, "mass") == 0)         rb->mass = (float)o->value;
    else if (strcmp(o->key, "drag") == 0)         rb->drag = (float)o->value;
    else if (strcmp(o->key, "angularDrag") == 0)  rb->angular_drag = (float)o->value;
    else if (strcmp(o->key, "friction") == 0)     rb->friction = (float)o->value;
    else if (strcmp(o->key, "restitution") == 0)  rb->restitution = (float)o->value;
    else if (strcmp(o->key, "gravityScale") == 0) rb->gravity_scale = (float)o->value;
    else return;                        /* unknown: NOT counted as applied */
    o->bodies++;
}

/* Split on ',' by hand.  strtok_s is MSVC and strtok_r is POSIX; a tool that
 * is meant to build on three desktop platforms should not pick one. */
static int apply_overrides(JceScene *scene, JceJson *report)
{
    const char *spec = env_or("JCE_BRIDGE_OVERRIDES", "");
    JceJson *applied = jce_json_array();
    int applied_count = 0, bodies = 0;
    size_t i = 0, n = strlen(spec);

    while (i < n) {
        char pair[128];
        size_t j = i, len;
        char *eq;

        while (j < n && spec[j] != ',')
            ++j;
        len = j - i;
        if (len > 0 && len < sizeof(pair)) {
            memcpy(pair, spec + i, len);
            pair[len] = '\0';
            eq = strchr(pair, '=');
            if (eq) {
                OverrideCtx o;
                *eq = '\0';
                o.key = pair;
                o.value = atof(eq + 1);
                o.bodies = 0;
                jce_scene_each_entity(scene, apply_one, &o);
                if (o.bodies > 0) {
                    jce_json_array_push_string(applied, pair);
                    applied_count++;
                    bodies += o.bodies;
                }
            }
        }
        i = j + 1;
    }
    jce_json_set_child(report, "overrides_applied", applied);
    jce_json_set_int(report, "overrides_bodies_touched", bodies);
    return applied_count;
}

static void verb_physics_probe(void)
{
    const char *scene_path = env_or("JCE_BRIDGE_ARG", "");
    const double duration = env_num("JCE_BRIDGE_DURATION", 10.0);
    const double settle_eps = env_num("JCE_BRIDGE_SETTLE_EPS", 0.05);
    const float dt = 1.0f / 60.0f;
    JceJson *root = jce_json_object();
    JceScene *scene = NULL;
    JceRuntime *rt = NULL;
    JcePakArchive *pak = NULL;
    JceRuntimeDesc desc;
    JceJson *doc = NULL;
    int steps, i, loaded;
    double t = 0.0;

    jce_json_set_string(root, "verb", "physics-probe");
    jce_json_set_string(root, "engine_version", jce_api_version_string());
    jce_json_set_number(root, "duration_s", duration);
    jce_json_set_number(root, "fixed_dt", dt);

    if (!scene_path[0]) {
        jce_json_set_bool(root, "ok", false);
        jce_json_set_string(root, "error", "no scene path");
        emit(root); return;
    }
    doc = jce_json_parse_file(scene_path);
    if (!doc) {
        jce_json_set_bool(root, "ok", false);
        jce_json_set_string(root, "error", "scene file unreadable or not JSON");
        emit(root); return;
    }
    scene = jce_scene_create();
    loaded = jce_scene_load_json(scene, doc);
    jce_json_free(doc);
    if (loaded < 0) {
        jce_json_set_bool(root, "ok", false);
        jce_json_set_string(root, "error", "the engine refused this scene");
        jce_scene_destroy(scene);
        emit(root); return;
    }
    jce_json_set_int(root, "entities_loaded", loaded);

    /* OVERRIDES GO ON BEFORE THE RUNTIME EXISTS, and that ordering is the
     * whole of whether they do anything.
     *
     * MEASURED: applied after jce_runtime_create, every override reported
     * itself as applied -- the components really were written -- and
     * restitution 0.0 against restitution 0.9 produced byte-identical
     * penetration, settle time, jitter and contact count, because the bodies
     * had already been built from the values the file carried.  A tuning
     * search driven by that would have reported noise as an optimum, with a
     * curve and a winner and nothing anywhere saying the knob was dead. */
    {
        int n_over = apply_overrides(scene, root);
        jce_json_set_int(root, "overrides_applied_count", n_over);
    }

    /* ── THE PAK, without which any scene that names an asset is measured
     * as if the asset were not there. ───────────────────────────────────
     *
     * jce_gltf_decode_cpu opens with `if (!pak || !asset_path) return NULL;`
     * -- PAK ONLY, no filesystem fallback, and on a NULL pak it returns
     * before the one LOG_ERROR it would otherwise print.  So a runtime built
     * without a pak loads no model, and the ragdoll spawn in jce_runtime.c
     * (which needs jce_model_get_skeleton) is skipped in silence.
     *
     * MEASURED, and this is why the flag exists: a ragdoll authored
     * correctly -- Ragdoll component present, not disabled, enable true,
     * SkeletalAnimator present, skeletonPath set -- reported zero bones
     * moving, zero NaN and a perfectly stable chain, for THREE different
     * capsule radii including one four times too large.  Stability and
     * never-having-run are the same reading.  Pointing skeletonPath at an
     * absolute path, and at a rig used in production, changed nothing: the
     * pak was NULL and nothing downstream of it could run.
     *
     * The caller cooks the pak with the SDK's own jce_pak, so the archive
     * this reads is written by the engine's writer rather than by a second
     * implementation of the format. */
    {
        const char *pak_path = env_or("JCE_BRIDGE_PAK", "");
        if (pak_path[0]) {
            pak = jce_pak_open_file(pak_path);
            jce_json_set_bool(root, "pak_opened", pak != NULL);
            if (!pak)
                jce_json_set_string(root, "pak_error",
                                    "the pak could not be opened; any scene "
                                    "referencing an asset is measured without "
                                    "it");
            else
                jce_json_set_int(root, "pak_assets",
                                 (int)jce_pak_count(pak));
        } else {
            jce_json_set_bool(root, "pak_opened", false);
        }
    }

    /* THE RUNTIME wires scene components to bodies.  Doing that here instead
     * would be a second scene-to-physics path, and this probe would then be
     * measuring the copy rather than the engine (CN-03). */
    memset(&desc, 0, sizeof(desc));
    desc.scene = scene;
    desc.pak = pak;
    desc.enable_physics = true;
    desc.gravity_y = -9.81f;
    desc.fixed_timestep = dt;
    rt = jce_runtime_create(&desc);
    if (!rt) {
        jce_json_set_bool(root, "ok", false);
        jce_json_set_string(root, "error", "the runtime refused to start");
        jce_scene_destroy(scene);
        emit(root); return;
    }

    memset(&g_probe, 0, sizeof(g_probe));
    g_probe.settle_time = -1.0;
    jce_scene_each_entity(scene, probe_collect, &g_probe);
    g_ragdoll_count = 0;
    jce_scene_each_entity(scene, ragdoll_collect, NULL);
    jce_physics_set_contact_begin(jce_runtime_physics(rt), on_contact, NULL);

    steps = (int)(duration / (double)dt);
    for (i = 0; i < steps; ++i) {
        double ke = 0.0, speed_max = 0.0;
        int b;

        jce_runtime_step(rt, dt);
        t += (double)dt;
        ragdoll_sample(scene);

        for (b = 0; b < g_probe.count; ++b) {
            ProbeBody *pb = &g_probe.bodies[b];
            JceTransform *tr = jce_scene_get_transform(scene, pb->e);
            jce_vec3 pos;
            if (!tr)
                continue;
            pos = tr->position;
            if (!finite3(pos)) {
                /* Counted ONCE per body, not once per step: a body that goes
                 * NaN stays NaN, and counting per step would report sixty
                 * events a second for one defect and drown everything else. */
                if (pb->have_prev) {
                    g_probe.nan_events++;
                    pb->have_prev = false;
                }
                continue;
            }
            if (pb->have_prev) {
                double sp;
                pb->vel.x = (pos.x - pb->prev.x) / dt;
                pb->vel.y = (pos.y - pb->prev.y) / dt;
                pb->vel.z = (pos.z - pb->prev.z) / dt;
                sp = sqrt((double)(pb->vel.x * pb->vel.x +
                                   pb->vel.y * pb->vel.y +
                                   pb->vel.z * pb->vel.z));
                if (sp > speed_max) speed_max = sp;
                ke += 0.5 * (double)pb->mass * sp * sp;
            }
            {
                /* The body's local up (0,1,0) rotated by its orientation,
                 * dotted with world up.  Quaternion applied directly rather
                 * than via a matrix: one fewer place for a transposed basis
                 * to hide, which this tree has paid for four times. */
                const jce_quat q = tr->rotation;
                float uy = 1.0f - 2.0f * (q.x * q.x + q.z * q.z);
                float deg;
                if (uy > 1.0f)  uy = 1.0f;
                if (uy < -1.0f) uy = -1.0f;
                deg = (float)(acos((double)uy) * 180.0 / 3.14159265358979323846);
                if (deg > pb->max_tilt_deg)
                    pb->max_tilt_deg = deg;
            }
            if (!pb->have_min || pos.y < pb->y_min) {
                pb->y_min = pos.y;
                pb->y_max_after_min = pos.y;
                pb->have_min = true;
            } else if (pos.y > pb->y_max_after_min) {
                pb->y_max_after_min = pos.y;
            }
            pb->prev = pos;
            pb->have_prev = true;
        }

        if (i == 1)
            g_probe.energy_first = ke;
        g_probe.energy_last = ke;

        if (!g_probe.settled && i > 1 && speed_max < settle_eps) {
            g_probe.settle_time = t;
            g_probe.settled = true;
        }
        /* Jitter is the residual motion AFTER settling: a body that keeps
         * twitching in contact reads as "settled" to a threshold test and as
         * wrong to anybody watching it. */
        if (g_probe.settled) {
            g_probe.jitter_accum += ke;
            g_probe.jitter_speed_accum += speed_max;
            g_probe.jitter_samples++;
        }
    }

    jce_json_set_bool(root, "ok", true);
    jce_json_set_int(root, "body_count", g_probe.count);
    ragdoll_report(root);
    jce_json_set_int(root, "steps", steps);
    jce_json_set_number(root, "max_penetration_m", g_probe.max_penetration);
    jce_json_set_number(root, "contact_events", (double)g_probe.contact_events);
    jce_json_set_number(root, "trigger_contacts",
                        (double)g_probe.trigger_contacts);
    jce_json_set_number(root, "nan_events", (double)g_probe.nan_events);
    if (g_probe.settled) {
        jce_json_set_bool(root, "settled", true);
        jce_json_set_number(root, "settle_s", g_probe.settle_time);
    } else {
        /* Absent, not a large number.  "It had not settled after 10 s" and
         * "it settled at 10 s" are different facts, and a caller comparing
         * settle_s against a budget must not be handed the second when the
         * first is true. */
        jce_json_set_bool(root, "settled", false);
        jce_json_set_string(root, "settle_absent_because",
                            "nothing was below the settle threshold before the "
                            "probe ran out of time");
    }
    jce_json_set_number(root, "settle_eps_m_per_s", settle_eps);
    jce_json_set_number(root, "jitter_energy",
                        g_probe.jitter_samples
                            ? g_probe.jitter_accum / (double)g_probe.jitter_samples
                            : 0.0);
    jce_json_set_number(root, "jitter_speed_m_per_s",
                        g_probe.jitter_samples
                            ? g_probe.jitter_speed_accum
                              / (double)g_probe.jitter_samples
                            : 0.0);
    {
        double rebound = 0.0;
        int b;
        for (b = 0; b < g_probe.count; ++b) {
            const ProbeBody *pb = &g_probe.bodies[b];
            double r;
            if (!pb->have_min)
                continue;
            r = (double)(pb->y_max_after_min - pb->y_min);
            if (r > rebound)
                rebound = r;
        }
        jce_json_set_number(root, "max_rebound_m", rebound);
    }
    {
        double tilt = 0.0;
        int b, rolled = 0;
        for (b = 0; b < g_probe.count; ++b) {
            double t = (double)g_probe.bodies[b].max_tilt_deg;
            if (t > tilt) tilt = t;
            if (t >= 60.0) rolled++;
        }
        jce_json_set_number(root, "max_tilt_deg", tilt);
        /* 60 degrees, because past that a wheeled vehicle cannot recover: its
         * contact patch has left the ground.  Reported as a count as well as
         * an angle so a scene with one tipped crate among fifty reads
         * differently from one where everything fell over. */
        jce_json_set_int(root, "rolled_over_bodies", rolled);
        jce_json_set_number(root, "rollover_threshold_deg", 60.0);
    }
    jce_json_set_number(root, "kinetic_energy_start", g_probe.energy_first);
    jce_json_set_number(root, "kinetic_energy_end", g_probe.energy_last);
    if (g_probe.energy_first > 1e-9)
        jce_json_set_number(root, "energy_drift_ratio",
                            (g_probe.energy_last - g_probe.energy_first)
                            / g_probe.energy_first);
    jce_json_set_string(root, "measured_from",
                        "entity transforms written back by the runtime each "
                        "step, plus the solver's own contact depths -- not the "
                        "solver's internal state");

    jce_runtime_destroy(rt);
    jce_scene_destroy(scene);
    if (pak) jce_pak_close(pak);
    emit(root);
}

/* ── scene-capture ────────────────────────────────────────────────────
 *
 * Render a scene and write one frame to a PNG, so scene.evaluate_visual has
 * something to measure.
 *
 * WHY NOT THE EDITOR.  The editor can already do this (JCE_WINCAP_PATH), and
 * two things make it the wrong host here.  It holds a SINGLE-INSTANCE LOCK --
 * with an editor already open in this checkout, a second launch activates that
 * window instead of rendering the scene asked for, and the capture that comes
 * back is of somebody else's session.  And its capture is the whole editor
 * window, most of which is UI: a rubric measuring composition and density
 * would be measuring panels.  This bridge is an SDK consumer with its own
 * process and its own build directory, and it renders the scene alone.
 *
 * AIMING THE CAMERA IS THE WHOLE RISK.  There is no public scene-bounds query,
 * so the frame is aimed from entity TRANSFORM POSITIONS -- an approximation,
 * because a position is not a mesh.  That is stated rather than hidden, and it
 * is why the renderer's own count is read back afterwards:
 * jce_scene_renderer_get_cull_stats reports what survived culling, and this
 * emits it in the SAME `SHOT <path> visible=N total=N culled=N` line the
 * editor emits, so jce_determinism.assert_subject_in_frame parses both with
 * one regex and neither grows a second copy.
 *
 * A frame with sky, grid and gizmos in it and NONE of its subject passes every
 * blank-image guard there is.  This repository has written a parity-ledger row
 * off exactly that frame.  So the aim is a guess and `visible` is the answer.
 */

static JceScene         *g_cap_scene;
static JcePakArchive    *g_cap_pak;
static JceRuntime       *g_cap_rt;
static JceSceneRenderer *g_cap_sr;
static JceCamera        *g_cap_cam;
static JceJson          *g_cap_root;
static int               g_cap_frames;
static int               g_cap_frame_at;
static int               g_cap_drawn;
static bool              g_cap_shot_logged;
static char              g_cap_png[512];

typedef struct {
    jce_vec3 lo, hi;
    int      n;
} BoundsCtx;

static void bounds_collect(JceScene *s, JceEntity e, void *ud)
{
    BoundsCtx *b = (BoundsCtx *)ud;
    JceTransform *tr = jce_scene_get_transform(s, e);
    jce_vec3 p;
    if (!tr) return;
    /* ONLY WHAT CAN BE SEEN.  Framing from every transform folds the scene's
     * own Camera and Light entities into the bounds, and those are authored
     * OUTSIDE the content -- a camera at 0.45 of the world span stretched a
     * 40 x 224 m street into a 200 x 224 m box, which reads as square, so the
     * along-the-axis shot never fired and the street was photographed from
     * a corner.  MEASURED: the frame sat at 0.043 with 75 crates in it,
     * against 0.031 for ground and sky alone. */
    if (!jce_scene_has_mesh_renderer(s, e)) return;
    p = tr->position;
    if (!isfinite(p.x) || !isfinite(p.y) || !isfinite(p.z)) return;
    if (b->n == 0) { b->lo = p; b->hi = p; }
    else {
        if (p.x < b->lo.x) b->lo.x = p.x;
        if (p.y < b->lo.y) b->lo.y = p.y;
        if (p.z < b->lo.z) b->lo.z = p.z;
        if (p.x > b->hi.x) b->hi.x = p.x;
        if (p.y > b->hi.y) b->hi.y = p.y;
        if (p.z > b->hi.z) b->hi.z = p.z;
    }
    b->n++;
}

static void verb_scene_capture_init(const JceServices *svc)
{
    const char *scene_path = env_or("JCE_BRIDGE_ARG", "");
    const char *pak_path   = env_or("JCE_BRIDGE_PAK", "");
    const char *png        = env_or("JCE_CAPTURE_PATH", "");
    JceJson *doc = NULL;
    JceRuntimeDesc desc;
    BoundsCtx b;
    jce_vec3 centre, size;
    float radius, dist;
    JceCameraDesc cd;
    int loaded;

    g_cap_root = jce_json_object();
    jce_json_set_string(g_cap_root, "verb", "scene-capture");
    jce_json_set_string(g_cap_root, "engine_version", jce_api_version_string());
    snprintf(g_cap_png, sizeof(g_cap_png), "%s", png);
    jce_json_set_string(g_cap_root, "png", g_cap_png);

    g_cap_frames   = (int)env_num("JCE_BRIDGE_FRAMES", 120.0);
    g_cap_frame_at = (int)env_num("JCE_CAPTURE_FRAME", 90.0);

    if (!scene_path[0] || !png[0]) {
        jce_json_set_bool(g_cap_root, "ok", false);
        jce_json_set_string(g_cap_root, "error",
                            "JCE_BRIDGE_ARG (scene) and JCE_CAPTURE_PATH "
                            "(png) are both required");
        return;
    }
    doc = jce_json_parse_file(scene_path);
    if (!doc) {
        jce_json_set_bool(g_cap_root, "ok", false);
        jce_json_set_string(g_cap_root, "error", "scene unreadable or not JSON");
        return;
    }
    g_cap_scene = jce_scene_create();
    loaded = jce_scene_load_json(g_cap_scene, doc);
    jce_json_free(doc);
    if (loaded < 0) {
        jce_json_set_bool(g_cap_root, "ok", false);
        jce_json_set_string(g_cap_root, "error", "the engine refused this scene");
        return;
    }
    jce_json_set_int(g_cap_root, "entities_loaded", loaded);

    if (pak_path[0]) g_cap_pak = jce_pak_open_file(pak_path);
    jce_json_set_bool(g_cap_root, "pak_opened", g_cap_pak != NULL);

    memset(&desc, 0, sizeof(desc));
    desc.scene = g_cap_scene;
    desc.pak = g_cap_pak;
    desc.enable_physics = true;
    desc.gravity_y = -9.81f;
    desc.fixed_timestep = 1.0f / 60.0f;
    g_cap_rt = jce_runtime_create(&desc);

    /* The scene renderer is the engine's OWN one -- the single source both the
     * editor and the runtime draw a JceScene through.  Drawing the scene here
     * by hand would be a second scene-to-pixels path, and this picture would
     * then be of the copy (CN-03, same rule the probe follows). */
    g_cap_sr = jce_scene_renderer_create(svc->renderer,
                                         g_cap_pak ? g_cap_pak : svc->pak,
                                         NULL);
    if (!g_cap_sr) {
        jce_json_set_bool(g_cap_root, "ok", false);
        jce_json_set_string(g_cap_root, "error",
                            "the scene renderer could not be created");
        return;
    }

    memset(&b, 0, sizeof(b));
    jce_scene_each_entity(g_cap_scene, bounds_collect, &b);
    if (b.n == 0) {
        b.lo = jce_v3(-1.0f, 0.0f, -1.0f);
        b.hi = jce_v3(1.0f, 1.0f, 1.0f);
    }
    centre = jce_v3((b.lo.x + b.hi.x) * 0.5f,
                    (b.lo.y + b.hi.y) * 0.5f,
                    (b.lo.z + b.hi.z) * 0.5f);
    size = jce_v3(b.hi.x - b.lo.x, b.hi.y - b.lo.y, b.hi.z - b.lo.z);
    radius = size.x;
    if (size.y > radius) radius = size.y;
    if (size.z > radius) radius = size.z;
    radius *= 0.5f;
    if (radius < 0.5f) radius = 0.5f;

    /* AN ESTABLISHING SHOT, NOT A BOUNDING-SPHERE SHOT.
     *
     * The first version framed the whole extent from a corner: camera at
     * centre + 2.4 * radius on the diagonal, looking at the centroid.  That
     * frames everything and is the wrong picture for anything long.  MEASURED
     * on an urban_street recipe -- a 20 x 12 x 120 m extent with 20 crates --
     * it put the camera 260 m out, made each 1 m crate about 4 pixels, and the
     * frame measured as empty as a scene with no objects in it at all
     * (density 0.039 against 0.031 for ground and sky alone).  Pulling the
     * camera IN made it worse, not better: closer, fewer objects are in frame.
     *
     * So the camera stands at one end of the scene's LONGEST axis and looks
     * along it, which is how a person shoots a street.  The distance is set by
     * the CROSS-SECTION -- the other two extents -- so the near objects fill
     * the frame and the far ones recede, instead of every object being equally
     * tiny.  Objects are the subject; the extent is not. */
    memset(&cd, 0, sizeof(cd));
    cd.mode = JCE_CAMERA_PERSPECTIVE;
    {
        float fov_rad = 45.0f * 3.14159265358979323846f / 180.0f;
        float half_tan = (float)tan((double)fov_rad * 0.5);
        float cross, along, eye_h;
        jce_vec3 eye;
        int long_z = (size.z >= size.x);
        cross = long_z ? size.x : size.z;
        if (size.y > cross) cross = size.y;
        /* A DEGENERATE AXIS IS NOT A ZERO-SIZE SCENE.  Positions are points,
         * so a scene whose objects all sit on x = 0 -- a corridor, a row of
         * props, the north-star smoke scene -- measures an X extent of
         * exactly 0 and framed itself from 1.2 m away.  The cross-section
         * falls back to a fraction of the long axis, which is the only other
         * thing known about the scene's size. */
        along = (long_z ? size.z : size.x) * 0.5f;
        if (cross < along * 0.25f) cross = along * 0.25f;
        if (cross < 1.0f) cross = 1.0f;
        /* WHICH SHOT depends on the scene's SHAPE, and one rule does not serve
         * both.  MEASURED, same 80 m scatter at rising object counts: the
         * along-the-axis shot put the content in a small band near the horizon
         * (spread 0.146-0.177 against 0.534-0.560 from the three-quarter
         * view), because a square scatter seen end-on piles up in the
         * distance.  The long shot is for long scenes; a squarish one wants
         * the three-quarter view that the first version used. */
        if (along * 2.0f < cross * 2.5f) {
            float r3 = (along > cross * 0.5f) ? along : cross * 0.5f;
            float d3 = r3 * 2.4f
                       * (float)env_num("JCE_BRIDGE_FRAME_DIST", 1.0);
            cd.position = jce_v3(centre.x + d3 * 0.75f,
                                 centre.y + d3 * 0.55f,
                                 centre.z + d3 * 0.75f);
            cd.target = centre;
            dist = d3;
            radius = r3;
            goto camera_done;
        }
        /* Far enough back that the cross-section fits with a margin, plus half
         * the long axis so the near end is in front of the camera. */
        dist = (cross * 0.5f) / (half_tan > 1e-4f ? half_tan : 0.4f) * 1.25f;
        dist *= (float)env_num("JCE_BRIDGE_FRAME_DIST", 1.0);
        eye_h = centre.y + cross * 0.35f;
        /* INSIDE the volume, at its near end -- not `along + dist` beyond it.
         * MEASURED on a 240 m street with 75 crates: standing 60 m outside the
         * near end put every crate past 170 m and the frame measured 0.043,
         * barely above the 0.031 that ground and sky alone read.  From just
         * inside the near end the near crates are metres away and the far ones
         * recede, which is what a street shot is and what makes a dense scene
         * measure dense.  `dist` still sets the height and the far plane. */
        /* One cross-section BEYOND the near end, never inside it.
         *
         * Standing just inside (0.95 of the half-length) framed a street well
         * and put the north-star scene's Player at z = -4.0 BEHIND a camera at
         * z = -3.8.  Content out of the frustum is not a composition problem,
         * it is a wrong picture, and no rubric axis can tell the difference --
         * the frame still had a ground and a door in it and measured a
         * perfectly ordinary contrast of 127.
         *
         * So the eye sits one cross-section before the nearest content: far
         * enough that everything is in front of it, near enough that the scale
         * is set by the scene's width rather than its length.  Whether it
         * worked is not assumed: `framing_kept_all_content` below compares the
         * renderer's own visible count against the total. */
        {
            float margin = cross > 1.0f ? cross : 1.0f;
            if (long_z)
                eye = jce_v3(centre.x, eye_h, centre.z - along - margin);
            else
                eye = jce_v3(centre.x - along - margin, eye_h, centre.z);
        }
        cd.position = eye;
        radius = along > cross ? along : cross;
    }
camera_done:

    cd.target = centre;
    cd.up = jce_v3(0.0f, 1.0f, 0.0f);
    cd.fov_deg = 45.0f;
    cd.near_plane = 0.05f;
    cd.far_plane = dist * 20.0f + 100.0f;
    g_cap_cam = jce_camera_create(&cd);

    {
        JceJson *cam = jce_json_object();
        jce_json_set_number(cam, "posX", cd.position.x);
        jce_json_set_number(cam, "posY", cd.position.y);
        jce_json_set_number(cam, "posZ", cd.position.z);
        jce_json_set_number(cam, "targetX", centre.x);
        jce_json_set_number(cam, "targetY", centre.y);
        jce_json_set_number(cam, "targetZ", centre.z);
        jce_json_set_number(cam, "fov_deg", cd.fov_deg);
        jce_json_set_number(cam, "extent_radius_m", radius);
        jce_json_set_int(cam, "framed_from_entities", b.n);
        jce_json_set_string(cam, "note",
                            "aimed from entity TRANSFORM POSITIONS -- there is "
                            "no public scene-bounds query, so this is an "
                            "approximation. `visible` below is what says "
                            "whether it worked.");
        jce_json_set_child(g_cap_root, "camera", cam);
    }
    jce_json_set_bool(g_cap_root, "ok", g_cap_cam != NULL);
}

static void verb_scene_capture_draw(const JceServices *svc)
{
    JceSceneRenderConfig cfg;
    uint32_t vw = 0, vh = 0;
    if (!g_cap_sr || !g_cap_cam || !g_cap_scene) return;

    /* THE CALL WITHOUT WHICH THE GEOMETRY DOES NOT APPEAR.
     *
     * jce_renderer_begin_frame_3d binds the camera's view and projection to
     * the view the scene renderer is about to draw into, and sets its rect.
     * Without it the skybox still draws -- it carries its own transform -- and
     * every opaque mesh submits against an unset view, so the frame comes back
     * as a flat field of sky with `visible=N total=N culled=0` beside it.
     *
     * MEASURED, and this is the whole reason the rubric has a blank-frame
     * precondition: a single 10 m cube at the origin, camera framed on it,
     * 2 of 2 entities surviving culling, and the capture was one uniform
     * colour (166,199,235) in all 921600 pixels.  Nothing in the cull stats,
     * the logs or the exit code said so.
     *
     * Copied from the SHIPPED path (caged_kingdom ck_app.c:922) rather than
     * from the editor's, which renders into an offscreen target and composites
     * -- that is a different arrangement and this bridge is standing in for a
     * shipped game. */
    jce_renderer_begin_frame_3d(svc->renderer, svc->window, g_cap_cam,
                                JCE_VIEW_MAIN_3D);
    /* The engine's own defaults, not a zeroed struct.  A memset config sets
     * framebuffer to 0 rather than the UINT16_MAX that means "the
     * backbuffer", so the scene renders into framebuffer handle 0 and the
     * capture reads a backbuffer nothing drew into.  MEASURED: the renderer
     * printed "avg_lum=0.0 <-- BLACK backbuffer" on a scene with 23 visible
     * entities, a light and a camera. */
    cfg = jce_scene_render_config_default();
    jce_window_get_size(svc->window, &vw, &vh);
    cfg.viewport_width  = (int)vw;
    cfg.viewport_height = (int)vh;
    /* JCE_VIEW_RUNTIME_GAME, not 0.  jce_views.h reserves view ids across the
     * engine and the scene renderer spans base+0..base+17 and beyond; 30 is the
     * base the SHIPPED runtime uses, which is the thing this bridge is
     * standing in for. */
    jce_scene_renderer_render(g_cap_sr, g_cap_scene, g_cap_cam,
                              JCE_VIEW_MAIN_3D, 1.0f / 60.0f, &cfg);
    g_cap_drawn++;
    /* Emit the count AFTER the frame the engine captures, in the editor's own
     * wording, so one regex reads both. */
    if (!g_cap_shot_logged && g_cap_drawn >= g_cap_frame_at) {
        JceSceneCullStats cs;
        memset(&cs, 0, sizeof(cs));
        jce_scene_renderer_get_cull_stats(g_cap_sr, &cs);
        LOG_INFO("bridge", "SHOT %s visible=%u total=%u culled=%u",
                 g_cap_png, cs.visible, cs.total, cs.culled);
        jce_json_set_int(g_cap_root, "visible", (int)cs.visible);
        jce_json_set_int(g_cap_root, "total", (int)cs.total);
        jce_json_set_int(g_cap_root, "culled", (int)cs.culled);
        /* The framing is a guess from positions; this is the check.  Content
         * the camera left out is a wrong picture that every rubric axis will
         * happily score. */
        jce_json_set_bool(g_cap_root, "framing_kept_all_content",
                          cs.culled == 0u);
        g_cap_shot_logged = true;
    }
}

static void verb_scene_capture_finish(void)
{
    if (!g_cap_root) return;
    jce_json_set_int(g_cap_root, "frames_drawn", g_cap_drawn);
    if (!g_cap_shot_logged)
        jce_json_set_string(g_cap_root, "warning",
                            "the run ended before the capture frame, so no "
                            "visible count was taken and no PNG was written");
    if (g_cap_cam) jce_camera_destroy(g_cap_cam);
    if (g_cap_sr) jce_scene_renderer_destroy(g_cap_sr);
    if (g_cap_rt) jce_runtime_destroy(g_cap_rt);
    if (g_cap_scene) jce_scene_destroy(g_cap_scene);
    if (g_cap_pak) jce_pak_close(g_cap_pak);
    g_cap_cam = NULL; g_cap_sr = NULL; g_cap_rt = NULL;
    g_cap_scene = NULL; g_cap_pak = NULL;
    emit(g_cap_root);
    g_cap_root = NULL;
}

/* ── bundle-diff ──────────────────────────────────────────────────────
 *
 * `jce_bundle_pack_diff` emits a .jdiff document -- added, updated, removed --
 * between two bundle catalogs.  It is public, it works, and the parity ledger
 * records it as BEHIND because NEITHER end was wired: nothing produced a diff
 * and nothing applied one.
 *
 * This closes the producing half, and it belongs next to provenance rather
 * than on its own.  A manifest already says what every file in ONE package
 * hashes to; a diff says what changed between TWO of them.  Given the two
 * manifests and the diff, "this patch turns package A into package B" stops
 * being a claim about a build server and becomes arithmetic anyone can redo.
 *
 * The APPLYING half stays open, and stays on the ledger.  Producing a patch
 * nobody can apply is half a feature, and saying so is the difference between
 * a closed row and a row that looks closed.
 */
static void verb_bundle_diff(void)
{
    const char *old_cat = env_or("JCE_BRIDGE_OLD_CATALOG", "");
    const char *new_cat = env_or("JCE_BRIDGE_NEW_CATALOG", "");
    const char *out     = env_or("JCE_BRIDGE_ARG", "");
    JceJson *root = jce_json_object();
    int rc;

    jce_json_set_string(root, "verb", "bundle-diff");
    jce_json_set_string(root, "engine_version", jce_api_version_string());
    jce_json_set_string(root, "old_catalog", old_cat);
    jce_json_set_string(root, "new_catalog", new_cat);
    jce_json_set_string(root, "out", out);

    if (!old_cat[0] || !new_cat[0] || !out[0]) {
        jce_json_set_bool(root, "ok", false);
        jce_json_set_string(root, "error",
                            "JCE_BRIDGE_OLD_CATALOG, JCE_BRIDGE_NEW_CATALOG "
                            "and JCE_BRIDGE_ARG (the .jdiff path) are all "
                            "required");
        emit(root); return;
    }

    /* The ENGINE'S OWN differ.  Comparing two catalogs here would be a second
     * answer to "what changed", and the two would disagree the first time the
     * catalog format moved. */
    rc = jce_bundle_pack_diff(old_cat, new_cat, out, NULL, NULL);
    jce_json_set_int(root, "exit_code", rc);
    jce_json_set_bool(root, "ok", rc == 0);
    if (rc != 0) {
        jce_json_set_string(root, "error",
                            "jce_bundle_pack_diff refused the inputs; the "
                            "usual cause is a catalog path that is not a "
                            "bundle_catalog.json");
        emit(root); return;
    }

    /* Read the document back and report its shape.  A caller that only got
     * "ok" would have to open the file to learn whether anything changed --
     * and "the diff was produced" and "the diff is empty" are different
     * facts. */
    {
        JceJson *doc = jce_json_parse_file(out);
        if (doc) {
            const char *keys[3] = {"added", "updated", "removed"};
            int i, total = 0;
            for (i = 0; i < 3; ++i) {
                JceJson *arr = jce_json_get(doc, keys[i]);
                int n = (arr && jce_json_is_array(arr))
                        ? (int)jce_json_array_size(arr) : 0;
                jce_json_set_int(root, keys[i], n);
                total += n;
            }
            jce_json_set_int(root, "changed_total", total);
            jce_json_set_bool(root, "identical", total == 0);
            jce_json_free(doc);
        } else {
            jce_json_set_string(root, "warning",
                                "the diff was written but could not be read "
                                "back as JSON");
        }
    }
    emit(root);
}

/* ── app shell ───────────────────────────────────────────────────── */

static bool s_done = false;

/* Every verb but one answers in init() and quits on the first frame, because
 * a question about the engine's own tables or its solver needs no pictures.
 * scene-capture is the exception: it has to run the render loop until the
 * frame the engine captures, so it arms the loop here and finishes in exit().
 */
static bool s_capturing = false;

static bool bridge_init(const JceServices *svc, void *ud)
{
    const char *verb = env_or("JCE_BRIDGE_VERB", "");
    (void)ud;

    if (strcmp(verb, "introspect") == 0)
        verb_introspect();
    else if (strcmp(verb, "compile-recipe") == 0)
        verb_compile_recipe();
    else if (strcmp(verb, "physics-probe") == 0)
        verb_physics_probe();
    else if (strcmp(verb, "bundle-diff") == 0)
        verb_bundle_diff();
    else if (strcmp(verb, "scene-capture") == 0) {
        verb_scene_capture_init(svc);
        s_capturing = true;
        return true;                      /* the loop runs; exit() emits */
    } else
        emit_error(verb, "unknown JCE_BRIDGE_VERB "
                         "(introspect|compile-recipe|physics-probe|"
                         "scene-capture|bundle-diff)");
    s_done = true;
    return true;
}

static void bridge_update(float dt, void *ud)
{
    (void)ud;
    if (s_capturing && g_cap_rt)
        jce_runtime_step(g_cap_rt, dt > 0.0f ? dt : (1.0f / 60.0f));
}

static void bridge_draw(const JceServices *svc, void *ud)
{
    (void)ud;
    if (s_capturing)
        verb_scene_capture_draw(svc);
}

static void bridge_exit(void *ud)
{
    (void)ud;
    if (s_capturing)
        verb_scene_capture_finish();
}

static bool bridge_should_quit(void *ud)
{
    (void)ud;
    if (s_capturing)
        return g_cap_drawn >= g_cap_frames || g_cap_root == NULL
               || g_cap_sr == NULL;
    return s_done;
}

static JceAppDesc bridge_get_desc(void)
{
    JceAppDesc d;
    memset(&d, 0, sizeof d);
    d.name          = "JceAgentBridge";
    d.init          = bridge_init;
    d.update        = bridge_update;
    d.draw          = bridge_draw;
    d.exit          = bridge_exit;
    d.should_quit   = bridge_should_quit;
    /* Big enough that the rubric's 48x32 grid has real pixels in each cell;
     * the window is hidden by JCE_WINDOW_HIDDEN, which keeps the whole
     * device/swapchain/readback path and only withholds it from the desktop. */
    d.window_width  = 1280;
    d.window_height = 720;
    return d;
}

JCE_MAIN(bridge_get_desc)
