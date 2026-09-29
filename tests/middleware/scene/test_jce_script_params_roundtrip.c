/* test_jce_script_params_roundtrip.c
 *
 * AN AUTHORED PARAMETER HAS TO SURVIVE SAVING THE SCENE.
 *
 * JceScriptComponent now carries up to JCE_SCRIPT_PARAM_MAX author-declared
 * parameters -- Unity's [SerializeField], Godot's @export.  Three properties
 * of that are easy to get wrong and silent when they are:
 *
 *   1. An ENTITY-kind parameter is a cross-entity reference, and entity ids
 *      are NOT stable once a scene is instantiated into another.  A parameter
 *      that stores a raw id and joins no map comes back pointing at whatever
 *      entity now holds that number.  "target != 0" cannot see that -- a
 *      stale id is non-zero too.
 *
 *      A LOAD CANNOT TEST THE MAPPING, and the first version of this file
 *      believed it could.  A load REPLACES the target scene, so ids are
 *      handed out in serialisation order and reproduce the source's exactly;
 *      "it points at Hero" then holds with the remap deleted.  The mutation
 *      control is what said so: removing the remap failed the DANGLING case
 *      and left the by-name one green, which is only possible if the by-name
 *      one was vacuous.  The mapping is settled here on the APPEND path
 *      instead, where the target already holds entities and the ids cannot
 *      line up -- two instances of one prefab, each of which must reference
 *      its own copy.
 *   2. The serialiser writes number, entity AND text on every row whatever
 *      the kind says, so that flipping the kind in the Inspector does not
 *      throw away what the author typed under the other one.  Writing only
 *      the field the kind names would pass a naive round trip (each kind
 *      keeps its own value) and lose data on the one edit that matters.
 *   3. `param_count` is not trustworthy input.  The component is reachable
 *      through the Automation API, through a script, and through a
 *      hand-edited scene file, so both halves clamp -- and the clamp on the
 *      SERIALISER is the one that would otherwise read off the end of an
 *      eight-element array.
 *
 * WHY THE COUNTING ASSERTIONS USE "number".  Of the five keys a parameter row
 * writes, `number` is the only one that no other component's serialiser uses
 * ("params" itself is also a MeshRenderer key, and "name"/"kind"/"text" are
 * common).  Counting it in the saved text is therefore a count of parameter
 * ROWS WRITTEN, which is exactly the quantity the clamp is about -- and a
 * quantity a reload cannot report, because the parser clamps too and would
 * hand back a legal-looking 8 either way.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory.
 */
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_prefab.h>
#include <jce/resource/jce_scene_serial.h>

#include "unity.h"

#include <stdio.h>
#include <string.h>

#define PREFAB_FILE "test_script_params_squad.prefab.json"

/* The two children of one prefab instance, told apart by which one carries
 * the Script component.  Names cannot do it: a second instance's "Hero"
 * is uniquified by jce_scene_name_set_unique, so a name lookup finds the
 * FIRST instance's -- which is the very confusion this case is about. */
typedef struct {
    JceEntity root;
    JceEntity scripted;
    JceEntity other;
} Kids;

void setUp(void)    {}
void tearDown(void) { remove(PREFAB_FILE); }

static JceEntity spawn(JceScene *s, const char *name)
{
    JceEntity e = jce_scene_create_entity(s, name);
    JceTransform t;
    memset(&t, 0, sizeof t);
    t.rotation.w = 1.0f;
    t.scale = jce_v3(1.0f, 1.0f, 1.0f);
    jce_scene_set_transform(s, e, &t);
    return e;
}

typedef struct { const char *want; JceEntity found; } NameSearch;

static void name_probe(JceScene *s, JceEntity e, void *user)
{
    NameSearch *q = (NameSearch *)user;
    if (q->found != JCE_ENTITY_INVALID) return;
    const char *nm = jce_scene_entity_name(s, e);
    if (nm && strcmp(nm, q->want) == 0) q->found = e;
}

