/*
 * es_script_probe.c — count what each language's script actually did, and say
 * the number out loud.
 *
 * WHY THIS EXISTS.  "The scene runs and looks right" does not distinguish a
 * script that runs from a script that was never instantiated: the Lua
 * director draws the whole diorama, so four silent backends look exactly
 * like four working ones.  Worse, the two failure modes are opposite —
 * on_start ran and on_update stopped (a throw disabled the callback:
 * THE FAILING-CALLBACK RULE, 3ae0ad3b), versus neither ever ran (no VM
 * claimed the extension) — and neither shows on screen.
 *
 * THE CHANNEL.  Each scripted entity writes its own counters into a dedicated
 * probe entity's TRANSFORM, which is the only structured value every backend
 * can write through the same call:
 *
 *     position.x = number of on_start calls   (must end at exactly 1)
 *     position.y = number of on_update calls  (must keep climbing)
 *     position.z = how many scene entities that script resolved by name
 *
 * The probes are ordinary entities authored by gen_scene.py with a Transform
 * and nothing else — no renderer, no light — so writing them is free and
 * visible in the editor's inspector during Play.
 *
 * WHY NOT JUST READ THE LOG.  Lua, Python and Java can all reach
 * JceScriptHost::log, so all three CAN print a start line.  The C++ and C
 * backends cannot: both reach the engine only through scripting/c_abi, and
 * `log` is one of the seven entries that ABI deliberately does not export
 * (C++ through the jce::script::Api wrapper, C through the raw header).  And a
 * start line proves on_start; nothing in a log proves on_update is still
 * being delivered at frame 3000 without printing 3000 lines.  The transform
 * answers both questions for all five languages in one shape.
 */

#include "es_script_probe.h"

#include <jce/api.h>

#include <stdio.h>
#include <string.h>

typedef struct {
    const char *language;   /* what a reader needs to see, not a VM key   */
    const char *entity;     /* the probe entity gen_scene.py authors      */
    const char *script;     /* the Script component's scriptPath          */
    JceEntity   id;
    int         starts;
    int         updates;
    int         found;
} EsProbe;

/* ONE ROW PER SCRIPTED LANGUAGE IN THE SCENE.  Adding a language to
 * elemental_serenity means adding a row here too; a language with no row is
 * a language nothing counts, which is the state this file exists to end. */
static EsProbe s_probes[] = {
    { "lua",    "EsLuaProbe",  "scripts/es_director.lua",  0, 0, 0, 0 },
    { "python", "EsPyProbe",   "scripts/es_fireflies.py",  0, 0, 0, 0 },
    { "java",   "EsJavaProbe", "scripts/EsCampfire.java",  0, 0, 0, 0 },
    { "cpp",    "EsCppProbe",  "scripts/EsFlowerSway.jcecpp", 0, 0, 0, 0 },
    { "c",      "EsCProbe",    "scripts/EsPropSurface.jcec",  0, 0, 0, 0 },
};
#define ES_PROBE_COUNT ((int)(sizeof s_probes / sizeof s_probes[0]))

static float s_report_t;
static bool  s_resolved;

void es_script_probe_init(JceScene *scene)
{
    int i;
    s_report_t = 0.0f;
    s_resolved = false;
    if (!scene) return;

    for (i = 0; i < ES_PROBE_COUNT; ++i) {
        JceEntity hit[2];
        int n = jce_scene_query_by_name(scene, s_probes[i].entity, hit, 2);
        s_probes[i].id      = (n > 0) ? hit[0] : 0;
        s_probes[i].starts  = 0;
        s_probes[i].updates = 0;
        s_probes[i].found   = 0;
        if (n <= 0)
            fprintf(stderr,
                    "ES probe: entity '%s' is NOT IN THE SCENE — the %s "
                    "script has nowhere to report and this run can prove "
                    "nothing about it\n",
                    s_probes[i].entity, s_probes[i].language);
    }
    s_resolved = true;
}

/* Read every probe's transform into the table.  Cheap: five lookups. */
static void es_probe_sample(JceScene *scene)
{
    int i;
    if (!scene || !s_resolved) return;
    for (i = 0; i < ES_PROBE_COUNT; ++i) {
        JceTransform *t;
        if (!s_probes[i].id) continue;
        t = jce_scene_get_transform(scene, s_probes[i].id);
        if (!t) continue;
        s_probes[i].starts  = (int)(t->position.x + 0.5f);
        s_probes[i].updates = (int)(t->position.y + 0.5f);
        s_probes[i].found   = (int)(t->position.z + 0.5f);
    }
}

void es_script_probe_update(JceScene *scene, float dt)
{
    es_probe_sample(scene);
    s_report_t += dt;
    if (s_report_t >= 10.0f) {
        s_report_t = 0.0f;
        es_script_probe_report("periodic");
    }
}

void es_script_probe_report(const char *why)
{
    int i;
    int live = 0;

    fprintf(stderr, "\n=== ES multi-language script probe (%s) ===\n",
            why ? why : "");
    fprintf(stderr, "%-8s %-28s %8s %8s %8s\n",
            "language", "scriptPath", "on_start", "on_update", "resolved");
    for (i = 0; i < ES_PROBE_COUNT; ++i) {
        const EsProbe *p = &s_probes[i];
        fprintf(stderr, "%-8s %-28s %8d %8d %8d%s\n",
                p->language, p->script, p->starts, p->updates, p->found,
                (p->id == 0) ? "   [NO PROBE ENTITY]"
                             : (p->starts == 0 ? "   [NEVER RAN]" : ""));
        if (p->starts > 0 && p->updates > 0) ++live;
    }
    fprintf(stderr, "=== %d of %d languages live ===\n\n",
            live, ES_PROBE_COUNT);
}

int es_script_probe_live_count(void)
{
    int i, live = 0;
    for (i = 0; i < ES_PROBE_COUNT; ++i)
        if (s_probes[i].starts > 0 && s_probes[i].updates > 0) ++live;
    return live;
}
