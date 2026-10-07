/*
 * es_prop_surface.c — the diorama's static props answer to the weather and
 * the season, written in C99 and attached to an entity as a `.jcec` script.
 *
 * ── WHAT THE SCENE WAS MISSING, MEASURED TWO WAYS ────────────────────────
 *
 * 1. NOTHING IN THIS SCENE EVER GETS WET.  Switch to a rainy state and the
 *    pond splashes, the rain emitter runs, the grass darkens through the
 *    director's per-state grass tint — and the rocks, the tree trunks, the
 *    bridge planks, the fire-pit stones and the tent canvas keep the exact
 *    albedo and the exact roughness 1.0 they have in spring sunshine.  No
 *    script and no engine system writes `roughness` on ANY entity in this
 *    project (grep: the director writes Light, Water, GrassField,
 *    FoliageCluster and two MeshRenderer fields, and roughness is not one of
 *    them), so a wet look is not something to take from somebody — it is a
 *    field nobody has ever written.
 *
 * 2. THE SEASONAL STONE TINT WAS AUTHORED AND REACHED NOTHING.  es_palettes.h
 *    carries a `rocks_tint` for all eight season x time-of-day states, from
 *    winter's cold {0.875, 0.925, 0.975} to autumn's warm {0.925, 0.725,
 *    0.525}.  The only consumer was this loop in the Lua director:
 *
 *        ids.rocks = jce.find_by_prefix("rock")     -- lowercase r
 *        for _, e in ipairs(D.ids.rocks or {}) do ... end
 *
 *    and the entity gen_scene.py authors is named "Rocks".  The host's
 *    find_by_prefix is a strncmp (engine/src/application/jce_rt_script.c,
 *    rt_find_prefix_cb) — case sensitive — so that list has ALWAYS been
 *    empty and the loop has always run zero times.  Confirmed against pixels
 *    as well as code: shot_spring_day.png, shot_winter_day.png and
 *    shot_rainy_day.png show the same grey rocks, three states apart, where
 *    the authored tints differ by more than half a unit per channel.
 *
 *    That loop and the `rocks_tint` column of the emitted Lua preset table
 *    are DELETED from build/gen_director.py in the same change that added
 *    this file, so exactly one script writes that field.  The value itself is
 *    not copied anywhere: this file includes es_palettes.h, the same
 *    generated header gen_director.py reads.
 *
 * ── OWNERSHIP — see MULTILANG.md ─────────────────────────────────────────
 *
 * This script owns `MeshRenderer.baseColorR/G/B` and `MeshRenderer.roughness`
 * of exactly five entities: Rocks, Trees, Bridge, Camp, Tent.  Nothing else
 * writes any field of those components.
 *
 * GROUND IS DELIBERATELY NOT IN THE LIST even though it is the same kind of
 * surface: the director writes `Ground.albedoTex` per state, and comp_set
 * REPLACES a whole component — two writers on one component is the one thing
 * MULTILANG.md forbids, and it would be invisible (last writer of the frame
 * wins, and both write plausible values).
 *
 * ── WHY C, HONESTLY ──────────────────────────────────────────────────────
 *
 * Not speed.  This is five entities, and the C++ flower script already holds
 * the per-frame-throughput argument with 102.
 *
 * The reason is the include on the next screen: `#include "es_palettes.h"`.
 * This project's application is C99, and its palette is a generated C header
 * — so a C script reads the SAME `ES_PRESETS[8]` table the Lua director's
 * generator parses, out of the same file, with no copy, no export step and no
 * second spelling of the numbers.  No other language here can do that: Lua
 * gets a generated transcription, and Python and Java would each need their
 * own.  A value that exists once cannot drift, and this whole repository's
 * worst recurring defect is a value that exists twice.
 *
 * The second reason is what it costs to ship.  Python needs an embeddable
 * CPython; Java needs a JDK to build and a JVM plus the JNI shim beside the
 * exe to run; the C++ script needs the C++ toolchain.  A `.jcec` script needs
 * the compiler that is already compiling main.c.  Of the five languages this
 * scene runs, C is the only one that adds nothing to the build or to the
 * shipped directory — so the work it should own is the work that must survive
 * the leanest possible build of this game, and a diorama whose props are the
 * wrong colour is wrong in every shipped frame.
 *
 * ── HOW IT KNOWS WHICH STATE THE SCENE IS IN ─────────────────────────────
 *
 * It asks the director, and it asks by reading a component the director
 * already writes: `Ground`'s MeshRenderer, whose `albedoTex` is set per state
 * to `ES_PRESETS[i].ground_tex`.  The key and the payload therefore come out
 * of the SAME struct row — the string that identifies the state and the tint
 * that state wants are `.ground_tex` and `.rocks_tint` of one `EsPreset`.
 *
 * This is the pattern EsCampfire.java established for CampfireLight2: read
 * the director's live value instead of writing a second copy of its state
 * machine.  A private `is_raining()` here would be a second authority that
 * agrees today and diverges the first time somebody adds a state.
 *
 * ── NO LOG.  AT ALL. ─────────────────────────────────────────────────────
 *
 * Like the C++ script and unlike Lua, Python and Java, a C script reaches the
 * engine only through <jce/script_api/jce_script_api.h> — scripting/c_abi —
 * and `log` is one of the seven entries that ABI deliberately does not
 * export.  So the one channel this class has for "I am alive" is the EsCProbe
 * transform: x = on_start count, y = on_update count, z = props resolved.
 * stderr is used only for hard failures, and only because this translation
 * unit is also compiled into elemental_serenity.exe; it is not the engine log
 * and in es_scripts_c.dll nothing collects it.
 *
 * AND THE PROBE IS NOT THE ONLY EVIDENCE.  The authored `Rocks` baseColor is
 * {0.68, 0.57, 0.31}.  Every state's `rocks_tint` differs from it in every
 * channel, so ONE sample of Rocks.baseColorR read by a different language is
 * proof this script's writes land — a still value that is not the authored
 * value proves as much as a moving one.  gen_director.py reads it back in
 * Lua next to the five probe rows.
 */

