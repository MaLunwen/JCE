/*
 * jce_introspect.c -- the engine describing itself, as JSON.
 *
 * WHY THE COMPONENT SCHEMA IS ROUND-TRIPPED RATHER THAN TABULATED.  The
 * obvious implementation of "list every component and its fields" is a table.
 * A table is also the one implementation that must not be written here: the
 * engine already has exactly one description of every component -- the
 * parse_/ser_ pair in jce_scene_components_json.c that reads and writes every
 * .scene.json on disk and backs the editor's Inspector -- and a second one
 * would drift from it.  The drift would be silent, because both halves keep
 * working; only a scene authored against the stale half is wrong, and the
 * loader DISCARDS an unknown field without a word.
 *
 * So the schema is DERIVED from that pair, in two steps:
 *
 *   1. hand the component's parse function an EMPTY JSON object.  Every field
 *      read in those functions is `j_num(c, "fov", 60.0)` or a sibling, so an
 *      empty object leaves the component holding the engine's own defaults --
 *      the same values a scene file that omits the field would produce.
 *   2. hand the result straight to the component's serialise function.  The
 *      keys that come out are the fields; the values are those defaults.
 *
 * The answer cannot disagree with the serialiser, because it IS the
 * serialiser's output.  That is REQ-INT-02 and ADR-08 made structural instead
 * of remembered.
 *
 * WHY EACH COMPONENT GETS ITS OWN ENTITY.  Several components read another's
 * state when parsing, and a few write through to siblings.  Sharing one
 * entity across ninety-seven parse calls would let component N's defaults be
 * whatever component N-1 happened to leave behind -- and the result would
 * look entirely plausible.  A fresh entity per component costs nothing here
 * and makes the answer independent of registration order.
 */

#include <jce/application/jce_introspect.h>

#include "../middleware/scene/jce_component_registry_internal.h"

#include <jce/jce_version.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_mem_profile.h>
#include <jce/os/core/jce_perf_phase.h>

/* NO <cjson/cJSON.h> HERE, deliberately.  contracts/dependency-ownership.yml
 * whitelists cJSON file by file, and says why: a TU that needs only parse,
 * get, is_*, array walk, object build, print and free is facade-covered and
 * does not belong on the list.  This one needs exactly those, plus detach --
 * which jce_json_detach() provides.  The component registry's parse/serialise
 * hooks are typed in cJSON, so `cJSON` is used as a TYPE (its forward typedef
 * comes from jce_component_registry_internal.h) and JceJson is that same
 * struct; no cJSON FUNCTION is called.
 *
 * The first version of this file included the header for CreateObject,
 * Duplicate and CreateNull, passed every pre-commit gate, and went red in the
 * post-commit architecture audit -- which is precisely the shape the audit
 * exists to catch. */

#include <string.h>

#define LOG_TAG "introspect"

/* One place to turn a finished tree into the caller's buffer.
 *
 * snprintf semantics: the return value is what the answer NEEDS, so a caller
 * can size with cap == 0.  The tree is freed here in every path, including
 * the failure paths -- an introspection call that leaked on the error branch
 * would leak once per poll, and a tool polls. */
static size_t emit(JceJson *root, char *buf, size_t cap)
{
    char  *text;
    size_t need;

    if (!root) {
        if (buf && cap)
            buf[0] = '\0';
        return 0;
    }
    text = jce_json_print(root, true);
    jce_json_free(root);
    if (!text) {
        if (buf && cap)
            buf[0] = '\0';
        return 0;
    }
    need = strlen(text);
    if (buf && cap) {
        size_t n = (need < cap - 1u) ? need : cap - 1u;
        memcpy(buf, text, n);
        buf[n] = '\0';
    }
    jce_json_free_string(text);
    return need;
}

/* MOVE every key of `src` except "type" into `dst`.
 *
 * A move rather than a copy: `src` is a throwaway this function owns, so
 * duplicating its subtrees would allocate a second copy of everything only to
 * free the first.  Whole subtrees travel, so nested objects and arrays
 * survive -- a flattening copy would silently drop the array-valued fields,
 * and nothing downstream could tell "this component has no such field" from
 * "this function dropped it".
 *
 * The next sibling is taken BEFORE detaching: detaching unlinks the node, so
 * reading `next` afterwards walks a list the node is no longer in. */
