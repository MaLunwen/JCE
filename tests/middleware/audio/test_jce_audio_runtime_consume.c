/*
 * test_jce_audio_runtime_consume.c — the RUNTIME consumes the authored mixer
 * routing at Play start.
 *
 * Closes a last-mile gap: the editor's Audio Mixer panel authors aux sends,
 * sidechain (ducking), per-bus insert effects, and named snapshots into
 * audio_mixer.json, but the runtime used to load ONLY the bus tree — so the
 * authored sends/sidechain/snapshots were persisted yet INERT in Play.
 *
 * jce_audio_mixer_apply_config() (engine/src/middleware/audio/
 * jce_audio_mixer_config.c) is the DEVICE-FREE half the runtime now calls: it
 * parses the EXACT schema the editor's mixer_save() writes and applies the bus
 * tree + sends + sidechain + snapshots onto a JceAudioMixer — pure L4
 * bookkeeping with NO miniaudio device.  This test authors an audio_mixer.json
 * string in that schema, runs the apply, and asserts via the public getters
 * that the mixer reflects the authored routing.
 *
 * The per-bus insert-effect attach path (jce_audio_mixer_config_each_effect ->
 * jce_audio_bus_add_effect) is DEVICE-GATED: it attaches to the live audio
 * device by bus name, so it is F12-verified in a running editor and is NOT
 * exercised here (no device).  We DO verify the effect *enumeration* (pure
 * parsing) reports the authored chain in order, which is the device-free half
 * of that path.
 */

#include "unity.h"

#include <jce/middleware/audio/jce_audio_mixer.h>
#include <jce/middleware/audio/jce_audio_mixer_config.h>

#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* The EXACT schema jce_panel_audio_mixer.cpp's mixer_save() emits: a buses
 * array (each bus row carries optional nested "sends" / "sidechain" / "effects")
 * and a top-level "snapshots" array.  JSON ids here (1=Master, 2=Music, 3=SFX,
 * 4=Voice, 5=Reverb) are remapped to the live dense ids assigned on load. */
static const char *k_authored_json =
"{\n"
"  \"buses\": [\n"
"    { \"id\": 1, \"parent\": 0, \"name\": \"Master\", \"volume\": 1.0000, \"muted\": 0, \"solo\": 0 },\n"
"    { \"id\": 2, \"parent\": 1, \"name\": \"Music\", \"volume\": 0.8000, \"muted\": 0, \"solo\": 0,"
"      \"sends\": [ { \"dest\": 5, \"amount\": 0.3000 } ],"
"      \"sidechain\": { \"key\": 4, \"threshold_db\": -28.0000, \"ratio\": 5.0000,"
"        \"attack_ms\": 8.0000, \"release_ms\": 220.0000, \"floor_db\": -20.0000 } },\n"
"    { \"id\": 3, \"parent\": 1, \"name\": \"SFX\", \"volume\": 1.0000, \"muted\": 0, \"solo\": 0,"
"      \"sends\": [ { \"dest\": 5, \"amount\": 0.1500 } ],"
"      \"effects\": [ { \"type\": \"eq\", \"shape\": 0, \"freq\": 1000.0000, \"gain_db\": 3.0000, \"q\": 0.7070 },"
"        { \"type\": \"limiter\", \"ceiling_db\": -1.0000, \"release_ms\": 50.0000 } ] },\n"
"    { \"id\": 4, \"parent\": 1, \"name\": \"Voice\", \"volume\": 1.0000, \"muted\": 0, \"solo\": 0 },\n"
"    { \"id\": 5, \"parent\": 1, \"name\": \"Reverb\", \"volume\": 1.0000, \"muted\": 0, \"solo\": 0 }\n"
"  ],\n"
"  \"snapshots\": [\n"
"    { \"name\": \"Combat\", \"volumes\": [ { \"id\": 2, \"volume\": 0.8000 }, { \"id\": 4, \"volume\": 1.0000 } ] },\n"
"    { \"name\": \"Stealth\", \"volumes\": [ { \"id\": 2, \"volume\": 0.2000 }, { \"id\": 4, \"volume\": 1.0000 } ] }\n"
"  ]\n"
"}\n";

/* A bare bus tree with NO sends / sidechain / snapshots / effects — proves the
 * classic-default path stays byte-identical (no routing introduced). */