#include <jce/api.h>
#include <jce/script_api/jce_script_api.h>
#include <jce/script_vm/jce_script_vm_c.h>

/* THE POINT OF WRITING THIS IN C.  The project's own generated palette,
 * included directly — the same ES_PRESETS[8] that build/gen_director.py
 * parses to emit the Lua director's preset table.  One authority, two
 * consumers, zero transcriptions. */
#include "es_palettes.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Tuning ──────────────────────────────────────────────────────────────
 *
 * Wetting is FASTER THAN DRYING and the asymmetry is the whole reason this is
 * a per-frame script rather than a per-transition patch: a surface soaks in
 * seconds and dries in a minute, and neither end of that is expressible as a
 * value attached to a discrete state. */
#define ES_WET_RISE_TAU   3.5f   /* seconds to ~63% soaked                  */
#define ES_WET_FALL_TAU  14.0f   /* seconds to ~63% dried                   */
#define ES_WET_DARKEN     0.34f  /* albedo x (1 - this * wet)               */
#define ES_WET_ROUGH      0.34f  /* roughness lerps toward this at wet = 1  */
#define ES_TINT_TAU       1.1f   /* stone tint chase; the dead loop CUT     */
#define ES_STATE_POLL     0.25f  /* seconds between Ground.albedoTex reads  */

/* Write only when a number actually moved.  A comp_set re-parses the whole
 * component and re-enters jce_scene_set_mesh_renderer, which dirties the
 * renderer's per-entity caches; paying that on a frame where nothing changed
 * would be a cost with no product.  1e-4 is well below one 8-bit colour step
 * (1/255 = 3.9e-3). */
#define ES_WRITE_EPS      1.0e-4f

#define ES_PROP_MAX       8
#define ES_JSON_CAP       2048

