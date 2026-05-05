/*
 * jce_game_module.c  Registry + default ECS-only game module.
 *
 * The default module's update() ticks the active scene via
 * jce_scene_update; render is left to whoever owns the camera (the
 * editor's game-view path or the standalone JCE_MAIN draw loop).
 */

#include <jce/runtime/jce_game_module.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_log.h>

#include <string.h>

#define LOG_TAG "game_module"
#define MAX_MODULES 16

/* Editor and CK both need a way to tell the default module which scene
 * to tick.  We expose a static setter (private to the engine, visible
 * to the editor through the same symbol). */
static JceScene *s_active_scene = NULL;

/* Public so the editor can hand us the scene pointer right before Play
 * fires.  Declared here to avoid expanding the public header before the
 * design stabilizes. */
JCE_API void jce_game_module_set_active_scene(JceScene *scene);
void jce_game_module_set_active_scene(JceScene *scene)
{
    s_active_scene = scene;
}

/* ── default module ───────────────────────────────────────────────── */

static bool default_init(const JceServices *svc, void *ud)
{
    (void)svc; (void)ud;
    return true;
}
static void default_exit(void *ud) { (void)ud; }
static void default_update(float dt, void *ud)
{
    (void)ud;
    if (s_active_scene && dt > 0.0f)
        jce_scene_update(s_active_scene, dt);
}
static void default_draw(const JceServices *svc, void *ud) { (void)svc; (void)ud; }
static void default_on_event(const JceEvent *ev, void *ud) { (void)ev; (void)ud; }
static bool default_should_quit(void *ud) { (void)ud; return false; }

static const JceGameModule s_default_module = {
    "Default (ECS Tick)",
    default_init,
    default_exit,
    default_update,
    default_draw,
    default_on_event,
    NULL,                /* on_resize */
    default_should_quit,
    NULL,                /* user_data */
    false,
    0, 0,
};

const JceGameModule *jce_game_module_default(void)
{
    return &s_default_module;
}

/* ── registry ─────────────────────────────────────────────────────── */

typedef struct ModuleEntry {
    const char           *name;
    const JceGameModule  *desc;
} ModuleEntry;

static ModuleEntry s_modules[MAX_MODULES];
static int         s_count = 0;
static bool        s_default_seeded = false;

static void seed_default_once(void)
{
    if (s_default_seeded) return;
    s_modules[0].name = s_default_module.name;
    s_modules[0].desc = &s_default_module;
    s_count = 1;
    s_default_seeded = true;
}

bool jce_game_module_register(const char *name, const JceGameModule *desc)
{
    if (!name || !desc) return false;
    seed_default_once();
    for (int i = 0; i < s_count; ++i) {
        if (s_modules[i].name && strcmp(s_modules[i].name, name) == 0) {
            LOG_WARN(LOG_TAG, "duplicate registration for module '%s'", name);
            return false;
        }
    }
    if (s_count >= MAX_MODULES) {
        LOG_WARN(LOG_TAG, "registry full (max=%d), rejecting '%s'",
                 MAX_MODULES, name);
        return false;
    }
    s_modules[s_count].name = name;
    s_modules[s_count].desc = desc;
    s_count++;
    LOG_INFO(LOG_TAG, "registered module '%s' (slot=%d)", name, s_count - 1);
    return true;
}

const JceGameModule *jce_game_module_find(const char *name)
{
    if (!name) return NULL;
    seed_default_once();
    for (int i = 0; i < s_count; ++i) {
        if (s_modules[i].name && strcmp(s_modules[i].name, name) == 0)
            return s_modules[i].desc;
    }
    return NULL;
}

int jce_game_module_count(void)
{
    seed_default_once();
    return s_count;
}

const char *jce_game_module_name_at(int idx)
{
    seed_default_once();
    if (idx < 0 || idx >= s_count) return NULL;
    return s_modules[idx].name;
}

const JceGameModule *jce_game_module_at(int idx)
{
    seed_default_once();
    if (idx < 0 || idx >= s_count) return NULL;
    return s_modules[idx].desc;
}