static JceEntity by_name(JceScene *s, const char *name)
{
    NameSearch q; q.want = name; q.found = JCE_ENTITY_INVALID;
    jce_scene_each_entity(s, name_probe, &q);
    return q.found;
}

static char *save(JceScene *s, size_t *len)
{
    char *json = jce_scene_serial_save(s, len);
    TEST_ASSERT_NOT_NULL_MESSAGE(json, "saving the scene failed outright");
    return json;
}

/* Parameter rows in the saved text.  See the file header for why `number`. */
static int count_rows(const char *json)
{
    int n = 0;
    const char *p = json;
    while ((p = strstr(p, "\"number\"")) != NULL) { ++n; p += 8; }
    return n;
}

static void set_param(JceScriptComponent *sc, int i, const char *name,
                      JceScriptParamKind kind, float number,
                      uint64_t entity, const char *text)
{
    JceScriptParam *pm = &sc->params[i];
    snprintf(pm->name, sizeof pm->name, "%s", name);
    pm->kind   = (uint32_t)kind;
    pm->number = number;
    pm->entity = entity;
    snprintf(pm->text, sizeof pm->text, "%s", text);
}

static JceScriptComponent *reload(JceScene *s, JceScene **out_r,
                                  const char *entity_name)
{
    size_t len = 0;
    char *json = save(s, &len);
    JceScene *r = jce_scene_create();
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_TRUE_MESSAGE(jce_scene_serial_load(r, json, len),
        "loading the saved scene failed");
    jce_json_free_string(json);
    JceEntity e = by_name(r, entity_name);
    TEST_ASSERT_TRUE(e != JCE_ENTITY_INVALID);
    *out_r = r;
    return jce_scene_get_script(r, e);
}

/* ---------------------------------------------------------------------- */

static void test_every_kind_survives_and_the_entity_is_remapped(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    /* THE IDS WILL COINCIDE, AND THAT IS STRUCTURAL.  A load REPLACES -- every
     * entity the caller had is destroyed first (jce_scene_components_json.c:
     * 2402) -- so the target is empty when the load starts and hands out ids
     * in serialisation order, reproducing the source scene's.  src_id ==
     * new_id is therefore a property of THIS PATH, and no setup in the source
     * scene changes it: a version of this case that created and destroyed
     * five entities first still came back with Hero's original id.
     *
     * So the by-name assertion below CORROBORATES; the DANGLING one carries
     * this case, because an id that exists in neither scene cannot coincide.
     * The mapping itself -- right entity versus wrong entity -- is settled by
     * test_two_prefab_instances_do_not_share_one_entity_reference, on the
     * APPEND path, where the target is not empty and the ids cannot line up. */
    (void)spawn(s, "Decoy");
    JceEntity hero   = spawn(s, "Hero");
    JceEntity turret = spawn(s, "Turret");

    JceScriptComponent sc;
    memset(&sc, 0, sizeof sc);
    snprintf(sc.script_path, sizeof sc.script_path, "%s", "turret.lua");
    set_param(&sc, 0, "speed",  JCE_SCRIPT_PARAM_NUMBER, 12.5f, 0, "");
    set_param(&sc, 1, "armed",  JCE_SCRIPT_PARAM_BOOL,   1.0f,  0, "");
    set_param(&sc, 2, "label",  JCE_SCRIPT_PARAM_TEXT,   0.0f,  0, "Watchtower");
    set_param(&sc, 3, "target", JCE_SCRIPT_PARAM_ENTITY, 0.0f, (uint64_t)hero, "");
    /* A DANGLING reference, deliberately.  No entity in this scene has this
     * id, so the loader cannot resolve it -- and the rule every other
     * reference here follows is that it becomes 0, because pointing a turret
     * at a rock is worse than pointing it at nothing. */
    set_param(&sc, 4, "backup", JCE_SCRIPT_PARAM_ENTITY, 0.0f, 999999u, "");
    sc.param_count = 5;
    jce_scene_set_script(s, turret, &sc);

    JceScene *r = NULL;
    JceScriptComponent *got = reload(s, &r, "Turret");
    TEST_ASSERT_NOT_NULL_MESSAGE(got,
        "the Script component did not come back at all");

    JceEntity r_hero = by_name(r, "Hero");
    TEST_ASSERT_TRUE(r_hero != JCE_ENTITY_INVALID);

    printf("  saved hero=%llu  reloaded target=%llu  Hero=%llu  count=%d\n",
           (unsigned long long)hero, (unsigned long long)got->params[3].entity,
           (unsigned long long)r_hero, got->param_count);

    TEST_ASSERT_EQUAL_INT_MESSAGE(5, got->param_count, "param_count changed");
    TEST_ASSERT_EQUAL_STRING("turret.lua", got->script_path);

    TEST_ASSERT_EQUAL_STRING("speed", got->params[0].name);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)JCE_SCRIPT_PARAM_NUMBER, got->params[0].kind);
    TEST_ASSERT_EQUAL_FLOAT(12.5f, got->params[0].number);

    TEST_ASSERT_EQUAL_STRING("armed", got->params[1].name);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)JCE_SCRIPT_PARAM_BOOL, got->params[1].kind);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, got->params[1].number);

    TEST_ASSERT_EQUAL_STRING("label", got->params[2].name);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)JCE_SCRIPT_PARAM_TEXT, got->params[2].kind);
    TEST_ASSERT_EQUAL_STRING("Watchtower", got->params[2].text);

    /* Corroborating, not discriminating -- see the note in the setup. */
    TEST_ASSERT_EQUAL_STRING("target", got->params[3].name);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)JCE_SCRIPT_PARAM_ENTITY, got->params[3].kind);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE((uint64_t)r_hero, got->params[3].entity,
        "the ENTITY parameter points at a different entity after loading -- "
        "it was not put through the loader's src_id -> new_id remap, so the "
        "turret now targets whatever holds that number");

    TEST_ASSERT_EQUAL_UINT64_MESSAGE(0u, got->params[4].entity,
        "an unresolvable entity reference must become 0, not survive as a "
        "number that now names an unrelated entity");

    jce_scene_destroy(r);
    jce_scene_destroy(s);
}

