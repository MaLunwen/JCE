/*
 * test_jce_runtime_audio_source_control.c — the authored AudioSource, driven.
 *
 * BEFORE THIS: a JceAudioSourceComponent sounded EXACTLY ONCE, at scene spawn,
 * and only if `playOnAwake` was set.  Nothing anywhere -- not the engine, not
 * the editor, not any of the seven script backends -- could start, stop or
 * restart one afterwards.  A door that creaks when it opens, a gun that fires,
 * an alarm a script arms, could not use the component at all: the only
 * alternative, jce.play_sound(path, ...), is a fire-and-forget one-shot that
 * discards everything the component authors -- loop, pitch, per-source volume,
 * mixer bus, and the entire 3D attenuation block.
 *
 * THE OBSERVABLE IS jce_runtime_audio_is_playing plus rt->voice_count.  The
 * first says "is this entity's authored source sounding"; the second is what
 * distinguishes RESTART from STACK, which no is_playing query can tell apart --
 * two voices and one voice both answer `true`.
 *
 * The audio engine is real (every JceAudio is a device-less engine summed by
 * one shared output device, so this needs no sound card) and the clips are
 * uploaded PCM, looping, so "still playing" is a property of the runtime's
 * bookkeeping and not of how fast the machine drained a buffer.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "application/jce_rt_internal.h"

#include "unity.h"

#include <jce/application/jce_runtime.h>
#include <jce/middleware/audio/jce_audio.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_json.h>

#include "jce_test_file_util.h"

#include <stdio.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* One entity per authored state.  `Silent` is the case the feature exists for:
 * a source the author does NOT want on at spawn, which was therefore
 * unreachable forever. */
static const char *const SCENE_JSON =
"{\"format_version\":1,\"entities\":["
 "{\"name\":\"Silent\",\"id\":1,\"parent_id\":0,\"components\":["
   "{\"type\":\"Transform\",\"posX\":0,\"posY\":0,\"posZ\":0,"
    "\"rotX\":0,\"rotY\":0,\"rotZ\":0,\"scaleX\":1,\"scaleY\":1,\"scaleZ\":1},"
   "{\"type\":\"AudioSource\",\"clipPath\":\"sfx/creak.wav\",\"loop\":true,"
    "\"playOnAwake\":false,\"volume\":0.5,\"pitch\":1.0}]},"
 "{\"name\":\"Awake\",\"id\":2,\"parent_id\":0,\"components\":["
   "{\"type\":\"Transform\",\"posX\":0,\"posY\":0,\"posZ\":0,"
    "\"rotX\":0,\"rotY\":0,\"rotZ\":0,\"scaleX\":1,\"scaleY\":1,\"scaleZ\":1},"
   "{\"type\":\"AudioSource\",\"clipPath\":\"sfx/hum.wav\",\"loop\":true,"
    "\"playOnAwake\":true,\"volume\":0.5,\"pitch\":1.0}]},"
 "{\"name\":\"Mute\",\"id\":3,\"parent_id\":0,\"components\":["
   "{\"type\":\"Transform\",\"posX\":0,\"posY\":0,\"posZ\":0,"
    "\"rotX\":0,\"rotY\":0,\"rotZ\":0,\"scaleX\":1,\"scaleY\":1,\"scaleZ\":1}]}"
"]}";

/* The `id` in scene JSON is an AUTHORING id and is remapped on load -- the
 * three above come back as 456/457/458 here.  Scripts get the runtime id from
 * jce.find_by_name / jce.spawn, so that is the currency these calls take, and
 * a test that hard-coded 1/2/3 would be asking about entities that do not
 * exist and passing its refusal cases for the wrong reason. */
static JceEntity E_SILENT, E_AWAKE, E_MUTE;

typedef struct { const char *want; JceEntity found; } NameLook;

static void name_cb(JceScene *s, JceEntity e, void *ud)
{
    NameLook *l = (NameLook *)ud;
    const char *n = jce_scene_entity_name(s, e);
    if (n && strcmp(n, l->want) == 0) l->found = e;
}

