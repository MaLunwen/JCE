/*
 * test_jce_audio_mixer_config.c — round-trip of the full mixer *config*
 * (buses + aux sends + sidechain ducking + named snapshots) through the
 * exact public read/write API the editor's audio_mixer.json (de)serializer
 * uses (jce_panel_audio_mixer.cpp).
 *
 * The editor panel serializes a JceAudioMixer to JSON by reading every field
 * back with the public getters, and reconstructs it on load by replaying the
 * setters in creation order (deferred send/sidechain/snapshot refs resolve
 * through an id-remap because add_bus reassigns dense ids).  The save path
 * depends on two read-back getters added for this feature:
 *
 *     jce_audio_mixer_get_sidechain()  — read installed duck params
 *     jce_audio_mixer_snapshot_name()  — enumerate snapshot names
 *
 * Without them the sidechain config and snapshot names could not be written
 * back out.  This test proves the whole config survives a save→reload by
 * mirroring the panel's logic over an in-memory model, with NO miniaudio
 * device — pure L4 bookkeeping.
 */

#include "unity.h"

#include <jce/middleware/audio/jce_audio_mixer.h>

#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* ------------------------------------------------------------------ */
/* New read-back getters (the serializer's missing-link API).          */
/* ------------------------------------------------------------------ */

static void test_get_sidechain_reads_back_set_params(void)
{
    JceAudioMixer *m = jce_audio_mixer_create();
    JceAudioBusId music = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "Music", 1.0f);
    JceAudioBusId voice = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "Voice", 1.0f);

    JceAudioDuckParams in = jce_audio_duck_default_params();
    in.key                = voice;
    in.threshold_db       = -24.0f;
    in.ratio              = 6.0f;
    in.attack_ms          = 12.0f;
    in.release_ms         = 300.0f;
    in.max_attenuation_db = -18.0f;
    TEST_ASSERT_TRUE(jce_audio_mixer_set_sidechain(m, music, &in, 48000u));
    TEST_ASSERT_TRUE(jce_audio_mixer_has_sidechain(m, music));

    JceAudioDuckParams out;
    memset(&out, 0, sizeof(out));
    TEST_ASSERT_TRUE(jce_audio_mixer_get_sidechain(m, music, &out));
    TEST_ASSERT_EQUAL_UINT16(voice, out.key);
    TEST_ASSERT_EQUAL_FLOAT(-24.0f, out.threshold_db);
    TEST_ASSERT_EQUAL_FLOAT( 6.0f,  out.ratio);
    TEST_ASSERT_EQUAL_FLOAT(12.0f,  out.attack_ms);
    TEST_ASSERT_EQUAL_FLOAT(300.0f, out.release_ms);
    TEST_ASSERT_EQUAL_FLOAT(-18.0f, out.max_attenuation_db);

    /* No sidechain on Voice -> getter reports false, leaves out untouched. */
    JceAudioDuckParams probe;
    memset(&probe, 0x7f, sizeof(probe));
    TEST_ASSERT_FALSE(jce_audio_mixer_get_sidechain(m, voice, &probe));
    /* NULL-safe / bad-arg paths. */
    TEST_ASSERT_FALSE(jce_audio_mixer_get_sidechain(m, music, NULL));
    TEST_ASSERT_FALSE(jce_audio_mixer_get_sidechain(NULL, music, &probe));

    jce_audio_mixer_destroy(m);
}

static void test_snapshot_name_enumerates_all(void)
{
    JceAudioMixer *m = jce_audio_mixer_create();
    jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_MASTER, "Music", 1.0f);
    TEST_ASSERT_TRUE(jce_audio_mixer_capture_snapshot(m, "Combat"));
    TEST_ASSERT_TRUE(jce_audio_mixer_capture_snapshot(m, "Stealth"));
    TEST_ASSERT_TRUE(jce_audio_mixer_capture_snapshot(m, "Paused"));
    TEST_ASSERT_EQUAL_UINT32(3u, jce_audio_mixer_snapshot_count(m));

    /* Every index 0..count-1 yields a known name; together they cover the set. */
    int seen_combat = 0, seen_stealth = 0, seen_paused = 0;
    for (uint32_t i = 0; i < jce_audio_mixer_snapshot_count(m); ++i) {
        char name[JCE_AUDIO_SNAPSHOT_NAME] = {0};
        TEST_ASSERT_TRUE(jce_audio_mixer_snapshot_name(m, i, name, sizeof(name)));
        if (strcmp(name, "Combat")  == 0) seen_combat  = 1;
        if (strcmp(name, "Stealth") == 0) seen_stealth = 1;
        if (strcmp(name, "Paused")  == 0) seen_paused  = 1;
    }
    TEST_ASSERT_TRUE(seen_combat && seen_stealth && seen_paused);

    /* Out-of-range index / bad args. */
    char buf[JCE_AUDIO_SNAPSHOT_NAME] = {0};
    TEST_ASSERT_FALSE(jce_audio_mixer_snapshot_name(m, 3u, buf, sizeof(buf)));
    TEST_ASSERT_FALSE(jce_audio_mixer_snapshot_name(m, 0u, buf, 0u));
    TEST_ASSERT_FALSE(jce_audio_mixer_snapshot_name(NULL, 0u, buf, sizeof(buf)));

    jce_audio_mixer_destroy(m);
}