static void move_fields(JceJson *src, JceJson *dst)
{
    JceJson *it = jce_json_first_child(src);

    while (it) {
        JceJson    *next = jce_json_next_sibling(it);
        const char *key  = jce_json_member_key(it);

        if (key && strcmp(key, "type") != 0) {
            jce_json_detach(src, it);
            jce_json_set_child(dst, key, it);
        }
        it = next;
    }
}

size_t JCE_CALL jce_introspect_components_json(char *buf, size_t cap)
{
    JceJson *root = jce_json_object();
    JceJson *rows = jce_json_array();
    JceScene *tmp;
    int count, id;

    /* Creating a scene is what registers the component table (the registration
     * is self-guarded and idempotent), so this is also how the count below
     * becomes non-zero on a process that has not loaded a scene yet. */
    tmp = jce_scene_create();
    count = jce_component_count();

    jce_json_set_string(root, "engine_version", jce_api_version_string());
    jce_json_set_int(root, "component_count", count);
    jce_json_set_string(root, "source",
                        "round-tripped through the engine's own component "
                        "parser and serialiser; there is no second table");

    for (id = 0; id < count; ++id) {
        const JceComponentDesc *d = jce_component_desc(id);
        JceJson *row;
        JceJson *aliases;
        int i;

        if (!d)
            continue;
        row = jce_json_object();
        jce_json_set_string(row, "name", d->name ? d->name : "");
        jce_json_set_int(row, "id", id);
        jce_json_set_number(row, "legacy_flag", (double)d->legacy_flag);
        jce_json_set_int(row, "struct_size", (int)d->struct_size);

        aliases = jce_json_array();
        for (i = 0; i < 4; ++i)
            if (d->aliases[i] && d->aliases[i][0] &&
                (!d->name || strcmp(d->aliases[i], d->name) != 0))
                jce_json_array_push_string(aliases, d->aliases[i]);
        jce_json_set_child(row, "aliases", aliases);

        /* TWO CAPABILITIES, REPORTED SEPARATELY, because they have different
         * consequences for anything authoring a scene and one flag loses the
         * distinction that matters:
         *
         *   parses, does not serialise   the type IS accepted in a scene file
         *       and applied -- but written back under another name, so the
         *       spelling does not survive a save.  DirectionalLight is a
         *       compat spelling of the unified Light; Animator is migrated.
         *       An author needs to know the name will change, not that it
         *       will not work.
         *   does not parse               the row is derived: produced by
         *       another component's parser, and writing it into a file does
         *       nothing at all.
         *
         * Measured: collapsing both into "authorable": false made the
         * schema-parity gate fail a correct tree on Animator and
         * DirectionalLight, and a gate that is red on a working design is a
         * gate somebody disables. */
        jce_json_set_bool(row, "parses", d->parse != NULL);
        jce_json_set_bool(row, "serializes", d->serialize != NULL);
        if (!d->parse || !d->serialize) {
            /* Reported, not omitted.  "This component does not exist" and
             * "this component exists and is written by another component's
             * serialiser" are different answers, and an omission collapses
             * them into the first -- which is the one that makes a caller
             * stop looking. */
            jce_json_set_bool(row, "authorable", false);
            jce_json_set_string(row, "not_authorable_because",
                                !d->parse
                                    ? "no standalone parser: this row is "
                                      "derived from another component, and "
                                      "writing it into a scene file does "
                                      "nothing"
                                    : "no standalone serialiser: a scene file "
                                      "may carry this type and the engine "
                                      "applies it, but it is written back "
                                      "under another name, so the spelling "
                                      "does not survive a save");
            jce_json_array_push(rows, row);
            continue;
        }

        {
            JceEntity e = jce_scene_create_entity(tmp, "__introspect__");
            JceJson *empty = jce_json_object();
            JceJson *out = jce_json_array();
            JceJson *first;
            JceJson *fields = jce_json_object();

            d->parse(tmp, e, (const cJSON *)empty);
            d->serialize(tmp, e, (cJSON *)out);
            first = jce_json_array_at(out, 0);
            if (first) {
                /* TWO SHAPES, BOTH REAL.  Most components serialise flat --
                 * {"type": "Transform", "posX": 0, ...} -- but some nest
                 * their fields under "properties", and the registry's own
                 * parse hook documents that ("`props` is the resolved property
                 * object (the "properties" child when nested)").  Reporting
                 * the outer keys for those gives a component whose only field
                 * is called "properties", which is what the first version did
                 * and what the schema-parity gate caught on Avatar.  The
                 * nesting is recorded rather than hidden: a writer has to
                 * reproduce it. */
                JceJson *props = jce_json_get(first, "properties");
                if (props && jce_json_is_object(props) &&
                    jce_json_first_child(props)) {
                    move_fields(props, fields);
                    jce_json_set_bool(row, "fields_nested_in_properties", true);
                } else {
                    move_fields(first, fields);
                }
            }
            jce_json_set_bool(row, "authorable", true);
            jce_json_set_child(row, "fields", fields);
            if (!first) {
                /* The pair exists and produced nothing.  Said out loud: an
                 * empty field set and a component with no fields look the
                 * same, and only one of them is a defect. */
                jce_json_set_string(row, "note",
                                    "the serialiser emitted nothing for a "
                                    "freshly defaulted component");
            }
            jce_json_free(empty);
            jce_json_free(out);
            jce_scene_destroy_entity(tmp, e);
        }
        jce_json_array_push(rows, row);
    }

    jce_json_set_child(root, "components", rows);
    jce_scene_destroy(tmp);
    return emit(root, buf, cap);
}