static JceEntity by_name(JceScene *s, const char *name)
{
    NameLook l = { name, 0 };
    jce_scene_each_entity(s, name_cb, &l);
    TEST_ASSERT_TRUE_MESSAGE(l.found != 0,
        "the fixture entity must exist -- otherwise every refusal below passes "
        "because the entity is missing, not because the guard works");
    return l.found;
}

/* Each load hands back a DISTINCT sound: rt->voices owns the clip it started
 * and unloads it when the voice is stopped, so a probe that returned one
 * shared handle would leave the second play pointing at freed state -- and the
 * test would be asserting about a use-after-free, not about the feature. */
typedef struct { JceAudio *audio; int calls; } Probe;

static uint32_t probe_load(void *user, JceAudio *audio, const char *path)
{
    Probe *p = (Probe *)user;
    static const int16_t pcm[480] = { 0 };      /* 10 ms of stereo silence */
    (void)path;
    p->calls++;
    return jce_audio_load_pcm(audio, pcm, (uint32_t)sizeof pcm, 2u, 48000u, 16u);
}

static JceScene   *s_scene;
static JceRuntime *s_rt;
static JceAudio   *s_audio;
static Probe       s_probe;

static void boot(void)
{
    JceJson *root = jce_json_parse(SCENE_JSON, strlen(SCENE_JSON));
    TEST_ASSERT_NOT_NULL_MESSAGE(root, "the fixture scene must parse");
    s_scene = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s_scene);
    /* The return is the ENTITY COUNT, not a bool: three here, and asserting
     * the exact number is what would catch a fixture that silently lost one. */
    TEST_ASSERT_EQUAL_INT(3, jce_scene_load_json(s_scene, root));
    jce_json_free(root);

    E_SILENT = by_name(s_scene, "Silent");
    E_AWAKE  = by_name(s_scene, "Awake");
    E_MUTE   = by_name(s_scene, "Mute");

    s_audio = jce_audio_create();
    TEST_ASSERT_NOT_NULL_MESSAGE(s_audio,
        "every JceAudio is device-less; create must succeed with no sound card");
    s_probe.audio = s_audio;
    s_probe.calls = 0;

    JceRuntimeDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.scene         = s_scene;
    desc.audio         = s_audio;
    desc.audio_load_fn = probe_load;
    desc.user_data     = &s_probe;
    s_rt = jce_runtime_create(&desc);
    TEST_ASSERT_NOT_NULL(s_rt);
}

/* Drop the runtime and the audio engine but KEEP the scene: the Lua case adds
 * a Script component and needs a fresh spawn walk over the same entities, and
 * the entity ids it resolved must stay valid. */
static void shutdown_all_keep_scene(void)
{
    jce_runtime_destroy(s_rt);
    jce_audio_destroy(s_audio);
    s_rt = NULL; s_audio = NULL;
}

static void shutdown_all(void)
{
    shutdown_all_keep_scene();
    jce_scene_destroy(s_scene);
    s_scene = NULL;
}

void setUp(void)    { boot(); }
void tearDown(void) { shutdown_all(); }

static void test_play_on_awake_still_decides_what_spawn_starts(void)
{
    /* The spawn walk was factored through rt_audio_source_start; this is the
     * guard that the refactor kept the ONE thing spawn decides.  If it broke
     * open, every authored ambience in every scene would start itself. */
    TEST_ASSERT_TRUE_MESSAGE(jce_runtime_audio_is_playing(s_rt, E_AWAKE),
        "playOnAwake:true must still sound at spawn");
    TEST_ASSERT_FALSE_MESSAGE(jce_runtime_audio_is_playing(s_rt, E_SILENT),
        "playOnAwake:false must still be silent at spawn");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_probe.calls,
        "exactly one clip loaded at spawn -- the awake one");
}