/* ------------------------------------------------------------------ */
/* Full config save -> reload round-trip (mirrors the editor panel).   */
/* ------------------------------------------------------------------ */

/* In-memory mirror of the audio_mixer.json schema. */
#define CFG_MAX_BUSES 16
typedef struct {
    unsigned id;        /* JSON id (stable across the dump)            */
    unsigned parent;    /* JSON parent id                              */
    char     name[32];
    float    volume;
    int      muted, solo;
    /* aux sends: dest JSON id + amount (0 dest = empty)               */
    struct { unsigned dest; float amount; } sends[JCE_AUDIO_MAX_SENDS];
    /* sidechain (key==0 => none)                                      */
    JceAudioDuckParams duck; int has_duck;
} CfgBus;
typedef struct {
    char  name[JCE_AUDIO_SNAPSHOT_NAME];
    float vol[CFG_MAX_BUSES]; int has[CFG_MAX_BUSES]; /* indexed by bus slot */
} CfgSnap;
typedef struct {
    CfgBus  buses[CFG_MAX_BUSES]; int n_buses;
    CfgSnap snaps[JCE_AUDIO_MAX_SNAPSHOTS]; int n_snaps;
} Cfg;

/* SAVE: read every field of `m` into `cfg` via the public getters — exactly
 * the data the editor's mixer_save() emits to JSON. */
static void cfg_dump(const JceAudioMixer *m, Cfg *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    JceAudioBusId ids[CFG_MAX_BUSES] = {0};
    uint32_t n = jce_audio_mixer_list_buses(m, ids, CFG_MAX_BUSES);
    cfg->n_buses = (int)n;
    for (uint32_t i = 0; i < n; ++i) {
        CfgBus *b = &cfg->buses[i];
        b->id     = ids[i];
        b->parent = jce_audio_mixer_get_parent(m, ids[i]);
        const char *nm = jce_audio_mixer_get_name(m, ids[i]);
        strncpy(b->name, nm ? nm : "", sizeof(b->name) - 1);
        b->volume = jce_audio_mixer_get_volume(m, ids[i]);
        b->muted  = jce_audio_mixer_is_muted(m, ids[i]) ? 1 : 0;
        b->solo   = jce_audio_mixer_is_solo (m, ids[i]) ? 1 : 0;

        int s = 0;
        for (uint32_t j = 0; j < n && s < JCE_AUDIO_MAX_SENDS; ++j) {
            if (ids[j] == ids[i]) continue;
            float amt = jce_audio_mixer_get_send(m, ids[i], ids[j]);
            if (amt > 0.0f) {
                b->sends[s].dest   = ids[j];
                b->sends[s].amount = amt;
                ++s;
            }
        }
        JceAudioDuckParams dp;
        if (jce_audio_mixer_get_sidechain(m, ids[i], &dp)) {
            b->duck     = dp;
            b->has_duck = 1;
        }
    }

    uint32_t sn = jce_audio_mixer_snapshot_count(m);
    cfg->n_snaps = (int)sn;
    for (uint32_t si = 0; si < sn; ++si) {
        CfgSnap *cs = &cfg->snaps[si];
        jce_audio_mixer_snapshot_name(m, si, cs->name, sizeof(cs->name));
        for (uint32_t i = 0; i < n; ++i) {
            float v = jce_audio_mixer_snapshot_get_volume(m, cs->name, ids[i]);
            if (v >= 0.0f) { cs->vol[i] = v; cs->has[i] = 1; }
        }
    }
}

/* Resolve a JSON id to the live bus id in the rebuilt mixer (id-remap, since
 * add_bus reassigns dense ids — same logic the editor loader uses). */
static JceAudioBusId remap(const unsigned *jids, const JceAudioBusId *lids,
                           int n, unsigned jid)
{
    if (jid == JCE_AUDIO_BUS_MASTER) return JCE_AUDIO_BUS_MASTER;
    for (int i = 0; i < n; ++i)
        if (jids[i] == jid) return lids[i];
    return JCE_AUDIO_BUS_INVALID;
}