static const char *k_plain_json =
"{\n"
"  \"buses\": [\n"
"    { \"id\": 1, \"parent\": 0, \"name\": \"Master\", \"volume\": 1.0000, \"muted\": 0, \"solo\": 0 },\n"
"    { \"id\": 2, \"parent\": 1, \"name\": \"Music\", \"volume\": 0.8000, \"muted\": 0, \"solo\": 0 },\n"
"    { \"id\": 3, \"parent\": 1, \"name\": \"SFX\", \"volume\": 1.0000, \"muted\": 0, \"solo\": 0 }\n"
"  ]\n"
"}\n";

/* ------------------------------------------------------------------ */

static JceAudioBusId find(JceAudioMixer *m, const char *name)
{
	return jce_audio_mixer_find_bus(m, name);
}

/* The runtime's device-free apply reflects the authored aux sends. */
static void test_apply_consumes_sends(void)
{
	JceAudioMixer *m = jce_audio_mixer_create();
	TEST_ASSERT_TRUE(jce_audio_mixer_apply_config(m, k_authored_json,
	                                              strlen(k_authored_json)));

	JceAudioBusId music = find(m, "Music");
	JceAudioBusId sfx   = find(m, "SFX");
	JceAudioBusId rev   = find(m, "Reverb");
	TEST_ASSERT_NOT_EQUAL(JCE_AUDIO_BUS_INVALID, music);
	TEST_ASSERT_NOT_EQUAL(JCE_AUDIO_BUS_INVALID, sfx);
	TEST_ASSERT_NOT_EQUAL(JCE_AUDIO_BUS_INVALID, rev);

	/* Send amounts survived the json-id -> live-id remap. */
	TEST_ASSERT_EQUAL_FLOAT(0.30f, jce_audio_mixer_get_send(m, music, rev));
	TEST_ASSERT_EQUAL_FLOAT(0.15f, jce_audio_mixer_get_send(m, sfx,   rev));

	/* And the live resolved gain delivered into the Reverb return is
	 * resolve_volume(src) * amount (Music vol 0.8 * 0.30 = 0.24). */
	TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.8f * 0.30f,
	                         jce_audio_mixer_resolve_send(m, music, rev));

	jce_audio_mixer_destroy(m);
}

/* The runtime's device-free apply reflects the authored sidechain field-for-
 * field, with the key bus remapped to its live id. */
static void test_apply_consumes_sidechain(void)
{
	JceAudioMixer *m = jce_audio_mixer_create();
	TEST_ASSERT_TRUE(jce_audio_mixer_apply_config(m, k_authored_json,
	                                              strlen(k_authored_json)));

	JceAudioBusId music = find(m, "Music");
	JceAudioBusId voice = find(m, "Voice");
	TEST_ASSERT_NOT_EQUAL(JCE_AUDIO_BUS_INVALID, music);
	TEST_ASSERT_NOT_EQUAL(JCE_AUDIO_BUS_INVALID, voice);

	TEST_ASSERT_TRUE(jce_audio_mixer_has_sidechain(m, music));
	JceAudioDuckParams dp;
	memset(&dp, 0, sizeof dp);
	TEST_ASSERT_TRUE(jce_audio_mixer_get_sidechain(m, music, &dp));
	TEST_ASSERT_EQUAL_UINT16(voice,   dp.key);      /* key remapped to live id */
	TEST_ASSERT_EQUAL_FLOAT(-28.0f,   dp.threshold_db);
	TEST_ASSERT_EQUAL_FLOAT(  5.0f,   dp.ratio);
	TEST_ASSERT_EQUAL_FLOAT(  8.0f,   dp.attack_ms);
	TEST_ASSERT_EQUAL_FLOAT(220.0f,   dp.release_ms);
	TEST_ASSERT_EQUAL_FLOAT(-20.0f,   dp.max_attenuation_db);

	jce_audio_mixer_destroy(m);
}