typedef struct {
    JceScriptEntity e;
    const char     *name;
    int             stone;        /* takes the seasonal rocks_tint          */
    float           base[3];      /* authored baseColor, read once          */
    float           rough;        /* authored roughness, read once          */
    float           last[4];      /* what was last written (rgb + rough)    */
    char            json[ES_JSON_CAP];   /* the component as authored       */
} EsProp;

/* The five.  `stone` marks the ones the authored rocks_tint is FOR — it is a
 * stone grade, and painting the tent canvas or the bridge planks with it
 * would be inventing art direction rather than delivering the art direction
 * that was already written down. */
static const struct { const char *name; int stone; } ES_PROP_DEFS[] = {
    { "Rocks",  1 },
    { "Trees",  0 },
    { "Bridge", 0 },
    { "Camp",   0 },
    { "Tent",   0 },
};
#define ES_PROP_DEF_COUNT ((int)(sizeof ES_PROP_DEFS / sizeof ES_PROP_DEFS[0]))

typedef struct EsPropSurface {
    JceScriptApi   *api;
    JceScriptEntity probe;
    JceScriptEntity ground;
    EsProp          props[ES_PROP_MAX];
    int             nprops;
    int             starts;
    int             frames;
    int             state;        /* 0..7 index into ES_PRESETS; -1 unknown */
    int             warned_state;
    float           poll_t;
    float           tint[3];      /* damped stone tint                      */
    float           wet;          /* 0..1                                   */
    char            scratch[ES_JSON_CAP];
    char            err[128];
} EsPropSurface;

/* ── Minimal JSON help ───────────────────────────────────────────────────
 *
 * comp_get hands back cJSON_PrintUnformatted output, so a key is exactly
 * `"name":` with no spaces.  Two functions is all this needs, and writing
 * them beats linking a parser into a script module: the module would then
 * carry a second JSON implementation into every process that loads it.
 *
 * es_json_num — read the number after `"key":`, or leave *out alone. */
static int es_json_num(const char *json, const char *key, float *out)
{
    char        tok[48];
    const char *p;
    snprintf(tok, sizeof tok, "\"%s\":", key);
    p = strstr(json, tok);
    if (!p) return 0;
    p += strlen(tok);
    *out = (float)atof(p);
    return 1;
}

/* es_json_splice — copy `src` to `dst`, replacing the number after each of
 * `keys[i]` with `vals[i]`.
 *
 * SEQUENTIAL, AND THAT IS DELIBERATE.  The keys must occur in `src` in the
 * order given (they do: ser_mesh_renderer emits baseColorR/G/B before
 * roughness).  Scanning forward means a serialiser that reorders its fields
 * makes this return 0 — a loud failure at the first frame — instead of
 * silently patching the wrong number, which is what an independent
 * search-and-replace per key would do the day two keys share a prefix.
 *
 * Every byte that is not one of those numbers is copied VERBATIM, so fields
 * this script has never heard of — a texture path, a toon block, a `visible`
 * flag added later — survive the round trip untouched. */
static int es_json_splice(const char *src, char *dst, size_t cap,
                          const char *const *keys, const float *vals, int n)
{
    size_t      w = 0;
    size_t      seg;
    const char *cur = src;
    int         i;

    for (i = 0; i < n; ++i) {
        char        tok[48];
        const char *p;
        const char *num;
        int         wrote;

        snprintf(tok, sizeof tok, "\"%s\":", keys[i]);
        p = strstr(cur, tok);
        if (!p) return 0;
        num = p + strlen(tok);

        seg = (size_t)(num - cur);
        if (w + seg >= cap) return 0;
        memcpy(dst + w, cur, seg);
        w += seg;

        wrote = snprintf(dst + w, cap - w, "%.6g", (double)vals[i]);
        if (wrote < 0 || (size_t)wrote >= cap - w) return 0;
        w += (size_t)wrote;

        /* Step over the number that was there: optional sign, digits, dot,
         * exponent.  cJSON never emits anything else in a number. */
        cur = num;
        if (*cur == '-' || *cur == '+') ++cur;
        while ((*cur >= '0' && *cur <= '9') || *cur == '.' ||
               *cur == 'e' || *cur == 'E' ||
               ((*cur == '-' || *cur == '+') &&
                (cur[-1] == 'e' || cur[-1] == 'E')))
            ++cur;
    }

    seg = strlen(cur);
    if (w + seg >= cap) return 0;
    memcpy(dst + w, cur, seg);
    dst[w + seg] = '\0';
    return 1;
}