/* LOAD: reconstruct a fresh mixer from `cfg` exactly as the editor loader. */
static JceAudioMixer *cfg_rebuild(const Cfg *cfg)
{
    JceAudioMixer *m = jce_audio_mixer_create();
    unsigned      jids[CFG_MAX_BUSES] = {0};
    JceAudioBusId lids[CFG_MAX_BUSES] = {0};
    int           map_n = 0;

    /* Pass 1: create buses in order. */
    for (int i = 0; i < cfg->n_buses; ++i) {
        const CfgBus *b = &cfg->buses[i];
        if (b->id == JCE_AUDIO_BUS_MASTER) {
            jce_audio_mixer_set_volume(m, JCE_AUDIO_BUS_MASTER, b->volume);
            jce_audio_mixer_set_muted (m, JCE_AUDIO_BUS_MASTER, b->muted != 0);
            jce_audio_mixer_set_solo  (m, JCE_AUDIO_BUS_MASTER, b->solo  != 0);
            jids[map_n] = b->id; lids[map_n] = JCE_AUDIO_BUS_MASTER; ++map_n;
        } else {
            JceAudioBusId par = remap(jids, lids, map_n,
                                      b->parent ? b->parent : JCE_AUDIO_BUS_MASTER);
            if (par == JCE_AUDIO_BUS_INVALID) par = JCE_AUDIO_BUS_MASTER;
            JceAudioBusId live = jce_audio_mixer_add_bus(m, par, b->name, b->volume);
            jce_audio_mixer_set_muted(m, live, b->muted != 0);
            jce_audio_mixer_set_solo (m, live, b->solo  != 0);
            jids[map_n] = b->id; lids[map_n] = live; ++map_n;
        }
    }
    /* Pass 2: sends + sidechain (refs resolved through the remap). */
    for (int i = 0; i < cfg->n_buses; ++i) {
        const CfgBus *b = &cfg->buses[i];
        JceAudioBusId src = remap(jids, lids, map_n, b->id);
        for (int s = 0; s < JCE_AUDIO_MAX_SENDS; ++s) {
            if (b->sends[s].dest == 0) continue;
            JceAudioBusId dst = remap(jids, lids, map_n, b->sends[s].dest);
            if (dst != JCE_AUDIO_BUS_INVALID)
                jce_audio_mixer_set_send(m, src, dst, b->sends[s].amount);
        }
        if (b->has_duck) {
            JceAudioDuckParams dp = b->duck;
            dp.key = remap(jids, lids, map_n, b->duck.key);
            jce_audio_mixer_set_sidechain(m, src, &dp, 48000u);
        }
    }
    /* Snapshots. */
    for (int si = 0; si < cfg->n_snaps; ++si) {
        const CfgSnap *cs = &cfg->snaps[si];
        for (int i = 0; i < cfg->n_buses; ++i) {
            if (!cs->has[i]) continue;
            JceAudioBusId live = remap(jids, lids, map_n, cfg->buses[i].id);
            if (live != JCE_AUDIO_BUS_INVALID)
                jce_audio_mixer_snapshot_set_volume(m, cs->name, live, cs->vol[i]);
        }
    }
    return m;
}