/* The runtime's device-free apply reflects the authored named snapshots. */
static void test_apply_consumes_snapshots(void)
{
	JceAudioMixer *m = jce_audio_mixer_create();
	TEST_ASSERT_TRUE(jce_audio_mixer_apply_config(m, k_authored_json,
	                                              strlen(k_authored_json)));

	TEST_ASSERT_EQUAL_UINT32(2u, jce_audio_mixer_snapshot_count(m));

	int seen_combat = 0, seen_stealth = 0;
	for (uint32_t i = 0; i < jce_audio_mixer_snapshot_count(m); ++i) {
		char name[JCE_AUDIO_SNAPSHOT_NAME] = {0};
		TEST_ASSERT_TRUE(jce_audio_mixer_snapshot_name(m, i, name, sizeof name));
		if (strcmp(name, "Combat")  == 0) seen_combat  = 1;
		if (strcmp(name, "Stealth") == 0) seen_stealth = 1;
	}
	TEST_ASSERT_TRUE(seen_combat && seen_stealth);

	/* The authored per-bus snapshot target volumes survive (remapped ids). */
	JceAudioBusId music = find(m, "Music");
	TEST_ASSERT_EQUAL_FLOAT(0.2f,
		jce_audio_mixer_snapshot_get_volume(m, "Stealth", music));
	TEST_ASSERT_EQUAL_FLOAT(0.8f,
		jce_audio_mixer_snapshot_get_volume(m, "Combat", music));

	jce_audio_mixer_destroy(m);
}

/* The bus tree (volume/mute/solo) is applied with the parent remap intact. */
static void test_apply_builds_bus_tree(void)
{
	JceAudioMixer *m = jce_audio_mixer_create();
	TEST_ASSERT_TRUE(jce_audio_mixer_apply_config(m, k_authored_json,
	                                              strlen(k_authored_json)));

	/* Master + 4 children. */
	TEST_ASSERT_EQUAL_UINT32(5u, jce_audio_mixer_bus_count(m));
	JceAudioBusId music = find(m, "Music");
	TEST_ASSERT_NOT_EQUAL(JCE_AUDIO_BUS_INVALID, music);
	TEST_ASSERT_EQUAL_FLOAT(0.8f, jce_audio_mixer_get_volume(m, music));
	TEST_ASSERT_EQUAL_UINT16(JCE_AUDIO_BUS_MASTER,
	                         jce_audio_mixer_get_parent(m, music));

	jce_audio_mixer_destroy(m);
}

/* Device-free enumeration of the per-bus insert-effect chains: the authored SFX
 * chain (EQ then Limiter) is reported, in order, with the parsed fields.  The
 * actual attach to a live device (jce_audio_bus_add_effect) is F12-verified. */
typedef struct {
	int      count;
	char     bus[8][32];
	int      type[8];
	uint32_t idx[8];
	float    eq_freq;     /* captured from the first EQ for a field check */
	float    eq_gain;
} FxCapture;

static void fx_capture_cb(const char *bus_name, const JceAudioEffectDesc *desc,
                          uint32_t index, void *user)
{
	FxCapture *c = (FxCapture *)user;
	if (c->count >= 8) return;
	strncpy(c->bus[c->count], bus_name ? bus_name : "", 31);
	c->type[c->count] = (int)desc->type;
	c->idx[c->count]  = index;
	if (desc->type == JCE_AUDIO_EFFECT_EQ) {
		c->eq_freq = desc->u.eq.frequency_hz;
		c->eq_gain = desc->u.eq.gain_db;
	}
	++c->count;
}

static void test_effect_enumeration_in_order(void)
{
	FxCapture cap;
	memset(&cap, 0, sizeof cap);
	uint32_t n = jce_audio_mixer_config_each_effect(
		k_authored_json, strlen(k_authored_json), fx_capture_cb, &cap);

	TEST_ASSERT_EQUAL_UINT32(2u, n);            /* EQ + Limiter on SFX */
	TEST_ASSERT_EQUAL_INT(2, cap.count);
	TEST_ASSERT_EQUAL_STRING("SFX", cap.bus[0]);
	TEST_ASSERT_EQUAL_STRING("SFX", cap.bus[1]);
	/* Authored chain order is preserved: EQ (index 0) then Limiter (index 1). */
	TEST_ASSERT_EQUAL_INT(JCE_AUDIO_EFFECT_EQ,      cap.type[0]);
	TEST_ASSERT_EQUAL_UINT32(0u, cap.idx[0]);
	TEST_ASSERT_EQUAL_INT(JCE_AUDIO_EFFECT_LIMITER, cap.type[1]);
	TEST_ASSERT_EQUAL_UINT32(1u, cap.idx[1]);
	/* The EQ's parsed fields match the authored json. */
	TEST_ASSERT_EQUAL_FLOAT(1000.0f, cap.eq_freq);
	TEST_ASSERT_EQUAL_FLOAT(   3.0f, cap.eq_gain);
}