static void test_a_kind_does_not_erase_the_other_values(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity e = spawn(s, "Keeper");

    JceScriptComponent sc;
    memset(&sc, 0, sizeof sc);
    snprintf(sc.script_path, sizeof sc.script_path, "%s", "keeper.lua");
    /* ONE row, kind TEXT, but every slot filled.  A serialiser that wrote
     * only the field the kind names would pass every assertion in the test
     * above and lose both of these -- so that an author who typed a number,
     * switched the row to Text and switched back would find a zero. */
    set_param(&sc, 0, "mode", JCE_SCRIPT_PARAM_TEXT, 3.25f, 4242u, "patrol");
    sc.param_count = 1;
    jce_scene_set_script(s, e, &sc);

    JceScene *r = NULL;
    JceScriptComponent *got = reload(s, &r, "Keeper");
    TEST_ASSERT_NOT_NULL(got);

    TEST_ASSERT_EQUAL_STRING("patrol", got->params[0].text);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(3.25f, got->params[0].number,
        "the number under a TEXT row was dropped; flipping the kind in the "
        "Inspector would silently discard what the author typed");
    /* 4242 is not an entity in this scene, so the remap zeroes it -- and it
     * must do so for the ENTITY kind only.  This row is TEXT, so the value
     * is inert data and has to come back verbatim. */
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(4242u, got->params[0].entity,
        "the entity field under a TEXT row was remapped or dropped; the remap "
        "must only touch rows whose kind IS entity");

    jce_scene_destroy(r);
    jce_scene_destroy(s);
}