/* Which of the eight states is live?  The director writes Ground.albedoTex to
 * ES_PRESETS[i].ground_tex, so the answer is a substring test against the
 * very rows this file is about to read the tint out of. */
static int es_state_from_ground(EsPropSurface *s)
{
    int i;
    int n;

    if (!s->ground) return -1;
    n = jce_script_api_comp_get(s->api, s->ground, "MeshRenderer",
                                s->scratch, ES_JSON_CAP);
    if (n <= 0) return -1;
    for (i = 0; i < 8; ++i)
        if (ES_PRESETS[i].ground_tex && ES_PRESETS[i].ground_tex[0] &&
            strstr(s->scratch, ES_PRESETS[i].ground_tex))
            return i;
    return -1;
}

static JceScriptEntity es_first_named(JceScriptApi *api, const char *name)
{
    JceScriptEntity hit[1] = { 0 };
    return (jce_script_api_find_by_name(api, name, hit, 1) > 0) ? hit[0] : 0;
}

/* exp-damped chase, frame-rate independent. */
static float es_chase(float cur, float target, float tau, float dt)
{
    float k;
    if (tau <= 0.0f) return target;
    k = 1.0f - expf(-dt / tau);
    return cur + (target - cur) * k;
}

static void es_write_probe(EsPropSurface *s)
{
    if (s->probe)
        jce_script_api_set_position(s->api, s->probe, (float)s->starts,
                                    (float)s->frames, (float)s->nprops);
}

/* ── Lifecycle ───────────────────────────────────────────────────────── */

static void *es_prop_surface_create(const JceCScriptContext *ctx)
{
    EsPropSurface *s;

    /* struct_size FIRST.  This struct was allocated by the ENGINE; a module
     * built against a newer header would otherwise read past its end. */
    if (!ctx || ctx->struct_size < sizeof(JceCScriptContext)) return NULL;

    s = (EsPropSurface *)calloc(1, sizeof *s);
    if (!s) return NULL;

    /* BOTH arguments, and host_size is the VM's ALREADY-CLAMPED number.
     * Passing sizeof(JceScriptHost) here would re-widen a host the engine
     * narrowed — the exact over-read the short-host ABI exists to prevent. */
    s->api = jce_script_api_open(ctx->host, ctx->host_size,
                                 JCE_SCRIPT_API_VERSION);
    if (!s->api) {
        fprintf(stderr, "es_prop_surface (c): jce_script_api_open REFUSED "
                        "— this script can do nothing and says so here "
                        "because a C script has no engine log\n");
        free(s);
        return NULL;
    }
    s->state = -1;
    return s;
}

static void es_prop_surface_destroy(void *self)
{
    EsPropSurface *s = (EsPropSurface *)self;
    if (!s) return;
    jce_script_api_close(s->api);
    free(s);
}