/* DEFAULT path: a json with NO sends/sidechain/snapshots/effects leaves the
 * mixer at classic defaults — no sends, no sidechain, zero snapshots, no
 * effects enumerated.  This guards byte-identical-to-today behavior. */
static void test_plain_config_no_routing_introduced(void)
{
	JceAudioMixer *m = jce_audio_mixer_create();
	TEST_ASSERT_TRUE(jce_audio_mixer_apply_config(m, k_plain_json,
	                                              strlen(k_plain_json)));

	JceAudioBusId music = find(m, "Music");
	JceAudioBusId sfx   = find(m, "SFX");
	TEST_ASSERT_NOT_EQUAL(JCE_AUDIO_BUS_INVALID, music);
	TEST_ASSERT_NOT_EQUAL(JCE_AUDIO_BUS_INVALID, sfx);

	/* No aux sends anywhere. */
	TEST_ASSERT_EQUAL_UINT32(0u, jce_audio_mixer_send_count(m, JCE_AUDIO_BUS_MASTER));
	TEST_ASSERT_EQUAL_UINT32(0u, jce_audio_mixer_send_count(m, music));
	TEST_ASSERT_EQUAL_UINT32(0u, jce_audio_mixer_send_count(m, sfx));
	TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_audio_mixer_get_send(m, music, sfx));

	/* No sidechain on any bus. */
	TEST_ASSERT_FALSE(jce_audio_mixer_has_sidechain(m, music));
	TEST_ASSERT_FALSE(jce_audio_mixer_has_sidechain(m, sfx));
	TEST_ASSERT_FALSE(jce_audio_mixer_has_sidechain(m, JCE_AUDIO_BUS_MASTER));

	/* No snapshots. */
	TEST_ASSERT_EQUAL_UINT32(0u, jce_audio_mixer_snapshot_count(m));

	/* Resolved volume is the classic ancestor-fold (Music 0.8 * Master 1.0). */
	TEST_ASSERT_EQUAL_FLOAT(0.8f, jce_audio_mixer_resolve_volume(m, music));
	/* ...and unchanged when folding the (absent) duck gain. */
	TEST_ASSERT_EQUAL_FLOAT(0.8f, jce_audio_mixer_resolve_volume_ducked(m, music));

	jce_audio_mixer_destroy(m);

	/* No effects enumerated from a config without an "effects" block. */
	FxCapture cap;
	memset(&cap, 0, sizeof cap);
	uint32_t fn = jce_audio_mixer_config_each_effect(
		k_plain_json, strlen(k_plain_json), fx_capture_cb, &cap);
	TEST_ASSERT_EQUAL_UINT32(0u, fn);
	TEST_ASSERT_EQUAL_INT(0, cap.count);
}

/* NULL / empty inputs are safe no-ops. */
static void test_null_and_empty_safe(void)
{
	JceAudioMixer *m = jce_audio_mixer_create();
	TEST_ASSERT_FALSE(jce_audio_mixer_apply_config(NULL, k_plain_json,
	                                              strlen(k_plain_json)));
	TEST_ASSERT_FALSE(jce_audio_mixer_apply_config(m, NULL, 0));
	TEST_ASSERT_FALSE(jce_audio_mixer_apply_config(m, "", 0));
	/* A json with no "buses" array yields false (caller falls back to default). */
	TEST_ASSERT_FALSE(jce_audio_mixer_apply_config(m, "{}", 2));

	TEST_ASSERT_EQUAL_UINT32(0u,
		jce_audio_mixer_config_each_effect(NULL, 0, fx_capture_cb, m));
	TEST_ASSERT_EQUAL_UINT32(0u,
		jce_audio_mixer_config_each_effect(k_authored_json,
			strlen(k_authored_json), NULL, m));

	jce_audio_mixer_destroy(m);
}

/* ------------------------------------------------------------------ */

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_apply_builds_bus_tree);
	RUN_TEST(test_apply_consumes_sends);
	RUN_TEST(test_apply_consumes_sidechain);
	RUN_TEST(test_apply_consumes_snapshots);
	RUN_TEST(test_effect_enumeration_in_order);
	RUN_TEST(test_plain_config_no_routing_introduced);
	RUN_TEST(test_null_and_empty_safe);
	return UNITY_END();
}
