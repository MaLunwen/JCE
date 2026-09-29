cmake_minimum_required(VERSION 3.20)

# ----------------------------------------------------------------------- #
#  L5 audio-layer pure-logic unit tests.                                  #
#                                                                         #
#  Occlusion solver and tracker run without miniaudio.  Raycasts are     #
#  supplied via a test stub, so no physics is needed either.             #
# ----------------------------------------------------------------------- #

jce_add_unit_test(test_jce_audio_occlusion
    SOURCES test_jce_audio_occlusion.c
    LINK    jce_core jce_audio)

jce_add_unit_test(test_jce_audio_mixer
    SOURCES test_jce_audio_mixer.c
    LINK    jce_core jce_audio)

# Full mixer-config round-trip (buses + aux sends + sidechain + snapshots) —
# exercises the public read/write API the editor's audio_mixer.json
# (de)serializer uses, including the get_sidechain / snapshot_name read-back
# getters added for the mixer authoring last-mile.  Pure L4 bookkeeping, no
# miniaudio device.
jce_add_unit_test(test_jce_audio_mixer_config
    SOURCES test_jce_audio_mixer_config.c
    LINK    jce_core jce_audio)

# The RUNTIME consumes the authored mixer routing at Play start: the device-free
# jce_audio_mixer_apply_config() (jce_audio_mixer_config.c) parses the exact
# audio_mixer.json schema the editor writes and applies the bus tree + aux sends
# + sidechain ducking + named snapshots onto a JceAudioMixer, and
# jce_audio_mixer_config_each_effect() enumerates the per-bus insert-effect
# chains.  Pure L4 bookkeeping + parsing, no miniaudio device (the effect ATTACH
# to a live device is device-gated / F12-verified, not exercised here).
jce_add_unit_test(test_jce_audio_runtime_consume
    SOURCES test_jce_audio_runtime_consume.c
    LINK    jce_core jce_audio)

# A bundle keeps the logical source address (for example .mp3) while its
# payload may be a cooked JCEA container. The memory path used by editor Play
# must consume that representation exactly like the PAK path does.
jce_add_unit_test(test_jce_audio_cooked_memory
    SOURCES test_jce_audio_cooked_memory.c
    LINK    jce_core jce_audio jce_resource)
