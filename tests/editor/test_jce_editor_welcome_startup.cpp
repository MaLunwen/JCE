#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "core/jce_editor_config.h"
#include "core/jce_editor_project.h"
#include "core/jce_editor_state.h"
#include "ui/jce_editor_welcome_policy.h"
#include <cstring>
#include <initializer_list>

/* Counted state/configuration ports; the policy and layout admission bodies
 * below are the actual production owners, not a second startup decision. */
static JceProject project_value{};
static const JceProject *current_project = nullptr;
static const char *current_scene_path = nullptr;
static bool scene_loading = false;
static bool config_available = false;
static JceEditorConfig config_value{};
static JceEditorStartupBehavior startup_behavior = JCE_EDITOR_STARTUP_LAST;
static int project_reads = 0, path_reads = 0, loading_reads = 0;
static int config_reads = 0, preference_reads = 0;
static bool s_show_welcome = false;
static bool s_welcome_startup_resolved = false;

extern "C" const JceProject *jce_editor_project_get(void)
{ ++project_reads; return current_project; }
extern "C" const char *jce_state_get_current_scene_path(void)
{ ++path_reads; return current_scene_path; }
extern "C" bool jce_state_is_scene_loading(void)
{ ++loading_reads; return scene_loading; }
extern "C" bool jce_editor_config_load(JceEditorConfig *out)
{ ++config_reads; if (config_available) *out = config_value; return config_available; }
extern "C" JceEditorStartupBehavior jce_editor_prefs_startup_behavior(void)
{ ++preference_reads; return startup_behavior; }

#include JCE_TEST_EDITOR_WELCOME_STARTUP_OWNER

static void reset()
{
    project_value = {};
    current_project = nullptr;
    current_scene_path = nullptr;
    scene_loading = config_available = false;
    config_value = {};
    startup_behavior = JCE_EDITOR_STARTUP_LAST;
    project_reads = path_reads = loading_reads = config_reads = preference_reads = 0;
    s_show_welcome = s_welcome_startup_resolved = false;
}

TEST_CASE("startup policy preserves preference and known-project fallback")
{
    struct Case { JceEditorStartupBehavior behavior; bool known; bool expected; };
    const Case cases[] = {
        {JCE_EDITOR_STARTUP_LAST, false, true},
        {JCE_EDITOR_STARTUP_LAST, true, false},
        {JCE_EDITOR_STARTUP_EMPTY, false, false},
        {JCE_EDITOR_STARTUP_EMPTY, true, false},
        {JCE_EDITOR_STARTUP_PICKER, false, true},
        {JCE_EDITOR_STARTUP_PICKER, true, true},
        {static_cast<JceEditorStartupBehavior>(99), false, true},
        {static_cast<JceEditorStartupBehavior>(99), true, false},
    };
    for (const auto &c : cases) {
        CHECK(jce_editor_welcome_should_open_on_startup(c.behavior, false, c.known) == c.expected);
        CHECK_FALSE(jce_editor_welcome_should_open_on_startup(c.behavior, true, c.known));
    }
}

TEST_CASE("actual startup admission recognizes loaded Bundle and scene without a project")
{
    for (const char *path : {"bundle://normal.jbundle", "scenes/selected.scene.json"}) {
        for (auto behavior : {JCE_EDITOR_STARTUP_LAST, JCE_EDITOR_STARTUP_EMPTY, JCE_EDITOR_STARTUP_PICKER}) {
            reset();
            current_scene_path = path;
            startup_behavior = behavior;
            resolve_startup_welcome_once();
            CHECK_FALSE(s_show_welcome);
            CHECK(s_welcome_startup_resolved);
            CHECK(project_reads == 1);
            CHECK(path_reads == 1);
        }
    }
}

TEST_CASE("actual startup admission recognizes pending scene load and existing project")
{
    reset();
    scene_loading = true;
    resolve_startup_welcome_once();
    CHECK_FALSE(s_show_welcome);
    CHECK(loading_reads == 1);

    reset();
    char project_root[] = "authoring/project";
    project_value.project_root = project_root;
    current_project = &project_value;
    startup_behavior = JCE_EDITOR_STARTUP_PICKER;
    resolve_startup_welcome_once();
    CHECK_FALSE(s_show_welcome);
}

TEST_CASE("no admitted scene retains fresh and known-project startup choices")
{
    for (const char *path : {static_cast<const char *>(nullptr), ""}) {
        reset();
        current_scene_path = path;
        resolve_startup_welcome_once();
        CHECK(s_show_welcome);
    }
    reset();
    current_project = &project_value; // A cached record with no root is not content.
    resolve_startup_welcome_once();
    CHECK(s_show_welcome);

    reset();
    config_available = true;
    std::strcpy(config_value.last_project, "known/project");
    resolve_startup_welcome_once();
    CHECK_FALSE(s_show_welcome);

    reset();
    config_available = true;
    config_value.recent_count = 2;
    std::strcpy(config_value.recent_projects[1], "recent/project");
    resolve_startup_welcome_once();
    CHECK_FALSE(s_show_welcome);

    reset();
    config_available = true;
    std::strcpy(config_value.last_project, "known/project");
    startup_behavior = JCE_EDITOR_STARTUP_PICKER;
    resolve_startup_welcome_once();
    CHECK(s_show_welcome);
}

TEST_CASE("startup resolution is once-only and retains later manual Welcome visibility")
{
    reset();
    current_scene_path = "bundle://normal.jbundle";
    resolve_startup_welcome_once();
    CHECK_FALSE(s_show_welcome);
    const int reads = project_reads + path_reads + loading_reads + config_reads + preference_reads;
    s_show_welcome = true; // The existing Help command opens Welcome explicitly.
    resolve_startup_welcome_once();
    CHECK(s_show_welcome);
    CHECK(project_reads + path_reads + loading_reads + config_reads + preference_reads == reads);

    reset();
    resolve_startup_welcome_once();
    CHECK(s_show_welcome);
    s_show_welcome = false;
    resolve_startup_welcome_once();
    CHECK_FALSE(s_show_welcome);
}