/* ── scene / entity ──────────────────────────────────────────────── */

typedef struct {
    const JceScene *scene;
    JceJson        *rows;
} SceneWalk;

static void entity_row(JceScene *s, JceEntity e, void *user)
{
    SceneWalk *w = (SceneWalk *)user;
    JceJson *row = jce_json_object();
    JceJson *comps = jce_json_array();
    const char *name;
    int id, count;

    jce_json_set_number(row, "id", (double)e);
    name = jce_scene_entity_name(s, e);
    jce_json_set_string(row, "name", name ? name : "");
    jce_json_set_number(row, "parent", (double)jce_scene_get_parent(s, e));

    count = jce_component_count();
    for (id = 0; id < count; ++id) {
        const JceComponentDesc *d = jce_component_desc(id);
        if (d && d->has && d->has(s, e) && d->name)
            jce_json_array_push_string(comps, d->name);
    }
    jce_json_set_child(row, "components", comps);
    jce_json_array_push(w->rows, row);
}

size_t JCE_CALL jce_introspect_scene_json(const JceScene *scene, int depth,
                                          char *buf, size_t cap)
{
    JceJson *root = jce_json_object();
    JceJson *rows = jce_json_array();
    SceneWalk w;

    jce_json_set_string(root, "engine_version", jce_api_version_string());
    if (!scene) {
        jce_json_set_bool(root, "loaded", false);
        jce_json_set_string(root, "note", "no scene is loaded in this process");
        jce_json_set_child(root, "entities", rows);
        return emit(root, buf, cap);
    }
    jce_json_set_bool(root, "loaded", true);
    /* `depth` is accepted for interface compatibility with the flat walk; the
     * rows carry `parent`, so a caller builds whatever depth it wants without
     * this function deciding for it.  Recorded rather than silently ignored. */
    jce_json_set_int(root, "requested_depth", depth);
    jce_json_set_string(root, "shape",
                        "flat rows with parent ids; the caller assembles the "
                        "tree, so no depth is imposed here");

    w.scene = scene;
    w.rows = rows;
    jce_scene_each_entity((JceScene *)scene, entity_row, &w);

    jce_json_set_int(root, "entity_count", jce_json_array_size(rows));
    jce_json_set_child(root, "entities", rows);
    return emit(root, buf, cap);
}