static JceCStatus es_prop_surface_start(void *self)
{
    EsPropSurface *s = (EsPropSurface *)self;
    int i;

    ++s->starts;
    s->probe  = es_first_named(s->api, "EsCProbe");
    s->ground = es_first_named(s->api, "Ground");

    for (i = 0; i < ES_PROP_DEF_COUNT && s->nprops < ES_PROP_MAX; ++i) {
        JceScriptEntity e = es_first_named(s->api, ES_PROP_DEFS[i].name);
        EsProp         *p;
        int             n;

        if (!e) continue;

        p = &s->props[s->nprops];
        n = jce_script_api_comp_get(s->api, e, "MeshRenderer",
                                    p->json, ES_JSON_CAP);
        /* n is the FULL length and may exceed the buffer — a truncated
         * component would be written back as invalid JSON, so refuse it. */
        if (n <= 0 || n >= ES_JSON_CAP) {
            fprintf(stderr,
                    "es_prop_surface (c): '%s' MeshRenderer is %d bytes "
                    "(cap %d) — skipped rather than written back truncated\n",
                    ES_PROP_DEFS[i].name, n, ES_JSON_CAP);
            continue;
        }

        p->e     = e;
        p->name  = ES_PROP_DEFS[i].name;
        p->stone = ES_PROP_DEFS[i].stone;
        p->base[0] = p->base[1] = p->base[2] = 1.0f;
        p->rough   = 1.0f;
        es_json_num(p->json, "baseColorR", &p->base[0]);
        es_json_num(p->json, "baseColorG", &p->base[1]);
        es_json_num(p->json, "baseColorB", &p->base[2]);
        es_json_num(p->json, "roughness",  &p->rough);
        p->last[0] = p->last[1] = p->last[2] = p->last[3] = -1.0f;
        ++s->nprops;
    }

    /* Seed the tint from the live state so the first frame is already right;
     * a chase that started at black would flash the rocks dark on load. */
    s->state = es_state_from_ground(s);
    if (s->state >= 0) {
        s->tint[0] = ES_PRESETS[s->state].rocks_tint[0];
        s->tint[1] = ES_PRESETS[s->state].rocks_tint[1];
        s->tint[2] = ES_PRESETS[s->state].rocks_tint[2];
        s->wet     = (s->state / 2 == ES_SEASON_RAINY) ? 1.0f : 0.0f;
    }

    if (!s->probe)
        fprintf(stderr, "es_prop_surface (c): EsCProbe is MISSING — this "
                        "script has no other way to report that it ran\n");
    if (s->nprops == 0)
        fprintf(stderr, "es_prop_surface (c): resolved 0 of %d props — the "
                        "callbacks will still be delivered and NOTHING will "
                        "change on screen\n", ES_PROP_DEF_COUNT);
    es_write_probe(s);
    return JCE_C_OK;
}

static JceCStatus es_prop_surface_update(void *self, float dt)
{
    EsPropSurface *s = (EsPropSurface *)self;
    static const char *const KEYS[4] = {
        "baseColorR", "baseColorG", "baseColorB", "roughness"
    };
    float wet_target;
    int   i;

    ++s->frames;

    /* State poll — 4 Hz, not per frame.  It costs a comp_get of the ground's
     * whole MeshRenderer, and the value it reads changes at most once per
     * user action. */
    s->poll_t += dt;
    if (s->poll_t >= ES_STATE_POLL) {
        int st = es_state_from_ground(s);
        s->poll_t = 0.0f;
        if (st >= 0) {
            s->state = st;
        } else if (!s->warned_state) {
            s->warned_state = 1;
            fprintf(stderr,
                    "es_prop_surface (c): Ground's albedoTex matches none of "
                    "the 8 ES_PRESETS[].ground_tex — holding the last state. "
                    "The director and es_palettes.h have diverged.\n");
        }
    }

    if (s->state < 0) { es_write_probe(s); return JCE_C_OK; }

    /* Wetness: rise fast, fall slow.  ES_SEASON_RAINY covers both the day and
     * the night rainy states, which is why the season is compared and not the
     * state index. */
    wet_target = (s->state / 2 == ES_SEASON_RAINY) ? 1.0f : 0.0f;
    s->wet = es_chase(s->wet, wet_target,
                      (wet_target > s->wet) ? ES_WET_RISE_TAU
                                            : ES_WET_FALL_TAU, dt);

    for (i = 0; i < 3; ++i)
        s->tint[i] = es_chase(s->tint[i], ES_PRESETS[s->state].rocks_tint[i],
                              ES_TINT_TAU, dt);

    for (i = 0; i < s->nprops; ++i) {
        EsProp *p = &s->props[i];
        float   v[4];
        float   darken = 1.0f - ES_WET_DARKEN * s->wet;
        int     c;
        int     changed = 0;

        for (c = 0; c < 3; ++c)
            v[c] = (p->stone ? s->tint[c] : p->base[c]) * darken;
        v[3] = p->rough + (ES_WET_ROUGH - p->rough) * s->wet;

        for (c = 0; c < 4; ++c)
            if (fabsf(v[c] - p->last[c]) > ES_WRITE_EPS) changed = 1;
        if (!changed) continue;

        if (!es_json_splice(p->json, s->scratch, ES_JSON_CAP, KEYS, v, 4)) {
            /* A per-instance buffer, never a local: THE FAILING-CALLBACK RULE
             * logs this string and then stops calling on_update on this
             * instance, so the pointer has to outlive the return. */
            snprintf(s->err, sizeof s->err,
                     "es_prop_surface: cannot patch '%s' MeshRenderer JSON "
                     "(field order changed?)", p->name ? p->name : "?");
            return s->err;
        }
        if (jce_script_api_comp_set(s->api, p->e, "MeshRenderer", s->scratch))
            memcpy(p->last, v, sizeof v);
    }

    es_write_probe(s);
    return JCE_C_OK;
}