static void test_a_script_with_no_parameters_writes_no_rows(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity e = spawn(s, "Plain");

    JceScriptComponent sc;
    memset(&sc, 0, sizeof sc);
    snprintf(sc.script_path, sizeof sc.script_path, "%s", "plain.lua");
    jce_scene_set_script(s, e, &sc);

    size_t len = 0;
    char *json = save(s, &len);
    /* Every scene authored before this feature existed is this case, and it
     * has to serialise to the bytes it did before -- otherwise the whole
     * corpus of existing scenes shows up as modified. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count_rows(json),
        "a script with no parameters wrote parameter rows anyway");
    jce_json_free_string(json);

    JceScene *r = NULL;
    JceScriptComponent *got = reload(s, &r, "Plain");
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQUAL_INT(0, got->param_count);

    jce_scene_destroy(r);
    jce_scene_destroy(s);
}

static void test_a_hostile_param_count_is_clamped_by_the_serialiser(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity e = spawn(s, "Hostile");

    JceScriptComponent sc;
    memset(&sc, 0, sizeof sc);
    snprintf(sc.script_path, sizeof sc.script_path, "%s", "hostile.lua");
    for (int i = 0; i < JCE_SCRIPT_PARAM_MAX; ++i) {
        char nm[8];
        snprintf(nm, sizeof nm, "p%d", i);
        set_param(&sc, i, nm, JCE_SCRIPT_PARAM_NUMBER, (float)i, 0, "");
    }
    /* Not reachable through the Inspector, which clamps -- but reachable
     * through the Automation API, through a script, and through a
     * hand-edited scene file. */
    sc.param_count = 99;
    jce_scene_set_script(s, e, &sc);

    size_t len = 0;
    char *json = save(s, &len);
    const int rows = count_rows(json);
    printf("  param_count=99, rows written=%d (max %d)\n",
           rows, JCE_SCRIPT_PARAM_MAX);
    TEST_ASSERT_EQUAL_INT_MESSAGE(JCE_SCRIPT_PARAM_MAX, rows,
        "the serialiser wrote a row per claimed parameter instead of clamping "
        "-- which means it also READ past the end of an eight-element array");
    jce_json_free_string(json);

    JceScene *r = NULL;
    JceScriptComponent *got = reload(s, &r, "Hostile");
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQUAL_INT(JCE_SCRIPT_PARAM_MAX, got->param_count);
    TEST_ASSERT_EQUAL_STRING("p0", got->params[0].name);
    /* Spelled from the constant, not typed: "p7" in the source would become a
     * lie the day JCE_SCRIPT_PARAM_MAX moves, and it would fail for a reason
     * that has nothing to do with what this case is about. */
    char last[8];
    snprintf(last, sizeof last, "p%d", JCE_SCRIPT_PARAM_MAX - 1);
    TEST_ASSERT_EQUAL_STRING(last, got->params[JCE_SCRIPT_PARAM_MAX - 1].name);

    jce_scene_destroy(r);
    jce_scene_destroy(s);
}


/* ---------------------------------------------------------------------- */

/* THE CASE WHERE THE MAP IS LOAD-BEARING.
 *
 * A prefab instantiates ADDITIVELY into a scene that already holds entities,
 * so the ids it was saved under are taken, or belong to something unrelated,
 * or do not exist.  Two properties that a load can never test:
 *
 *   1. each instance's reference points at ITS OWN copy, not at the first
 *      instance's.  A map built once and reused -- or a reference resolved
 *      against the SOURCE ids -- gives both turrets the same hero, and the
 *      second squad then defends the first squad's hero forever.
 *   2. neither points at the id the prefab was authored under, which in the
 *      target scene names something else entirely.
 *
 * Both are invisible in the load case above, where the ids coincide.
 */
static void find_kids(JceScene *s, JceEntity e, void *user)
{
    Kids *k = (Kids *)user;
    if (jce_scene_get_parent(s, e) != k->root) return;
    if (jce_scene_get_script(s, e)) k->scripted = e;
    else                            k->other    = e;
}

static Kids kids_of(JceScene *s, JceEntity root)
{
    Kids k;
    k.root = root;
    k.scripted = JCE_ENTITY_INVALID;
    k.other = JCE_ENTITY_INVALID;
    jce_scene_each_entity(s, find_kids, &k);
    return k;
}