static void test_a_silent_source_can_be_started_and_stopped(void)
{
    /* This is the whole feature.  Before it, every assert below was
     * unreachable: there was no call that could reach an authored source. */
    TEST_ASSERT_TRUE_MESSAGE(jce_runtime_audio_play(s_rt, E_SILENT),
        "an authored, enabled, clip-bearing source must start on request");
    TEST_ASSERT_TRUE(jce_runtime_audio_is_playing(s_rt, E_SILENT));
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, s_probe.calls,
        "the source's OWN clip is loaded -- not a path the caller supplied");

    TEST_ASSERT_TRUE_MESSAGE(jce_runtime_audio_stop(s_rt, E_SILENT),
        "stop must report that it stopped something");
    TEST_ASSERT_FALSE(jce_runtime_audio_is_playing(s_rt, E_SILENT));

    TEST_ASSERT_FALSE_MESSAGE(jce_runtime_audio_stop(s_rt, E_SILENT),
        "a second stop has nothing to stop and must say so");

    TEST_ASSERT_TRUE_MESSAGE(jce_runtime_audio_play(s_rt, E_SILENT),
        "and it must be startable AGAIN -- a door creaks more than once");
    TEST_ASSERT_TRUE(jce_runtime_audio_is_playing(s_rt, E_SILENT));
}

static void test_replay_restarts_rather_than_stacking(void)
{
    /* Unity's Play() on a sounding source restarts it.  Without that, a script
     * firing on a repeating event adds a voice per call until the mixer runs
     * out -- and is_playing cannot see the difference, so the voice table is
     * the only honest witness. */
    TEST_ASSERT_TRUE(jce_runtime_audio_play(s_rt, E_SILENT));
    const int after_first = s_rt->voice_count;
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, after_first,
        "the awake source plus the one just started");

    for (int i = 0; i < 8; ++i)
        TEST_ASSERT_TRUE(jce_runtime_audio_play(s_rt, E_SILENT));

    TEST_ASSERT_EQUAL_INT_MESSAGE(after_first, s_rt->voice_count,
        "eight replays must leave ONE voice for this entity, not nine");
    TEST_ASSERT_TRUE(jce_runtime_audio_is_playing(s_rt, E_SILENT));
}

static void test_stopping_one_source_leaves_the_others_alone(void)
{
    /* The stop path compacts rt->voices in place.  A compaction that drops the
     * wrong rows silences unrelated entities, and nothing else in the suite
     * would notice. */
    TEST_ASSERT_TRUE(jce_runtime_audio_play(s_rt, E_SILENT));
    TEST_ASSERT_TRUE(jce_runtime_audio_is_playing(s_rt, E_AWAKE));

    TEST_ASSERT_TRUE(jce_runtime_audio_stop(s_rt, E_SILENT));
    TEST_ASSERT_FALSE(jce_runtime_audio_is_playing(s_rt, E_SILENT));
    TEST_ASSERT_TRUE_MESSAGE(jce_runtime_audio_is_playing(s_rt, E_AWAKE),
        "stopping one entity must not stop another");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_rt->voice_count,
        "and the surviving row must still be in the table");
}

static void test_a_disabled_component_does_not_sound(void)
{
    /* Component enable is authored per component; a script that played a
     * disabled source would defeat the author's off switch. */
    jce_scene_set_component_enabled(s_scene, (JceEntity)E_SILENT,
                                    JCE_COMP_FLAG_AUDIO_SOURCE, false);
    TEST_ASSERT_FALSE_MESSAGE(jce_runtime_audio_play(s_rt, E_SILENT),
        "a disabled AudioSource must refuse to start");
    TEST_ASSERT_FALSE(jce_runtime_audio_is_playing(s_rt, E_SILENT));
}

static void test_bad_requests_are_refused_and_do_not_crash(void)
{
    TEST_ASSERT_FALSE(jce_runtime_audio_play(NULL, E_SILENT));
    TEST_ASSERT_FALSE(jce_runtime_audio_stop(NULL, E_SILENT));
    TEST_ASSERT_FALSE(jce_runtime_audio_is_playing(NULL, E_SILENT));

    TEST_ASSERT_FALSE_MESSAGE(jce_runtime_audio_play(s_rt, E_MUTE),
        "an entity with no AudioSource has nothing to play");
    TEST_ASSERT_FALSE_MESSAGE(jce_runtime_audio_play(s_rt, 999999u),
        "and neither has an entity that does not exist");
    TEST_ASSERT_FALSE(jce_runtime_audio_is_playing(s_rt, 999999u));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_probe.calls,
        "none of the refusals may have reached the loader");
}