/* NOTE WHAT IS NOT HERE: no on_collision, no on_message, no on_anim_event.
 * An omitted handler is an omitted LINE — the slots are named, not
 * positional, so a class that declines a handler declines it by saying
 * nothing. */
JCE_C_SCRIPT_CLASS_BEGIN(EsPropSurface, "EsPropSurface",
                         es_prop_surface_create, es_prop_surface_destroy)
    JCE_C_ON_START(es_prop_surface_start)
    JCE_C_ON_UPDATE(es_prop_surface_update)
JCE_C_SCRIPT_CLASS_END()

/* A SECOND MODULE, not a second class in the C++ one.  One module descriptor
 * is one translation unit and a translation unit is C or C++; and both
 * JCE_C_MODULE_END and JCE_CPP_MODULE_END export `jce_cpp_script_module`, so
 * the two could not share a shared object even if the languages did not
 * matter.  jce_project.json's "script_modules" therefore carries two entries,
 * which is exactly the shape jce_script_vm_c.h documents for a project that
 * writes both. */
JCE_C_MODULE_BEGIN()
    JCE_C_MODULE_CLASS(EsPropSurface)
JCE_C_MODULE_GLOBALS()
JCE_C_MODULE_END("elemental_serenity_c", es_prop_surface_module)

#if !defined(ES_SCRIPT_MODULE_SHARED)
/* Publish the module into the host's registry.  Called once from
 * es_script_langs.c BEFORE the scene loads — a module added after the first
 * instantiate is a module the scene has already been refused by.
 *
 * COMPILED OUT OF es_scripts_c.dll, AND THE LINKER SAYS WHY IF IT IS NOT.
 * jce_script_vm_cpp_add_module() lives in the REGISTRY, the one library a
 * module must not link, so the shared build defines ES_SCRIPT_MODULE_SHARED
 * and this function does not exist in it.  Building it anyway is LNK2019,
 * unresolved jce_script_vm_cpp_add_module — not a subtle failure.
 *
 * THE REGISTRY CALL IS SPELLED `cpp` FOR A C MODULE ON PURPOSE.  There is one
 * native class registry and it serves both languages: a compiled class has no
 * language at run time, only a table of C function pointers, so a second
 * registry would buy a second loader and a second answer to "is this module
 * loaded" for one set of DLLs.  jce_script_vm_c.h states this. */
extern bool es_prop_surface_register(void);
bool es_prop_surface_register(void)
{
    if (!jce_script_vm_cpp_add_module(es_prop_surface_module())) {
        fprintf(stderr,
                "es_prop_surface: module registration FAILED — "
                "instantiate(\"scripts/EsPropSurface.jcec\") will return 0\n");
        return false;
    }
    return true;
}
#endif  /* !ES_SCRIPT_MODULE_SHARED */