target_include_directories(test_jce_audio_cooked_memory PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")

# Reproduce the editor bundle-Play sound path (VFS read + CPU decode) against
# the real packed elemental_serenity bundle.  Self-ignores when the artifact
# is absent.  Diagnoses "jce.play_sound: cannot load" in mounted-bundle Play.
jce_add_unit_test(test_jce_bundle_play_audio
    SOURCES test_jce_bundle_play_audio.c
    LINK    jce_core jce_audio jce_resource)

jce_add_unit_test(test_jce_reverb_zones
    SOURCES test_jce_reverb_zones.c
    LINK    jce_core jce_audio)

# Master-mix architecture (single shared device summing device-less engines +
# jce_audio_master_tap_set — the recorder's audio source).  Exercises the
# REAL device path: gapless tap cadence across engine create/destroy, format
# contract (48k/2ch), and the clear-tap fence.  Self-ignores without an
# output device.
jce_add_unit_test(test_jce_audio_master_tap
    SOURCES test_jce_audio_master_tap.c
    LINK    jce_core jce_audio jce_resource)

# Insert-effect DSP chain (FEATURE 5.1): EQ / compressor / limiter / delay.
# The DSP math runs offline over plain float buffers — no miniaudio device.
jce_add_unit_test(test_jce_audio_dsp
    SOURCES test_jce_audio_dsp.c
    LINK    jce_core jce_audio)

# The palette past its original four, and the registration surface that stops
# it being a CLOSED SET -- the two axes audio.dsp.insert-effect-chain names.
# No case asserts "the effect runs": a chain that accepted an effect and
# passed the buffer through untouched would satisfy that, and is the exact
# shape this ledger keeps finding.  Each names a property a PASS-THROUGH does
# not have -- a shaper returns sample values the input never took, a
# modulated delay makes two identical input blocks come out different, and
# feedback is still audible after the input stops.
jce_add_unit_test(test_jce_audio_dsp_palette
    SOURCES test_jce_audio_dsp_palette.c
    LINK    jce_core jce_audio)

# Bus send/return + sidechain ducking + mixer snapshots (FEATURE 5.2).
# The send/duck/snapshot math is exercised directly; the duck DSP is also
# proven through miniaudio's offline read path (ma_audio_buffer /
# ma_data_source_read_pcm_frames) with NO audio device — hence the explicit
# miniaudio link so the test can pull frames headlessly.
jce_add_unit_test(test_jce_audio_routing
    SOURCES test_jce_audio_routing.c
    LINK    jce_core jce_audio miniaudio::miniaudio)

# An aux send may not close a ring.  jce_audio_mixer_set_send guarded only
# src == dest, so A->B plus B->A -- and any longer ring -- was accepted.
# Harmless ONLY because nothing routes a send's audio: resolve_send has no
# product caller.  The moment a device-side send exists that authored ring is
# a ring in the miniaudio node graph and pulling frames through it recurses
# with no bottom, so the refusal lands before any node is attached.  Pure
# bookkeeping, no device.
jce_add_unit_test(test_jce_audio_send_cycle
    SOURCES test_jce_audio_send_cycle.c
    LINK    jce_core jce_audio)

# The LIVE half of FEATURE 5.2's sidechain ducking: jce_audio_bus_get_peak
# metered on the real miniaudio node graph, and the duck follower driven from
# it.  Before this, duck_advance had no caller outside the tests, so the duck
# gain was a permanent 1.0 that the runtime applied every frame.
#
# It runs the REAL graph -- voices, bus groups, insert nodes -- and still needs
# NO device: jce_audio_create_offline() keeps the shared output device off this
# engine and jce_audio_render_offline() pumps it instead.  That is deliberate.
# A case that self-ignored for want of sound hardware would have measured
# nothing on every machine this matters on, and SKIP is not PASS.
# jce_resource is not optional here even though nothing in the test names it:
# pulling jce_audio.c in at all drags jce_audio_decode_cpu's PAK and cooked-
# asset readers with it (jce_pak_find, jce_asset_open, ...).  test_jce_audio_
# routing gets away without it because it only ever touches jce_audio_mixer.c.
jce_add_unit_test(test_jce_audio_bus_meter
    SOURCES test_jce_audio_bus_meter.c
    LINK    jce_core jce_audio jce_resource)

# Voice priority when the 64-slot pool runs out.  Stealing used to read AGE
# and nothing else, so a boss cue was evicted by whatever started after it and
# an author had no way to say otherwise.  Both halves are asserted separately
# because either alone is useless: choosing a better victim still kills the
# protected voice once every voice is protected, and refusing without victim
# choice only means "dies last", which age already did.  Case B is the
# negative control keeping the protection cases from passing on a build that
# simply never steals; case C makes the protected voice the OLDEST so an
# age-only policy picks exactly the wrong victim.  Offline engine, no device.
jce_add_unit_test(test_jce_audio_voice_priority
    SOURCES test_jce_audio_voice_priority.c
    LINK    jce_core jce_audio jce_resource)

# spatial_blend as a CONTINUOUS value.  The runtime read `blend > 0.5f` and
# handed a bool to set_3d, so a float the inspector edits in 0.01 steps had two
# reachable states -- 0.0 and 0.49 bit-identical, 0.51 and 1.0 likewise.  The
# observable is a rendered LEVEL at a fixed distance, because the blend scales
# how far distance attenuation may pull the voice down; anything reading the
# authored volume would see nothing.  Case 1 is the positive control that the
# offline graph makes sound at all, without which every comparison here is
# between two silences.  Offline engine, no device.
jce_add_unit_test(test_jce_audio_spatial_blend
    SOURCES test_jce_audio_spatial_blend.c
    LINK    jce_core jce_audio jce_resource)

# The DEVICE side of an aux send.  jce_audio_mixer modelled sends for a long
# time and jce_audio_mixer_resolve_send had NO caller outside its own header
# and implementation -- the device offered only bus_create/bus_set_volume/
# voice_set_bus, so every bus was a flat child of Master with no parallel tap.
# The observable is the DESTINATION bus's meter: a send is a parallel tap, so
# the source's level is unchanged either way and says nothing.  Case 1 is the
# negative control that a bus with no voice and no send reads silence, without
# which every later case is satisfied by buses that are not isolated at all.
# Offline engine, no device.
jce_add_unit_test(test_jce_audio_bus_send_device
    SOURCES test_jce_audio_bus_send_device.c
    LINK    jce_core jce_audio jce_resource)

# Streaming clips: the ENCODED bytes stay resident and each voice decodes on
# demand, instead of one up-front decode into s16.  Every clip was
# decompress-on-load with no engine alternative -- a five-minute stereo track
# cost ~50 MB resident.  The load-bearing case is NOT "it makes sound" (the old
# path does too) but that jce_audio_get_pcm_data returns NULL for a streaming
# sound while the SAME payload loaded normally returns a buffer.  Offline
# engine, no device; the fixture is a hand-built 16-bit mono WAV because dr_wav
# is always compiled in, so the case does not depend on which optional codecs
# this build carries.
jce_add_unit_test(test_jce_audio_streaming_clip
    SOURCES test_jce_audio_streaming_clip.c
    LINK    jce_core jce_audio jce_resource)

# Interactive / adaptive music director (FEATURE 5.3): vertical stem layering
# driven by an intensity parameter, beat/bar-quantized horizontal transitions,
# and one-shot stingers.  The director math is exercised directly; the resolved
# layer gains are also proven through miniaudio's offline read path
# (ma_audio_buffer / ma_data_source_read_pcm_frames) with NO audio device, hence
# the explicit miniaudio link.
jce_add_unit_test(test_jce_music
    SOURCES test_jce_music.c
    LINK    jce_core jce_audio miniaudio::miniaudio)

# Pure PCM sizing primitive (header-only).  Needs engine/src on the include
# path to reach the audio-internal header jce_pcm_convert.h.
jce_add_unit_test(test_jce_pcm_convert
    SOURCES test_jce_pcm_convert.c
    LINK    jce_core)
target_include_directories(test_jce_pcm_convert PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")

# REF-022 (dedup audit) — backward-shift repair in the voice->bus open-addressed
# map.  unassign_voice used '>' where Knuth Algorithm R needs '>=', which
# stranded a colliding voice past the hole so its bus routing silently vanished.
# These tests construct the collision explicitly rather than hard-coding ids.
jce_add_unit_test(test_jce_audio_voicemap_shift
    SOURCES test_jce_audio_voicemap_shift.c
    LINK    jce_core jce_audio)

# The occlusion solver's output tracks the parameters it is handed, which is
# what makes handing it the AUTHORED AudioOcclusion values mean anything.
jce_add_unit_test(test_jce_audio_occlusion_params
    SOURCES test_jce_audio_occlusion_params.c
    LINK    jce_core jce_audio jce_resource)

# The reverb's three stages -- direct, early reflection, late tail -- measured
# as an IMPULSE RESPONSE, which is the only way to tell "the tail is delayed"
# from "the tail is quieter".  Six authored numbers reached no part of the DSP
# before this, and none of it could be asserted because the reverb had no
# offline entry point; the DSP chain beside it has had one all along.
jce_add_unit_test(test_jce_audio_reverb_stages
    SOURCES test_jce_audio_reverb_stages.c
    LINK    jce_core jce_audio jce_resource)

jce_add_unit_test(test_jce_audio_file SOURCES test_jce_audio_file.c LINK jce_core jce_audio)