static void test_full_config_round_trip(void)
{
    /* ---- Author a non-trivial mix on A. ---- */
    JceAudioMixer *A = jce_audio_mixer_create();
    JceAudioBusId music = jce_audio_mixer_add_bus(A, JCE_AUDIO_BUS_MASTER, "Music", 0.8f);
    JceAudioBusId sfx   = jce_audio_mixer_add_bus(A, JCE_AUDIO_BUS_MASTER, "SFX",   1.0f);
    JceAudioBusId voice = jce_audio_mixer_add_bus(A, JCE_AUDIO_BUS_MASTER, "Voice", 1.0f);
    JceAudioBusId rev   = jce_audio_mixer_add_bus(A, JCE_AUDIO_BUS_MASTER, "Reverb",1.0f);
    jce_audio_mixer_set_muted(A, sfx, 0);
    jce_audio_mixer_set_solo (A, voice, 0);

    /* aux sends: Music & SFX feed the Reverb return. */
    TEST_ASSERT_TRUE(jce_audio_mixer_set_send(A, music, rev, 0.30f));
    TEST_ASSERT_TRUE(jce_audio_mixer_set_send(A, sfx,   rev, 0.15f));

    /* sidechain: Music ducks under Voice. */
    JceAudioDuckParams dp = jce_audio_duck_default_params();
    dp.key = voice; dp.threshold_db = -28.0f; dp.ratio = 5.0f;
    dp.attack_ms = 8.0f; dp.release_ms = 220.0f; dp.max_attenuation_db = -20.0f;
    TEST_ASSERT_TRUE(jce_audio_mixer_set_sidechain(A, music, &dp, 48000u));

    /* snapshots. */
    TEST_ASSERT_TRUE(jce_audio_mixer_capture_snapshot(A, "Combat"));
    jce_audio_mixer_snapshot_set_volume(A, "Stealth", music, 0.2f);
    jce_audio_mixer_snapshot_set_volume(A, "Stealth", voice, 1.0f);

    /* ---- Dump A -> reconstruct B (the save/reload round-trip). ---- */
    Cfg cfg;
    cfg_dump(A, &cfg);
    JceAudioMixer *B = cfg_rebuild(&cfg);

    /* ---- Re-dump B and compare to A's dump field-by-field. ---- */
    Cfg cfg2;
    cfg_dump(B, &cfg2);

    TEST_ASSERT_EQUAL_INT(cfg.n_buses, cfg2.n_buses);
    for (int i = 0; i < cfg.n_buses; ++i) {
        TEST_ASSERT_EQUAL_STRING(cfg.buses[i].name, cfg2.buses[i].name);
        TEST_ASSERT_EQUAL_FLOAT(cfg.buses[i].volume, cfg2.buses[i].volume);
        TEST_ASSERT_EQUAL_INT(cfg.buses[i].muted, cfg2.buses[i].muted);
        TEST_ASSERT_EQUAL_INT(cfg.buses[i].solo,  cfg2.buses[i].solo);
        /* sends preserved (count + amounts; ids dense+stable so they match). */
        for (int s = 0; s < JCE_AUDIO_MAX_SENDS; ++s) {
            TEST_ASSERT_EQUAL_UINT(cfg.buses[i].sends[s].dest,
                                   cfg2.buses[i].sends[s].dest);
            TEST_ASSERT_EQUAL_FLOAT(cfg.buses[i].sends[s].amount,
                                    cfg2.buses[i].sends[s].amount);
        }
        TEST_ASSERT_EQUAL_INT(cfg.buses[i].has_duck, cfg2.buses[i].has_duck);
        if (cfg.buses[i].has_duck) {
            TEST_ASSERT_EQUAL_UINT16(cfg.buses[i].duck.key, cfg2.buses[i].duck.key);
            TEST_ASSERT_EQUAL_FLOAT(cfg.buses[i].duck.threshold_db,
                                    cfg2.buses[i].duck.threshold_db);
            TEST_ASSERT_EQUAL_FLOAT(cfg.buses[i].duck.ratio,
                                    cfg2.buses[i].duck.ratio);
            TEST_ASSERT_EQUAL_FLOAT(cfg.buses[i].duck.attack_ms,
                                    cfg2.buses[i].duck.attack_ms);
            TEST_ASSERT_EQUAL_FLOAT(cfg.buses[i].duck.release_ms,
                                    cfg2.buses[i].duck.release_ms);
            TEST_ASSERT_EQUAL_FLOAT(cfg.buses[i].duck.max_attenuation_db,
                                    cfg2.buses[i].duck.max_attenuation_db);
        }
    }

    /* snapshots survive (count + the authored values). */
    TEST_ASSERT_EQUAL_INT(cfg.n_snaps, cfg2.n_snaps);
    /* Stealth's Music target volume specifically survives the round-trip. */
    TEST_ASSERT_EQUAL_FLOAT(0.2f,
        jce_audio_mixer_snapshot_get_volume(B, "Stealth", music));
    TEST_ASSERT_EQUAL_FLOAT(1.0f,
        jce_audio_mixer_snapshot_get_volume(B, "Stealth", voice));
    /* Combat captured all four buses' live volumes; Music was 0.8. */
    TEST_ASSERT_EQUAL_FLOAT(0.8f,
        jce_audio_mixer_snapshot_get_volume(B, "Combat", music));

    /* And the live resolved sends still produce the authored gain on B. */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.8f * 0.30f,
        jce_audio_mixer_resolve_send(B, music, rev));

    jce_audio_mixer_destroy(A);
    jce_audio_mixer_destroy(B);
}

/* ------------------------------------------------------------------ */
/* runner                                                             */
/* ------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_get_sidechain_reads_back_set_params);
    RUN_TEST(test_snapshot_name_enumerates_all);
    RUN_TEST(test_full_config_round_trip);
    return UNITY_END();
}