/* ── the same thing, reached from Lua ──────────────────────────────────
 *
 * Every link in the script chain has its own gate: the key is registered
 * (test_jce_script_table_shape), the generated binding is byte-identical to a
 * fresh emit (gen_script_bindings), the host slot is assigned
 * (check_script_host_writers), and the runtime call is tested above.  All four
 * were green for jce.audio_play the moment it was declared -- and this repo
 * has shipped a feature whose every link was individually correct and which
 * nevertheless never ran (the cloud-shadow pass: a guard returning false, a
 * legitimately-unbaked binding branch, and a shader early-out).  So drive it
 * end to end: a Lua script asks for the sound, and the C side asserts a voice.
 */
#define AUDIO_SCRIPT_FILE "jce_rt_audio_ctrl_selftest.lua"

static void test_a_lua_script_can_start_and_stop_a_source(void)
{
    jce_test_write_file(AUDIO_SCRIPT_FILE,
        "local M = {}\n"
        "function M:on_update(dt)\n"
        "  local e = jce.find_by_name(\"Silent\")\n"
        "  if e == 0 then return end\n"
        "  self.n = (self.n or 0) + 1\n"
        "  if self.n == 1 then\n"
        "    self.started = jce.audio_play(e)\n"
        "  elseif self.n == 2 then\n"
        "    self.mid = jce.audio_is_playing(e)\n"
        "    self.stopped = jce.audio_stop(e)\n"
        "  elseif self.n == 3 then\n"
        "    self.after = jce.audio_is_playing(e)\n"
        "    jce.set_position(self.entity,\n"
        "      self.started and 1 or 0, self.mid and 1 or 0,\n"
        "      (self.stopped and not self.after) and 1 or 0)\n"
        "  end\n"
        "end\n"
        "return M\n");

    JceScriptComponent sc;
    memset(&sc, 0, sizeof sc);
    snprintf(sc.script_path, sizeof sc.script_path, "%s", AUDIO_SCRIPT_FILE);
    jce_scene_set_script(s_scene, E_MUTE, &sc);
    jce_scene_set_component_enabled(s_scene, E_MUTE, JCE_COMP_FLAG_SCRIPT, true);

    /* Rebuild: the script must be present when the spawn walk loads scripts. */
    shutdown_all_keep_scene();
    s_audio = jce_audio_create();
    s_probe.audio = s_audio;
    s_probe.calls = 0;
    JceRuntimeDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.scene         = s_scene;
    desc.audio         = s_audio;
    desc.audio_load_fn = probe_load;
    desc.user_data     = &s_probe;
    s_rt = jce_runtime_create(&desc);
    TEST_ASSERT_NOT_NULL(s_rt);

    for (int i = 0; i < 3; ++i) jce_runtime_step(s_rt, 1.0f / 60.0f);

    const JceTransform *t = jce_scene_get_transform(s_scene, E_MUTE);
    TEST_ASSERT_NOT_NULL_MESSAGE(t, "the reporter entity must still exist");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(1.0f, t->position.x,
        "jce.audio_play must reach jce_runtime_audio_play and return true -- "
        "0 here means the binding, the host slot or the adapter is dead");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(1.0f, t->position.y,
        "jce.audio_is_playing must see the voice the previous frame started");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(1.0f, t->position.z,
        "jce.audio_stop must stop it, and is_playing must then say false");

    remove(AUDIO_SCRIPT_FILE);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_play_on_awake_still_decides_what_spawn_starts);
    RUN_TEST(test_a_silent_source_can_be_started_and_stopped);
    RUN_TEST(test_replay_restarts_rather_than_stacking);
    RUN_TEST(test_stopping_one_source_leaves_the_others_alone);
    RUN_TEST(test_a_disabled_component_does_not_sound);
    RUN_TEST(test_bad_requests_are_refused_and_do_not_crash);
    RUN_TEST(test_a_lua_script_can_start_and_stop_a_source);
    return UNITY_END();
}