static void test_two_prefab_instances_do_not_share_one_entity_reference(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    JceEntity squad  = spawn(s, "Squad");
    JceEntity hero   = spawn(s, "Hero");
    JceEntity turret = spawn(s, "Turret");
    jce_scene_set_parent(s, hero, squad);
    jce_scene_set_parent(s, turret, squad);

    JceScriptComponent sc;
    memset(&sc, 0, sizeof sc);
    snprintf(sc.script_path, sizeof sc.script_path, "%s", "turret.lua");
    set_param(&sc, 0, "target", JCE_SCRIPT_PARAM_ENTITY, 0.0f,
              (uint64_t)hero, "");
    sc.param_count = 1;
    jce_scene_set_script(s, turret, &sc);

    TEST_ASSERT_TRUE_MESSAGE(jce_prefab_save_subtree(s, squad, PREFAB_FILE),
        "saving the prefab subtree failed outright");

    /* A populated target, so the prefab's authored ids are already spoken
     * for.  This is the whole reason the case can discriminate. */
    JceScene *t = jce_scene_create();
    TEST_ASSERT_NOT_NULL(t);
    (void)spawn(t, "Ground");
    (void)spawn(t, "Sun");
    (void)spawn(t, "Camera");

    JceEntity a = jce_prefab_instantiate_file(t, PREFAB_FILE, NULL);
    JceEntity b = jce_prefab_instantiate_file(t, PREFAB_FILE, NULL);
    TEST_ASSERT_TRUE_MESSAGE(a != 0 && b != 0,
        "instantiating the prefab failed");
    TEST_ASSERT_TRUE_MESSAGE(a != b, "both instances came back as one entity");

    Kids ka = kids_of(t, a);
    Kids kb = kids_of(t, b);
    TEST_ASSERT_TRUE_MESSAGE(ka.scripted != JCE_ENTITY_INVALID &&
                             ka.other    != JCE_ENTITY_INVALID &&
                             kb.scripted != JCE_ENTITY_INVALID &&
                             kb.other    != JCE_ENTITY_INVALID,
        "an instance did not come back with both children -- the rest of this "
        "case would be comparing absences");

    JceScriptComponent *sa = jce_scene_get_script(t, ka.scripted);
    JceScriptComponent *sb = jce_scene_get_script(t, kb.scripted);
    TEST_ASSERT_NOT_NULL(sa);
    TEST_ASSERT_NOT_NULL(sb);
    TEST_ASSERT_EQUAL_INT(1, sa->param_count);
    TEST_ASSERT_EQUAL_INT(1, sb->param_count);

    printf("  authored hero=%llu  A: hero=%llu target=%llu  "
           "B: hero=%llu target=%llu\n",
           (unsigned long long)hero,
           (unsigned long long)ka.other, (unsigned long long)sa->params[0].entity,
           (unsigned long long)kb.other, (unsigned long long)sb->params[0].entity);

    TEST_ASSERT_TRUE_MESSAGE(ka.other != kb.other,
        "the two instances share one hero entity -- they are not two copies, "
        "and nothing below would mean anything");

    TEST_ASSERT_EQUAL_UINT64_MESSAGE((uint64_t)ka.other, sa->params[0].entity,
        "instance A's turret does not point at instance A's hero");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE((uint64_t)kb.other, sb->params[0].entity,
        "instance B's turret points somewhere other than its own hero -- a "
        "map built once and reused, or a reference resolved against the "
        "SOURCE ids, gives both turrets the first instance's hero");

    jce_scene_destroy(t);
    jce_scene_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_every_kind_survives_and_the_entity_is_remapped);
    RUN_TEST(test_a_kind_does_not_erase_the_other_values);
    RUN_TEST(test_a_script_with_no_parameters_writes_no_rows);
    RUN_TEST(test_a_hostile_param_count_is_clamped_by_the_serialiser);
    RUN_TEST(test_two_prefab_instances_do_not_share_one_entity_reference);
    return UNITY_END();
}