size_t JCE_CALL jce_introspect_entity_json(const JceScene *scene,
                                           JceEntity entity,
                                           char *buf, size_t cap)
{
    JceJson *root = jce_json_object();
    JceJson *comps = jce_json_array();
    const char *name;
    int id, count;

    if (!scene) {
        jce_json_set_string(root, "error", "no scene");
        return emit(root, buf, cap);
    }
    jce_json_set_number(root, "id", (double)entity);
    name = jce_scene_entity_name(scene, entity);
    jce_json_set_string(root, "name", name ? name : "");
    jce_json_set_number(root, "parent",
                        (double)jce_scene_get_parent(scene, entity));

    count = jce_component_count();
    for (id = 0; id < count; ++id) {
        const JceComponentDesc *d = jce_component_desc(id);
        JceJson *out;
        JceJson *item;

        if (!d || !d->has || !d->has(scene, entity) || !d->serialize)
            continue;
        out = jce_json_array();
        d->serialize((JceScene *)scene, entity, (cJSON *)out);
        /* The serialiser may emit more than one object for a single row (the
         * unified Light writes its compat spellings), so every element is
         * carried across rather than only the first.  Re-reading the first
         * child each pass is correct precisely because detach removes it. */
        while ((item = jce_json_first_child(out)) != NULL) {
            jce_json_detach(out, item);
            jce_json_array_push(comps, item);
        }
        jce_json_free(out);
    }
    jce_json_set_child(root, "components", comps);
    return emit(root, buf, cap);
}

/* ── stats ───────────────────────────────────────────────────────── */

static void count_one(JceScene *s, JceEntity e, void *user)
{
    (void)s;
    (void)e;
    *(int *)user += 1;
}

size_t JCE_CALL jce_introspect_stats_json(const JceScene *scene,
                                          char *buf, size_t cap)
{
    JceJson *root = jce_json_object();
    JceJson *phases = jce_json_array();
    uint64_t cur = 0, peak = 0;

    jce_json_set_string(root, "engine_version", jce_api_version_string());

    /* AN ABSENT MEASUREMENT IS FLAGGED, NEVER ZEROED.  A zero entity count or
     * a zero frame time is a number a caller compares against a budget and
     * concludes everything is fine; "there was nothing to measure" is a
     * different fact and has to look different.  It is spelled as an explicit
     * `_available` boolean plus a reason rather than as JSON null, because
     * the JSON facade this file is restricted to has no null constructor and
     * reaching past it for one would put this TU on the cJSON whitelist for a
     * single literal. */
    if (scene) {
        int n = 0;
        jce_scene_each_entity((JceScene *)scene, count_one, &n);
        jce_json_set_bool(root, "entity_count_available", true);
        jce_json_set_int(root, "entity_count", n);
    } else {
        jce_json_set_bool(root, "entity_count_available", false);
        jce_json_set_string(root, "entity_count_absent_because",
                            "no scene was passed");
    }

    jce_mem_profile_get_total(&cur, &peak);
    jce_json_set_number(root, "memory_current_bytes", (double)cur);
    jce_json_set_number(root, "memory_peak_bytes", (double)peak);

    if (!jce_perf_phase_enabled() || !jce_perf_phase_frame_has_data()) {
        jce_json_set_bool(root, "frame_phases_available", false);
        jce_json_set_string(root, "frame_phases_absent_because",
                            jce_perf_phase_enabled()
                                ? "the profiler is on but has no frame data yet"
                                : "the profiler is off (jce_perf_phase_set_enabled)");
    } else {
        jce_json_set_bool(root, "frame_phases_available", true);
        int i, n = jce_perf_phase_count();
        for (i = 0; i < n; ++i) {
            const char *pname = NULL;
            double ms = 0.0;
            if (jce_perf_phase_peek_frame(i, &pname, &ms) && pname) {
                JceJson *row = jce_json_object();
                jce_json_set_string(row, "phase", pname);
                jce_json_set_number(row, "ms", ms);
                jce_json_array_push(phases, row);
            }
        }
        jce_json_set_child(root, "frame_phases", phases);
        phases = NULL;
    }
    if (phases)
        jce_json_free(phases);

    return emit(root, buf, cap);
}
