/*
 * jce_audio.c  miniaudio audio implementation.
 *
 * Loads WAV and OGG from PAK into PCM buffers, plays them via the
 * miniaudio engine.  Manages a pool of sounds and voices for
 * concurrent playback.
 */

#include <jce/middleware/audio/jce_audio.h>
#include <jce/middleware/audio/jce_m4a_decode.h>
#include <jce/os/core/jce_log.h>
/* jce_fs_host_read_all, for the host branch of the streaming loader. */
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/os/core/jce_str.h>
#include <jce/resource/jce_pak_loader.h>
#include <jce/resource/jce_asset_format.h>

#include "jce_miniaudio_opus_backend.h"
/* The single fdk-aac wrapper lives in middleware/video; jce_audio links
 * jce_video PRIVATE, and jce_m4a_decode.c already reaches it the same way. */
#include "middleware/video/jce_aac_decode.h"

#ifndef JCE_NO_AUDIO

#include "os/core/jce_memory.h"
#include "resource/jce_asset_reader.h"
#include "middleware/audio/jce_pcm_convert.h"
#include "middleware/audio/jce_audio_decode.h"

#include <miniaudio.h>
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JCE_MAX_SOUNDS  64
#define JCE_MAX_VOICES  64
#define JCE_MAX_BUSES   16
#define JCE_BUS_NAME_MAX 32
#define JCE_AUDIO_SOUND_PATH_MAX 256   /* dedup key length for loaded clips */

/* ── Freeverb (public-domain Schroeder reverb) ─────────────────────────
 *
 * Jezar's Freeverb topology: 8 parallel low-pass comb filters summed into
 * 4 series all-pass filters per channel.  We vendor a compact, dependency-
 * free implementation and wrap it as a custom miniaudio node so authored
 * reverb zones drive a real DSP tail.  Tuning constants are the classic
 * Freeverb values (scaled per sample rate).  Public domain — no license. */

#define JCE_FV_NUM_COMBS    8
#define JCE_FV_NUM_ALLPASS  4
#define JCE_FV_MAX_CH       2

/* Comb/all-pass delay lengths (in frames @ 44100 Hz), Freeverb defaults.
 * The second channel adds a small stereo spread offset. */
static const int g_fv_comb_len[JCE_FV_NUM_COMBS] =
    { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
static const int g_fv_allpass_len[JCE_FV_NUM_ALLPASS] =
    { 556, 441, 341, 225 };
#define JCE_FV_STEREO_SPREAD 23

typedef struct {
    float *buf;
    int    size;
    int    pos;
    float  feedback;
    float  filterstore; /* one-pole LP state for damping */
    float  damp1, damp2;
} FvComb;

typedef struct {
    float *buf;
    int    size;
    int    pos;
    float  feedback;
} FvAllpass;

/* A delay line long enough for the pre-delay and the early taps.
 *
 * 250 ms at 48 kHz is 12000 frames; the preset's pre_delay_ms is documented
 * 0..~200 and the component's reflectionsDelay 0..0.3 s, so one line sized
 * for the larger of the two covers both with room to clamp. */
#define JCE_FV_MAX_PREDELAY_MS 320.0f

typedef struct {
    float *buf;
    int    size;
    int    pos;
} FvDelay;

static inline float fv_delay_tap(const FvDelay *d, int frames_back)
{
    if (!d->buf || d->size <= 0) return 0.0f;
    if (frames_back < 0) frames_back = 0;
    if (frames_back >= d->size) frames_back = d->size - 1;
    int i = d->pos - frames_back;
    while (i < 0) i += d->size;
    return d->buf[i];
}

static inline void fv_delay_push(FvDelay *d, float x)
{
    if (!d->buf || d->size <= 0) return;
    if (++d->pos >= d->size) d->pos = 0;
    d->buf[d->pos] = x;
}

typedef struct {
    FvComb    comb[JCE_FV_MAX_CH][JCE_FV_NUM_COMBS];
    FvAllpass allpass[JCE_FV_MAX_CH][JCE_FV_NUM_ALLPASS];
    /* One line per channel, tapped twice: once for the late tail's PRE-DELAY
     * (the silence before a room answers) and once for the EARLY REFLECTION
     * (the first bounce off a wall, which arrives before the diffuse tail and
     * is what tells a listener how big the room is). */
    FvDelay   predelay[JCE_FV_MAX_CH];
    int       channels;
    int       sample_rate;
    float     wet;       /* wet output gain */
    float     dry;       /* dry passthrough gain */
    float     roomsize;  /* comb feedback (0..~0.98) */
    float     damp;      /* comb LP damping (0..1) */
    int       predelay_frames;  /* late tail input delay */
    int       early_frames;     /* early reflection tap */
    float     early_gain;       /* 0 = no early reflection */
    bool      allocated;
} Freeverb;

/* Custom miniaudio node embedding a Freeverb. */
typedef struct {
    ma_node_base base;
    Freeverb     fv;
} ReverbNode;

/* ── Insert-effect DSP node (FEATURE 5.1) ───────────────────────────────
 *
 * A custom ma_node that runs an ordered JceAudioDspChain (EQ / compressor /
 * limiter / delay — implemented in jce_audio_dsp.c) on the f32 interleaved
 * signal passing through it.  One node may be spliced into a bus group's
 * output edge or a voice's output edge so the inserts process exactly the
 * signal flowing to the bus/endpoint.  The DSP math is device-independent and
 * unit-tested offline; here we just feed it the live node-graph buffer.
 *
 * It is also where a BUS IS METERED (jce_audio_bus_get_peak), which is why a
 * bus can have one with an empty chain.  Putting the meter here rather than
 * in a second node type is deliberate: this node already exists, already
 * copies the whole block, and is already spliced/torn down by one pair of
 * functions, so a meter costs one compare per sample and no new lifecycle.
 * What it measures is the node's OUTPUT -- post-chain, and post-fader
 * because the bus group applies its gain upstream of here. */
typedef struct {
    ma_node_base      base;
    JceAudioDspChain *chain;   /* owned; NULL = passthrough */
    bool              inited;
    /* Peak |sample| of the output since the last read, as raw float bits.
     * Written by the audio thread, read-and-cleared by any thread.  Bits are
     * compared as ints: for NON-NEGATIVE IEEE-754 floats the bit pattern
     * orders exactly as the value does, so max-of-bits IS max-of-floats and
     * the CAS loop needs no float round-trip. */
    SDL_AtomicInt     peak_bits;
} DspNode;

/* Each sound owns a block of decoded PCM (s16) data. */
typedef struct {
    void      *pcm_data;      /* JCE_MALLOC'd, s16 PCM */
    ma_uint64  frame_count;
    ma_uint32  channels;
    ma_uint32  sample_rate;
    /* Source path this slot was loaded from, or "" for a raw-PCM load.  Used to
     * dedup repeated path loads (jce_audio_load / jce_audio_load_memory): a clip
     * requested again returns the already-loaded slot instead of allocating a
     * new one, so a scene firing the same one-shot every few seconds cannot
     * exhaust the 64-slot table.  A slot is playback-shared: many voices carry
     * independent read cursors over the same PCM. */
    char       path[JCE_AUDIO_SOUND_PATH_MAX];
    /* STREAMING slots hold the ENCODED bytes instead of decoded PCM, and each
     * voice decodes them on demand through its own ma_decoder.  A five-minute
     * stereo track costs its compressed size resident (a few MB) rather than
     * ~50 MB of s16, and nothing is decoded up front.  pcm_data stays NULL for
     * these, which is what jce_audio_get_pcm_data reports and what makes a
     * streaming slot distinguishable from a decoded one. */
    void      *enc_data;      /* encoded bytes, or NULL; see enc_owned */
    size_t     enc_size;
    bool       streaming;
    /* enc_data is NOT always ours to free.  A STORED pak entry is a flat
     * pointer into the archive blob, so those bytes are already resident and
     * copying them would double the cost this entry point exists to avoid.
     * enc_owned says which case this is; enc_pak holds the reference that
     * keeps the blob alive underneath a borrowed pointer. */
    bool       enc_owned;
    const JcePakArchive *enc_pak;
    /* HOST path: nothing is held at all.  Each voice opens the file through
     * ma_decoder_init_file and reads it incrementally, so a five-minute track
     * costs its decoder buffers and nothing else.  The path is slot->path. */
    bool       from_file;
} SoundSlot;

/* A named output bus backed by a ma_sound_group node. */
typedef struct {
    bool           used;
    char           name[JCE_BUS_NAME_MAX];
    ma_sound_group group;     /* node: voices attach here, group -> reverb/endpoint */
    DspNode        dsp;       /* insert chain spliced group -> dsp -> reverb/endpoint */
    /* AUX SEND.  A console send is a PARALLEL tap: the bus keeps feeding its
     * normal output AND a scaled copy goes to another bus.  A single output
     * edge cannot express that, so the terminal feeds a splitter instead --
     * output 0 is the normal target, output 1 is the send, and the send
     * amount is that bus's output volume.
     *
     * Created lazily: a bus with no send has no splitter and its graph is
     * byte-identical to what it was before sends existed. */
    ma_splitter_node send_split;
    bool             split_inited;
    /* Parallel arrays indexed by SLOT; slot k is splitter output bus k+1,
     * output 0 being the bus's normal path.  dest -1 = free. */
    int              send_dest[JCE_AUDIO_BUS_MAX_SENDS];
    float            send_amount[JCE_AUDIO_BUS_MAX_SENDS];
} BusSlot;

/* Each voice is an independent playback instance. */
typedef struct {
    ma_audio_buffer buffer;   /* owns a read cursor over the SoundSlot PCM */
    ma_sound        sound;    /* attached to the engine */
    bool            inited;
    int             sound_slot;
    uint32_t        generation; /* bumped on teardown; packed into JceVoice to reject stale handles */
    uint64_t        play_seq;   /* allocation order, for oldest-voice stealing when the pool is full */
    int             priority;   /* higher survives; 0 = normal.  See jce_audio.h
                                 * for why 0 and not Unity's inverted scale. */
    /* Streaming voices decode from the sound's encoded bytes.  Per VOICE, not
     * per sound: two voices on one streaming clip need independent read
     * cursors, exactly as two ma_audio_buffers do over shared PCM. */
    ma_decoder      dec;
    bool            dec_inited;
    ma_lpf_node     lpf;        /* occlusion muffle filter, inserted sound→lpf→endpoint */
    bool            lpf_ok;
    float           lpf_cutoff; /* current cutoff Hz; avoids reinit churn when unchanged */
    DspNode         dsp;        /* insert chain spliced (sound/lpf)→dsp→target */
    bool            vol_gated;  /* auto-stopped by the volume gate (see
                                 * jce_audio_set_volume): a LOOPING voice at
                                 * volume ~0 still decodes/mixes every audio
                                 * callback — on wasm that work lands on the
                                 * MAIN thread.  Gate stops it and restarts
                                 * transparently when the volume rises. */
    /* Streaming voices: custom data source instead of ma_audio_buffer. */
    bool                  is_stream;
    ma_data_source_base   stream_ds;
    JceAudioStreamPullFn  stream_on_read;
    void                 *stream_ud;
    ma_uint32             stream_channels;
    ma_uint32             stream_samplerate;
    ma_uint64             stream_cursor; /* frames pulled so far */
    int                   bus;        /* index into JceAudio.buses, or -1 (direct) */
} VoiceSlot;

struct JceAudio {
    ma_engine   engine;
    bool        engine_inited;
    /* True when this engine is NOT registered with the shared output device,
     * so nothing pumps it but the caller.  jce_audio_render_offline refuses
     * any other engine: two pumps on one graph race for the same read
     * cursors, and that has to be impossible rather than merely documented. */
    bool        offline;

    SoundSlot   sounds[JCE_MAX_SOUNDS];
    bool        sound_used[JCE_MAX_SOUNDS];

    VoiceSlot   voices[JCE_MAX_VOICES];
    uint64_t    play_counter;   /* monotonic; stamped into voice.play_seq on alloc */

    /* Named mixer buses (flat under Master).  Group nodes route into the
     * reverb node when present, otherwise to the engine endpoint. */
    BusSlot     buses[JCE_MAX_BUSES];

    /* Global reverb DSP (Freeverb).  Created lazily on first jce_audio_set_reverb
     * with a positive wet mix.  When live, all bus groups attach to it and it
     * feeds the endpoint; otherwise groups attach straight to the endpoint. */
    ReverbNode  reverb;
    bool        reverb_inited;
    float       reverb_wet;      /* cached send level for dry/wet ramp */
};

/* ── Freeverb DSP ──────────────────────────────────────────────────── */

static void fv_comb_set(FvComb *c, float feedback, float damp)
{
    c->feedback = feedback;
    c->damp1    = damp;
    c->damp2    = 1.0f - damp;
}

static inline float fv_comb_process(FvComb *c, float in)
{
    float out = c->buf[c->pos];
    c->filterstore = out * c->damp2 + c->filterstore * c->damp1;
    c->buf[c->pos] = in + c->filterstore * c->feedback;
    if (++c->pos >= c->size) c->pos = 0;
    return out;
}

static inline float fv_allpass_process(FvAllpass *a, float in)
{
    float bufout = a->buf[a->pos];
    float out    = -in + bufout;
    a->buf[a->pos] = in + bufout * a->feedback;
    if (++a->pos >= a->size) a->pos = 0;
    return out;
}

/* Allocate the comb/allpass delay lines for `channels` at `sample_rate`. */
static bool fv_alloc(Freeverb *fv, int channels, int sample_rate)
{
    if (channels < 1) channels = 1;
    if (channels > JCE_FV_MAX_CH) channels = JCE_FV_MAX_CH;
    fv->channels    = channels;
    fv->sample_rate = sample_rate > 0 ? sample_rate : 44100;
    float sr_scale  = (float)fv->sample_rate / 44100.0f;

    for (int ch = 0; ch < channels; ++ch) {
        int spread = ch * JCE_FV_STEREO_SPREAD;
        for (int i = 0; i < JCE_FV_NUM_COMBS; ++i) {
            int len = (int)((float)(g_fv_comb_len[i] + spread) * sr_scale);
            if (len < 1) len = 1;
            FvComb *c = &fv->comb[ch][i];
            c->buf = (float *)JCE_CALLOC((size_t)len, sizeof(float));
            if (!c->buf) return false;
            c->size = len; c->pos = 0; c->filterstore = 0.0f;
        }
        for (int i = 0; i < JCE_FV_NUM_ALLPASS; ++i) {
            int len = (int)((float)(g_fv_allpass_len[i] + spread) * sr_scale);
            if (len < 1) len = 1;
            FvAllpass *a = &fv->allpass[ch][i];
            a->buf = (float *)JCE_CALLOC((size_t)len, sizeof(float));
            if (!a->buf) return false;
            a->size = len; a->pos = 0; a->feedback = 0.5f;
        }
        {
            int dl = (int)(JCE_FV_MAX_PREDELAY_MS * 0.001f
                           * (float)fv->sample_rate) + 2;
            FvDelay *d = &fv->predelay[ch];
            d->buf = (float *)JCE_CALLOC((size_t)dl, sizeof(float));
            if (!d->buf) return false;
            d->size = dl; d->pos = 0;
        }
    }
    fv->allocated = true;
    return true;
}

static void fv_free(Freeverb *fv)
{
    for (int ch = 0; ch < JCE_FV_MAX_CH; ++ch) {
        for (int i = 0; i < JCE_FV_NUM_COMBS; ++i) {
            JCE_FREE(fv->comb[ch][i].buf);
            fv->comb[ch][i].buf = NULL;
        }
        for (int i = 0; i < JCE_FV_NUM_ALLPASS; ++i) {
            JCE_FREE(fv->allpass[ch][i].buf);
            fv->allpass[ch][i].buf = NULL;
        }
        JCE_FREE(fv->predelay[ch].buf);
        fv->predelay[ch].buf = NULL;
        fv->predelay[ch].size = 0;
        fv->predelay[ch].pos  = 0;
    }
    fv->allocated = false;
}

/* Push tuning into the comb / allpass / delay coefficients.
 *
 * `diffusion` drives the ALLPASS feedback, which is what diffusion is: how
 * much a reflection is smeared rather than passed through.  Freeverb's fixed
 * 0.5 is the middle of the usable range, so 0.5 reproduces the old sound
 * exactly and the parameter opens it in both directions.  Clamped to 0.2..0.8
 * -- past that an allpass rings rather than diffuses, which is an effect but
 * not the one the slider is named after.
 *
 * `predelay_ms` and `early_ms` become tap offsets; `early_gain` 0 disables the
 * early tap entirely, so a preset that does not ask for one costs one branch. */
static void fv_set_params(Freeverb *fv, float wet, float dry,
                          float roomsize, float damp,
                          float diffusion, float predelay_ms,
                          float early_ms, float early_gain)
{
    if (wet < 0.0f) wet = 0.0f;
    if (dry < 0.0f) dry = 0.0f;
    if (roomsize < 0.0f) roomsize = 0.0f;
    if (roomsize > 0.98f) roomsize = 0.98f;
    if (damp < 0.0f) damp = 0.0f;
    if (damp > 1.0f) damp = 1.0f;
    fv->wet = wet; fv->dry = dry; fv->roomsize = roomsize; fv->damp = damp;
    for (int ch = 0; ch < fv->channels; ++ch)
        for (int i = 0; i < JCE_FV_NUM_COMBS; ++i)
            fv_comb_set(&fv->comb[ch][i], roomsize, damp);

    float ap = (diffusion <= 0.0f) ? 0.5f : diffusion;
    if (ap < 0.2f) ap = 0.2f;
    if (ap > 0.8f) ap = 0.8f;
    for (int ch = 0; ch < fv->channels; ++ch)
        for (int i = 0; i < JCE_FV_NUM_ALLPASS; ++i)
            fv->allpass[ch][i].feedback = ap;

    const float max_ms = JCE_FV_MAX_PREDELAY_MS;
    if (predelay_ms < 0.0f) predelay_ms = 0.0f;
    if (predelay_ms > max_ms) predelay_ms = max_ms;
    if (early_ms < 0.0f) early_ms = 0.0f;
    if (early_ms > max_ms) early_ms = max_ms;
    fv->predelay_frames = (int)(predelay_ms * 0.001f * (float)fv->sample_rate);
    fv->early_frames    = (int)(early_ms    * 0.001f * (float)fv->sample_rate);
    fv->early_gain      = (early_gain > 0.0f) ? early_gain : 0.0f;
}

/* Process one channel's block: out = dry*in + wet*(early + late(delayed in)).
 *
 * THE ORDER IS THE ROOM.  A sound reaches a listener three times: direct,
 * then off the nearest surfaces (the EARLY reflection, one clear echo), then
 * as the diffuse tail once the room has smeared everything together (the LATE
 * reverb, which starts after the PRE-DELAY).  Freeverb alone produced only
 * the third of those, starting immediately, which is why every room sounded
 * like the same room at a different volume. */
static void fv_process_channel(Freeverb *fv, int ch,
                               const float *in, float *out, ma_uint32 n)
{
    const float gain = 0.015f; /* Freeverb fixed input gain */
    FvDelay *dl = &fv->predelay[ch];
    for (ma_uint32 s = 0; s < n; ++s) {
        fv_delay_push(dl, in[s]);

        /* Late tail, fed by the pre-delayed input. */
        float x = fv_delay_tap(dl, fv->predelay_frames) * gain;
        float acc = 0.0f;
        for (int i = 0; i < JCE_FV_NUM_COMBS; ++i)
            acc += fv_comb_process(&fv->comb[ch][i], x);
        for (int i = 0; i < JCE_FV_NUM_ALLPASS; ++i)
            acc = fv_allpass_process(&fv->allpass[ch][i], acc);

        /* Early reflection: one clean tap, not through the network. */
        float early = (fv->early_gain > 0.0f)
                    ? fv_delay_tap(dl, fv->early_frames) * fv->early_gain
                    : 0.0f;

        out[s] = in[s] * fv->dry + (acc + early) * fv->wet;
    }
}

/* ── Custom ma_node wrapping Freeverb ──────────────────────────────── */

static void reverb_node_process(ma_node *node,
                                const float **frames_in, ma_uint32 *frame_count_in,
                                float **frames_out, ma_uint32 *frame_count_out)
{
    ReverbNode *rn = (ReverbNode *)node;
    Freeverb   *fv = &rn->fv;
    ma_uint32   n  = *frame_count_out;
    int         ch = fv->allocated ? fv->channels
                   : (int)ma_node_get_output_channels(node, 0);

    if (frame_count_in) {
        ma_uint32 in_n = frame_count_in[0];
        if (in_n < n) n = in_n;
    }
    if (!fv->allocated || ch < 1) {
        /* Passthrough if not ready. */
        if (n > 0 && ch >= 1)
            memcpy(frames_out[0], frames_in[0],
                   (size_t)n * (size_t)ch * sizeof(float));
        *frame_count_out = n;
        return;
    }

    /* Interleaved stereo (engine runs f32 interleaved per bus). De-interleave,
     * run per channel, re-interleave. */
    const float *in  = frames_in[0];
    float       *out = frames_out[0];
    static float scratch_in[JCE_FV_MAX_CH][4096];
    static float scratch_out[JCE_FV_MAX_CH][4096];
    ma_uint32 done = 0;
    while (done < n) {
        ma_uint32 blk = n - done;
        if (blk > 4096) blk = 4096;
        for (ma_uint32 s = 0; s < blk; ++s)
            for (int c = 0; c < ch; ++c)
                scratch_in[c][s] = in[(done + s) * ch + c];
        for (int c = 0; c < ch; ++c)
            fv_process_channel(fv, c, scratch_in[c], scratch_out[c], blk);
        for (ma_uint32 s = 0; s < blk; ++s)
            for (int c = 0; c < ch; ++c)
                out[(done + s) * ch + c] = scratch_out[c][s];
        done += blk;
    }
    *frame_count_out = n;
}

static ma_node_vtable g_reverb_vtable = {
    reverb_node_process,
    NULL,   /* onGetRequiredInputFrameCount */
    1,      /* input bus count  */
    1,      /* output bus count */
    0       /* flags */
};

/* ── Custom ma_node wrapping a JceAudioDspChain (insert effects) ──────── */

static void dsp_node_process(ma_node *node,
                             const float **frames_in, ma_uint32 *frame_count_in,
                             float **frames_out, ma_uint32 *frame_count_out)
{
    DspNode  *dn = (DspNode *)node;
    ma_uint32 n  = *frame_count_out;
    int       ch = (int)ma_node_get_output_channels(node, 0);

    if (frame_count_in) {
        ma_uint32 in_n = frame_count_in[0];
        if (in_n < n) n = in_n;
    }
    if (n == 0 || ch < 1) { *frame_count_out = n; return; }

    /* Copy input → output, then run the chain in place on the output. The
     * chain is a no-op when empty, so this is just a memcpy in that case. */
    memcpy(frames_out[0], frames_in[0],
           (size_t)n * (size_t)ch * sizeof(float));
    if (dn->chain)
        jce_audio_dsp_chain_process(dn->chain, frames_out[0], n);

    /* Meter the block AFTER the inserts, so the number is what this bus
     * actually put out.  `>` never holds for NaN, so a NaN sample is skipped
     * rather than pinning the meter at a value no reader can clear. */
    const float *o = frames_out[0];
    float        pk = 0.0f;
    const ma_uint32 cnt = n * (ma_uint32)ch;
    for (ma_uint32 i = 0; i < cnt; ++i) {
        float a = o[i] < 0.0f ? -o[i] : o[i];
        if (a > pk) pk = a;
    }
    if (pk > 0.0f) {
        int bits;
        memcpy(&bits, &pk, sizeof bits);
        for (;;) {   /* CAS-max: only this thread writes, but a reader may
                      * clear between our load and our store. */
            int cur = SDL_GetAtomicInt(&dn->peak_bits);
            if (cur >= bits) break;
            if (SDL_CompareAndSwapAtomicInt(&dn->peak_bits, cur, bits)) break;
        }
    }
    *frame_count_out = n;
}

static ma_node_vtable g_dsp_vtable = {
    dsp_node_process,
    NULL,   /* onGetRequiredInputFrameCount */
    1,      /* input bus count  */
    1,      /* output bus count */
    0       /* flags */
};

/* Forward decl: defined below near the voice routing helpers. */
static ma_node *voice_output_src(const VoiceSlot *v);

/* The node a bus's audio EXITS FROM -- the far end of its insert chain.
 *
 * A bus is `group -> [dsp] -> <output>`, so the terminal is the dsp node when
 * one has been stood up and the group otherwise.  Every site that re-routes a
 * bus has to attach THIS node, not the group, or it silently bypasses the
 * bus's insert effects.
 *
 * It is one function because the chain is going to grow: an aux send adds a
 * splitter after the dsp, and a conditional spelled out at each call site is
 * how the reverb re-route came to know about the dsp node separately from the
 * code that created it. */
static ma_node *bus_terminal_node(BusSlot *b)
{
    return b->dsp.inited ? (ma_node *)&b->dsp.base : (ma_node *)&b->group;
}

/* Create the global reverb node and re-route every live bus group through it.
 * On any failure the groups keep their endpoint attachment (dry-only). */
static bool audio_init_reverb(JceAudio *audio)
{
    if (audio->reverb_inited) return true;
    ma_engine *e   = &audio->engine;
    ma_uint32 chan = ma_engine_get_channels(e);
    ma_uint32 sr   = ma_engine_get_sample_rate(e);

    /* Freeverb handles mono/stereo only.  Surround endpoints stay dry. */
    if (chan < 1 || chan > JCE_FV_MAX_CH) {
        LOG_WARN("jce_audio", "reverb: %u-channel endpoint unsupported, staying dry", chan);
        return false;
    }

    if (!fv_alloc(&audio->reverb.fv, (int)chan, (int)sr)) {
        fv_free(&audio->reverb.fv);
        LOG_WARN("jce_audio", "reverb: freeverb alloc failed");
        return false;
    }
    fv_set_params(&audio->reverb.fv, 0.0f, 1.0f, 0.5f, 0.5f,
                  0.5f, 0.0f, 0.0f, 0.0f);

    ma_node_config cfg = ma_node_config_init();
    cfg.vtable          = &g_reverb_vtable;
    cfg.pInputChannels  = &chan;
    cfg.pOutputChannels = &chan;
    if (ma_node_init(ma_engine_get_node_graph(e), &cfg, NULL,
                     &audio->reverb.base) != MA_SUCCESS) {
        fv_free(&audio->reverb.fv);
        LOG_WARN("jce_audio", "reverb: ma_node_init failed");
        return false;
    }
    if (ma_node_attach_output_bus(&audio->reverb.base, 0,
                                  ma_engine_get_endpoint(e), 0) != MA_SUCCESS) {
        ma_node_uninit(&audio->reverb.base, NULL);
        fv_free(&audio->reverb.fv);
        LOG_WARN("jce_audio", "reverb: attach to endpoint failed");
        return false;
    }
    audio->reverb_inited = true;
    audio->reverb_wet    = 0.0f;

    /* Route any already-created buses through the reverb node.  It is the
     * bus's TERMINAL that moves, not its group -- attaching the group would
     * route around the bus's own insert effects. */
    for (int i = 0; i < JCE_MAX_BUSES; ++i) {
        if (audio->buses[i].used)
            ma_node_attach_output_bus(bus_terminal_node(&audio->buses[i]), 0,
                                      &audio->reverb.base, 0);
    }
    /* Re-route direct voices (no bus) that were attached to the endpoint
     * before the reverb node existed, so the global tail covers them too. */
    for (int i = 0; i < JCE_MAX_VOICES; ++i) {
        VoiceSlot *v = &audio->voices[i];
        if (!v->inited || v->bus >= 0) continue;
        ma_node *src = voice_output_src(v);
        ma_node_attach_output_bus(src, 0, &audio->reverb.base, 0);
    }
    LOG_SUCCESS("jce_audio", "reverb node (freeverb) initialized");
    return true;
}

/* The node a bus group / direct sound should feed: the reverb node if live,
 * otherwise the engine endpoint. */
static ma_node *audio_output_node(JceAudio *audio)
{
    if (audio->reverb_inited) return (ma_node *)&audio->reverb.base;
    return ma_engine_get_endpoint(&audio->engine);
}

/* Find a bus index by name (case-insensitive); -1 if none. */
static int audio_find_bus(const JceAudio *audio, const char *name)
{
    if (!name || !name[0]) return -1;
    for (int i = 0; i < JCE_MAX_BUSES; ++i)
        if (audio->buses[i].used &&
            SDL_strcasecmp(audio->buses[i].name, name) == 0)
            return i;
    return -1;
}

/* ── Insert-effect node lifecycle ──────────────────────────────────────
 *
 * A DspNode is created lazily the first time an effect is added to a bus or a
 * voice.  Standing it up splices it onto `src`'s output edge so the signal
 * flows  src → dsp → target  instead of  src → target.  On any failure the
 * existing edge is left intact (effects silently no-op) so audio still flows.
 */
static bool dsp_node_ensure(JceAudio *audio, DspNode *dn,
                            ma_node *src, ma_node *target)
{
    if (dn->inited) return true;
    ma_engine *e   = &audio->engine;
    ma_uint32 chan = ma_engine_get_channels(e);
    ma_uint32 sr   = ma_engine_get_sample_rate(e);

    dn->chain = jce_audio_dsp_chain_create(chan, sr);
    if (!dn->chain) {
        LOG_WARN("jce_audio", "insert: dsp chain alloc failed");
        return false;
    }
    SDL_SetAtomicInt(&dn->peak_bits, 0);   /* meter starts at silence */

    ma_node_config cfg = ma_node_config_init();
    cfg.vtable          = &g_dsp_vtable;
    cfg.pInputChannels  = &chan;
    cfg.pOutputChannels = &chan;
    if (ma_node_init(ma_engine_get_node_graph(e), &cfg, NULL,
                     &dn->base) != MA_SUCCESS) {
        jce_audio_dsp_chain_destroy(dn->chain);
        dn->chain = NULL;
        LOG_WARN("jce_audio", "insert: ma_node_init failed");
        return false;
    }
    /* Splice: src → dsp → target. */
    if (ma_node_attach_output_bus(&dn->base, 0, target, 0) != MA_SUCCESS
        || ma_node_attach_output_bus(src, 0, &dn->base, 0) != MA_SUCCESS) {
        ma_node_uninit(&dn->base, NULL);
        jce_audio_dsp_chain_destroy(dn->chain);
        dn->chain = NULL;
        LOG_WARN("jce_audio", "insert: splice failed");
        return false;
    }
    dn->inited = true;
    return true;
}

static void dsp_node_uninit(DspNode *dn)
{
    if (dn->inited) {
        ma_node_uninit(&dn->base, NULL);
        dn->inited = false;
    }
    if (dn->chain) {
        jce_audio_dsp_chain_destroy(dn->chain);
        dn->chain = NULL;
    }
}

/* ================================================================== */
/* Process-wide master mix (single shared output device)               */
/* ================================================================== */
/* Every JceAudio engine is DEVICE-LESS (ma_engine_config.noDevice — the
 * miniaudio-documented multi-engine form) and is pumped + summed by ONE
 * shared playback device, the engine's "master bus" (the same shape as
 * Unreal's master submix / Unity's AudioListener / Godot's Master bus).
 * Consequences:
 *   - the process opens exactly one OS audio stream, resident once created
 *     (Play start/stop no longer opens/closes devices);
 *   - jce_audio_master_tap_set() observes the final mixed PCM — sample-
 *     exact, cross-platform capture of everything THIS process plays
 *     (the editor recorder's audio source; see
 *     .docs/AUDIO_MASTER_MIX_DESIGN.md);
 *   - the tap stream is gapless by construction: the device callback runs
 *     at a constant cadence and sums to silence when engines are idle.
 * Threading: create/destroy/tap_set are main-thread (the existing JceAudio
 * contract); the mix callback runs on the device thread and never blocks —
 * slots publish via "pointer first, flag second" over SDL atomic ints
 * (full barriers), teardown waits one callback generation (RCU-lite). */

#define JCE_AUDIO_MAX_ENGINES 8
#define JCE_AUDIO_MASTER_RATE 48000u  /* Opus/recording expectation */
#define JCE_AUDIO_MASTER_CH   2u

typedef struct {
    SDL_AtomicInt live;   /* 1 = ptr readable by the mix callback */
    JceAudio     *ptr;
} MasterSlot;

static struct {
    bool                 device_inited;
    bool                 device_failed;  /* don't retry every create */
    ma_device            device;
    MasterSlot           slots[JCE_AUDIO_MAX_ENGINES];
    SDL_AtomicInt        generation;    /* bumped each callback entry */
    SDL_AtomicInt        tap_live;
    JceAudioMasterTapFn  tap_fn;
    void                *tap_ud;
} s_master;

/* Wait until the mix callback has entered at least once more, so a slot or
 * tap cleared BEFORE this call can no longer be referenced.  No-op when the
 * device never started (nothing runs the callback). */
static void master_wait_generation(void)
{
    if (!s_master.device_inited) return;
    int g0 = SDL_GetAtomicInt(&s_master.generation);
    for (int spin = 0; spin < 400; ++spin) {   /* ~2 s worst case */
        if (SDL_GetAtomicInt(&s_master.generation) != g0) return;
        SDL_Delay(5);
    }
}

static void master_device_cb(ma_device *dev, void *out, const void *in,
                             ma_uint32 frames)
{
    (void)dev; (void)in;
    SDL_AddAtomicInt(&s_master.generation, 1);

    float *dst = (float *)out;   /* f32/2ch as requested at device init */
    memset(dst, 0, (size_t)frames * JCE_AUDIO_MASTER_CH * sizeof(float));

    enum { CHUNK = 1024 };
    static float tmp[CHUNK * JCE_AUDIO_MASTER_CH];  /* device thread only */
    ma_uint32 done = 0;
    while (done < frames) {
        ma_uint32 n = frames - done;
        if (n > CHUNK) n = CHUNK;
        float *acc = dst + (size_t)done * JCE_AUDIO_MASTER_CH;
        for (int i = 0; i < JCE_AUDIO_MAX_ENGINES; ++i) {
            if (!SDL_GetAtomicInt(&s_master.slots[i].live)) continue;
            JceAudio *a = s_master.slots[i].ptr;
            if (!a) continue;
            ma_uint64 read = 0;
            ma_engine_read_pcm_frames(&a->engine, tmp, n, &read);
            const ma_uint32 cnt = (ma_uint32)read * JCE_AUDIO_MASTER_CH;
            for (ma_uint32 s = 0; s < cnt; ++s) acc[s] += tmp[s];
        }
        done += n;
    }

    if (SDL_GetAtomicInt(&s_master.tap_live) && s_master.tap_fn)
        s_master.tap_fn(s_master.tap_ud, dst, frames,
                        JCE_AUDIO_MASTER_RATE, JCE_AUDIO_MASTER_CH);
}

static bool master_device_ensure(void)
{
    if (s_master.device_inited) return true;
    if (s_master.device_failed) return false;

    ma_device_config dc = ma_device_config_init(ma_device_type_playback);
    dc.playback.format   = ma_format_f32;
    dc.playback.channels = JCE_AUDIO_MASTER_CH;
    dc.sampleRate        = JCE_AUDIO_MASTER_RATE;
    dc.dataCallback      = master_device_cb;

    if (ma_device_init(NULL, &dc, &s_master.device) != MA_SUCCESS) {
        s_master.device_failed = true;
        LOG_WARN("jce_audio", "master mix device init failed — engines run "
                              "silent, master tap unavailable");
        return false;
    }
    if (ma_device_start(&s_master.device) != MA_SUCCESS) {
        ma_device_uninit(&s_master.device);
        s_master.device_failed = true;
        LOG_WARN("jce_audio", "master mix device start failed");
        return false;
    }
    s_master.device_inited = true;
    LOG_SUCCESS("jce_audio", "master mix device started (48kHz/2ch)");
    return true;
}

static bool master_register(JceAudio *a)
{
    for (int i = 0; i < JCE_AUDIO_MAX_ENGINES; ++i) {
        if (SDL_GetAtomicInt(&s_master.slots[i].live) ||
            s_master.slots[i].ptr)
            continue;
        s_master.slots[i].ptr = a;                  /* pointer first… */
        SDL_SetAtomicInt(&s_master.slots[i].live, 1); /* …flag second */
        return true;
    }
    return false;
}

static void master_unregister(JceAudio *a)
{
    for (int i = 0; i < JCE_AUDIO_MAX_ENGINES; ++i) {
        if (s_master.slots[i].ptr != a) continue;
        SDL_SetAtomicInt(&s_master.slots[i].live, 0);
        master_wait_generation();   /* callback can no longer touch it */
        s_master.slots[i].ptr = NULL;
        return;
    }
}

bool jce_audio_master_tap_set(JceAudioMasterTapFn fn, void *ud)
{
    if (fn) {
        if (!master_device_ensure()) return false;
        s_master.tap_fn = fn;                    /* fields first… */
        s_master.tap_ud = ud;
        SDL_SetAtomicInt(&s_master.tap_live, 1); /* …flag second */
        return true;
    }
    SDL_SetAtomicInt(&s_master.tap_live, 0);
    master_wait_generation();   /* no further calls into the old fn */
    s_master.tap_fn = NULL;
    s_master.tap_ud = NULL;
    return true;
}

/* -- Lifecycle ------------------------------------------------------ */

/* Both engine flavours differ in exactly one thing: whether the shared output
 * device pumps this graph, or the caller does. */
static JceAudio *audio_create_common(bool offline)
{
    JceAudio *audio = (JceAudio *)JCE_CALLOC(1, sizeof(*audio));
    if (!audio) return NULL;

    /* Device-less engine pinned to the master-mix format; the shared device
     * pumps it via ma_engine_read_pcm_frames (see master mix above). */
    ma_engine_config cfg = ma_engine_config_init();
    cfg.noDevice   = MA_TRUE;
    cfg.channels   = JCE_AUDIO_MASTER_CH;
    cfg.sampleRate = JCE_AUDIO_MASTER_RATE;
    if (ma_engine_init(&cfg, &audio->engine) != MA_SUCCESS) {
        LOG_ERROR("jce_audio", "ma_engine_init failed");
        JCE_FREE(audio);
        return NULL;
    }

    audio->engine_inited = true;
    audio->offline       = offline;

    if (!offline) {
        /* Best effort: without an output device the engine still works
         * (silent). */
        master_device_ensure();
        if (!master_register(audio)) {
            LOG_ERROR("jce_audio", "master mix slots exhausted (%d live engines)",
                      JCE_AUDIO_MAX_ENGINES);
            ma_engine_uninit(&audio->engine);
            JCE_FREE(audio);
            return NULL;
        }
    }

    for (int i = 0; i < JCE_MAX_VOICES; i++) {
        audio->voices[i].sound_slot = -1;
        audio->voices[i].bus        = -1;
    }

    LOG_SUCCESS("jce_audio", "miniaudio engine initialized%s",
                offline ? " (offline)" : "");
    return audio;
}

JceAudio *jce_audio_create(void)         { return audio_create_common(false); }
JceAudio *jce_audio_create_offline(void) { return audio_create_common(true);  }

static void uninit_voice(VoiceSlot *v)
{
    if (!v->inited) return;
    ma_sound_uninit(&v->sound);
    /* Insert chain node sits downstream of sound/lpf — uninit it first. */
    dsp_node_uninit(&v->dsp);
    if (v->lpf_ok) {
        ma_lpf_node_uninit(&v->lpf, NULL);
        v->lpf_ok = false;
    }
    if (v->is_stream) {
        ma_data_source_uninit(&v->stream_ds);
        v->is_stream = false;
        v->stream_on_read = NULL;
        v->stream_ud = NULL;
    } else if (v->dec_inited) {
        /* A STREAMING CLIP's decoder.  Distinct from is_stream above, which is
         * the caller-driven pull ring: this one owns a ma_decoder over the
         * sound's encoded bytes and must be torn down with the voice, or the
         * next occupant of this slot inherits a live decoder. */
        ma_decoder_uninit(&v->dec);
        v->dec_inited = false;
    } else {
        ma_audio_buffer_uninit(&v->buffer);
    }
    v->inited = false;
    v->sound_slot = -1;
    v->bus = -1;
    v->generation++;   /* invalidate any outstanding JceVoice handle to this slot */
}

void jce_audio_destroy(JceAudio *audio)
{
    if (!audio) return;

    /* FIRST detach from the master mix — after this the shared device
     * callback can no longer pump this engine, making the teardown below
     * race-free against the audio thread. */
    master_unregister(audio);

    /* Uninit all voices first (they reference engine). */
    for (int i = 0; i < JCE_MAX_VOICES; i++)
        uninit_voice(&audio->voices[i]);

    /* Bus groups (nodes) — uninit before the reverb node/engine they feed.
     * A bus insert node is downstream of its group, so uninit it first. */
    for (int i = 0; i < JCE_MAX_BUSES; i++) {
        if (audio->buses[i].used) {
            /* Splitter first: it sits DOWNSTREAM of the group and the dsp, and
             * uninitialising a node its input still feeds is what the comment
             * above this loop is about. */
            if (audio->buses[i].split_inited) {
                ma_splitter_node_uninit(&audio->buses[i].send_split, NULL);
                audio->buses[i].split_inited = false;
            }
            dsp_node_uninit(&audio->buses[i].dsp);
            ma_sound_group_uninit(&audio->buses[i].group);
            audio->buses[i].used = false;
        }
    }

    /* Global reverb node + its delay lines. */
    if (audio->reverb_inited) {
        ma_node_uninit(&audio->reverb.base, NULL);
        audio->reverb_inited = false;
    }
    fv_free(&audio->reverb.fv);

    /* Free all sound PCM data. */
    for (int i = 0; i < JCE_MAX_SOUNDS; i++) {
        if (audio->sound_used[i]) {
            JCE_FREE(audio->sounds[i].pcm_data);
            /* Only OUR copy: a STORED pak entry was borrowed, not allocated. */
            if (audio->sounds[i].enc_owned)
                JCE_FREE(audio->sounds[i].enc_data);
            if (audio->sounds[i].enc_pak)
                jce_pak_close((JcePakArchive *)(uintptr_t)audio->sounds[i].enc_pak);
            audio->sounds[i].pcm_data = NULL;
            audio->sounds[i].enc_data = NULL;
            audio->sounds[i].enc_pak  = NULL;
            audio->sound_used[i] = false;
        }
    }

    if (audio->engine_inited)
        ma_engine_uninit(&audio->engine);

    JCE_FREE(audio);
    LOG_INFO("jce_audio", "audio system destroyed");
}

/* -- Sound loading -------------------------------------------------- */

static int alloc_buffer_slot(const JceAudio *audio)
{
    for (int i = 0; i < JCE_MAX_SOUNDS; i++) {
        if (!audio->sound_used[i])
            return i;
    }
    return -1;
}

/* Return the slot already holding `path` (case-insensitive, matching the VFS
 * key normalization), or -1 if none.  Empty paths (raw-PCM loads) never match. */
static int find_sound_slot_by_path(const JceAudio *audio, const char *path)
{
    if (!path || !path[0]) return -1;
    for (int i = 0; i < JCE_MAX_SOUNDS; i++) {
        if (audio->sound_used[i] && audio->sounds[i].path[0] &&
            jce_strcasecmp(audio->sounds[i].path, path) == 0)
            return i;
    }
    return -1;
}

/* Record the source path on a loaded slot so future loads of the same clip
 * dedup to it.  Truncates safely if the path exceeds the key length (a
 * truncated key still dedups consistently within one session). */
static void set_sound_slot_path(JceAudio *audio, int slot, const char *path)
{
    if (slot < 0 || slot >= JCE_MAX_SOUNDS || !path) return;
    snprintf(audio->sounds[slot].path, sizeof(audio->sounds[slot].path),
             "%s", path);
}

/* Decode into an audio sound slot (registration on the owning thread). */
static JceSound load_from_memory(JceAudio *audio, int slot,
                                  const uint8_t *data, size_t size,
                                  const char *path)
{
    int16_t  *pcm = NULL;
    ma_uint64 frames = 0;
    ma_uint32 channels = 0, rate = 0;
    if (!jce_audio_decode_pcm_mem(data, size, path, &pcm, &frames, &channels, &rate))
        return JCE_SOUND_INVALID;

    audio->sounds[slot].pcm_data    = pcm;
    audio->sounds[slot].frame_count = frames;
    audio->sounds[slot].channels    = channels;
    audio->sounds[slot].sample_rate = rate;
    audio->sound_used[slot]         = true;
    return (JceSound)(slot + 1);
}

JceSound jce_audio_load_pcm(JceAudio *audio,
                             const void *pcm_data, uint32_t pcm_size,
                             uint16_t channels, uint32_t sample_rate,
                             uint16_t bits_per_sample)
{
    if (!audio || !pcm_data || pcm_size == 0 || channels == 0)
        return JCE_SOUND_INVALID;

    int slot = alloc_buffer_slot(audio);
    if (slot < 0) return JCE_SOUND_INVALID;

    /* Convert 8-bit to 16-bit if needed, or just copy 16-bit. */
    ma_uint64 frame_count;
    void *pcm_copy;

    if (bits_per_sample == 8) {
        /* Whole frames only — the allocation size and the conversion loop
         * MUST use the same sample count, or a clip whose byte count is not a
         * multiple of the channel count overflows the heap (audit R2F8). */
        size_t n_samples = jce_pcm_u8_to_s16_samples(pcm_size, channels);
        if (n_samples == 0) return JCE_SOUND_INVALID;
        frame_count = (ma_uint64)(n_samples / channels);
        pcm_copy = JCE_MALLOC(n_samples * sizeof(int16_t));
        if (!pcm_copy) return JCE_SOUND_INVALID;

        /* Convert u8 → s16. */
        const uint8_t *src = (const uint8_t *)pcm_data;
        int16_t *dst = (int16_t *)pcm_copy;
        for (size_t i = 0; i < n_samples; i++)
            dst[i] = (int16_t)((src[i] - 128) * 256);
    } else if (bits_per_sample == 16) {
        frame_count = pcm_size / (channels * 2);
        pcm_copy = JCE_MALLOC(pcm_size);
        if (!pcm_copy) return JCE_SOUND_INVALID;
        memcpy(pcm_copy, pcm_data, pcm_size);
    } else {
        return JCE_SOUND_INVALID;
    }

    audio->sounds[slot].pcm_data    = pcm_copy;
    audio->sounds[slot].frame_count = frame_count;
    audio->sounds[slot].channels    = channels;
    audio->sounds[slot].sample_rate = sample_rate;
    audio->sounds[slot].path[0]     = '\0';  /* raw-PCM: no dedup key until a
                                              * path-aware caller records one */
    audio->sound_used[slot]         = true;

    return (JceSound)(slot + 1);
}

struct JceAudioCpu {
    void    *pcm;
    uint32_t pcm_bytes;
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t bits;
};

JceAudioCpu *jce_audio_decode_cpu_memory(const void *data, size_t size,
                                         const char *hint_path)
{
    const char *path = hint_path ? hint_path : "<memory>";
    if (!data || size == 0) return NULL;

    JceAudioCpu *c = (JceAudioCpu *)JCE_CALLOC(1, sizeof(*c));
    if (!c) return NULL;

    if (jce_asset_is_cooked(data, size)) {
        JceAssetView view;
        if (!jce_asset_open(&view, data, size) ||
            !view.header || view.header->asset_type != JCEASSET_TYPE_SOUND) {
            LOG_ERROR("jce_audio",
                      "cooked asset '%s' is not a valid sound representation",
                      path);
            JCE_FREE(c);
            return NULL;
        }

        const JceAssetChunkEntry *info_c =
            jce_asset_find_chunk(&view, JCEASSET_CHUNK_AUDIO_INFO);
        const JceAssetChunkEntry *pcm_c =
            jce_asset_find_chunk(&view, JCEASSET_CHUNK_AUDIO_PCM);
        if (!info_c || !pcm_c || pcm_c->original_size > UINT32_MAX) {
            LOG_ERROR("jce_audio", "cooked sound '%s' has invalid chunks", path);
            JCE_FREE(c);
            return NULL;
        }

        JceAssetAudioInfo ainfo;
        if (jce_asset_chunk_data(&view, info_c, &ainfo, sizeof(ainfo)) == 0 ||
            ainfo.channels == 0 ||
            (ainfo.bits_per_sample != 8 && ainfo.bits_per_sample != 16)) {
            LOG_ERROR("jce_audio", "cooked sound '%s' has invalid metadata", path);
            JCE_FREE(c);
            return NULL;
        }

        uint32_t pcm_size = (uint32_t)pcm_c->original_size;
        void *pcm_data = JCE_MALLOC(pcm_size ? pcm_size : 1u);
        if (!pcm_data) {
            JCE_FREE(c);
            return NULL;
        }
        if (jce_asset_chunk_data(&view, pcm_c, pcm_data, pcm_size) == 0) {
            JCE_FREE(pcm_data);
            JCE_FREE(c);
            return NULL;
        }

        c->pcm         = pcm_data;
        c->pcm_bytes   = pcm_size;
        c->channels    = ainfo.channels;
        c->sample_rate = ainfo.sample_rate;
        c->bits        = ainfo.bits_per_sample;
        return c;
    }

    int16_t  *pcm = NULL;
    ma_uint64 frames = 0;
    ma_uint32 channels = 0, rate = 0;
    if (!jce_audio_decode_pcm_mem((const uint8_t *)data, size, path, &pcm, &frames,
                        &channels, &rate) ||
        frames > UINT32_MAX / (channels ? channels * sizeof(int16_t) : 1u)) {
        if (pcm) JCE_FREE(pcm);
        JCE_FREE(c);
        return NULL;
    }

    c->pcm         = pcm;
    c->pcm_bytes   = (uint32_t)(frames * channels * sizeof(int16_t));
    c->channels    = (uint16_t)channels;
    c->sample_rate = rate;
    c->bits        = 16;
    return c;
}

static JceSound jce_audio_load_inner(JceAudio *audio, const JcePakArchive *pak,
                                     const char *path)
{
    if (!audio || !pak || !path) return JCE_SOUND_INVALID;

    /* Dedup: reuse the slot if this clip is already resident. */
    int cached = find_sound_slot_by_path(audio, path);
    if (cached >= 0) return (JceSound)(cached + 1);

    const JcePakAsset *asset = jce_pak_find(pak, path);
    if (!asset) {
        LOG_ERROR("jce_audio", "asset '%s' not found in PAK", path);
        return JCE_SOUND_INVALID;
    }

    /* Decompress asset from PAK. */
    void *raw = JCE_MALLOC((size_t)asset->original_size);
    if (!raw) return JCE_SOUND_INVALID;

    size_t decoded = jce_pak_decompress(asset, raw, (size_t)asset->original_size);
    if (decoded == 0) {
        LOG_ERROR("jce_audio", "decompress failed for '%s'", path);
        JCE_FREE(raw);
        return JCE_SOUND_INVALID;
    }

    JceAudioCpu *cpu = jce_audio_decode_cpu_memory(raw, decoded, path);
    JCE_FREE(raw);
    JceSound snd = jce_audio_upload_cpu(audio, cpu);
    if (snd != JCE_SOUND_INVALID)
        set_sound_slot_path(audio, (int)snd - 1, path);
    return snd;
}

JceSound jce_audio_load(JceAudio *audio, const JcePakArchive *pak, const char *path)
{
    JCE_PROFILE_ZONE_N("Audio::Load");
    JceSound result = jce_audio_load_inner(audio, pak, path);
    JCE_PROFILE_ZONE_END;
    return result;
}

JceSound jce_audio_load_streaming(JceAudio *audio, const JcePakArchive *pak,
                                  const char *path)
{
    if (!audio || !path || !path[0]) return JCE_SOUND_INVALID;

    /* Dedup like jce_audio_load: a clip already resident IS that slot, so a
     * second request cannot produce a second copy of the encoded bytes.  A
     * slot already loaded DECODED is returned as-is rather than converted --
     * two callers disagreeing about how one clip is held is worse than one of
     * them getting residency it did not ask for, and the decoded copy is the
     * one that already has voices reading it. */
    int existing = find_sound_slot_by_path(audio, path);
    if (existing >= 0) return (JceSound)(existing + 1);

    int slot = alloc_buffer_slot(audio);
    if (slot < 0) {
        LOG_WARN("jce_audio", "sound pool exhausted loading '%s'", path);
        return JCE_SOUND_INVALID;
    }

    /* Nothing is DECODED here -- that is the whole point.  How much is even
     * RESIDENT depends on where the clip lives, and the cheapest case is the
     * shipping one:
     *
     *   pak, STORED entry  -> the encoded bytes are already in the archive
     *                         blob, so borrow the pointer.  ZERO extra bytes.
     *                         jce_archive_writer stores an entry raw when ZSTD
     *                         cannot beat its keep threshold, and .ogg / .mp3
     *                         never do, so this is the normal case for music
     *                         in a single-file exe.
     *   pak, compressed    -> the container was re-compressed, so it has to be
     *                         expanded once and held: compressed-in-memory,
     *                         which is the best available for that entry.
     *   no pak (host file) -> hold NOTHING.  Each voice opens the file with
     *                         ma_decoder_init_file and reads it incrementally.
     */
    void  *enc       = NULL;
    size_t size      = 0;
    bool   owned     = false;
    bool   from_file = false;
    const JcePakArchive *keep_pak = NULL;

    if (pak) {
        const JcePakAsset *a = jce_pak_find(pak, path);
        if (!a || a->original_size == 0) {
            LOG_ERROR("jce_audio", "stream: '%s' not in PAK", path);
            return JCE_SOUND_INVALID;
        }
        if ((a->flags & JCE_PAK_ASSET_STORED) && a->compressed_data) {
            enc   = (void *)(uintptr_t)a->compressed_data;
            size  = (size_t)a->compressed_size;
            owned = false;
            /* Borrowing into the blob must not outlive it: take a reference so
             * a jce_pak_close elsewhere cannot leave this slot pointing at
             * freed memory.  The cast drops const only to touch the refcount,
             * which is itself thread-safe. */
            keep_pak = jce_pak_acquire((JcePakArchive *)(uintptr_t)pak);
        } else {
            enc = JCE_MALLOC((size_t)a->original_size);
            if (!enc) return JCE_SOUND_INVALID;
            size = jce_pak_decompress(a, enc, (size_t)a->original_size);
            if (size == 0) {
                LOG_ERROR("jce_audio", "stream: decompress failed for '%s'", path);
                JCE_FREE(enc);
                return JCE_SOUND_INVALID;
            }
            owned = true;
        }
    } else {
        from_file = true;
    }

    /* Prove it is DECODABLE before handing back a handle.  A slot that only
     * fails when someone plays it turns a load error into a silent voice far
     * from its cause, and this costs one decoder init rather than a decode. */
    ma_decoder probe;
    const ma_result probe_res =
        from_file ? ma_decoder_init_file(path, NULL, &probe)
                  : ma_decoder_init_memory(enc, size, NULL, &probe);
    if (probe_res != MA_SUCCESS) {
        LOG_ERROR("jce_audio", "stream: cannot decode '%s'", path);
        if (owned)    JCE_FREE(enc);
        if (keep_pak) jce_pak_close((JcePakArchive *)(uintptr_t)keep_pak);
        return JCE_SOUND_INVALID;
    }
    ma_uint64 frames = 0;
    (void)ma_decoder_get_length_in_pcm_frames(&probe, &frames);
    ma_uint32 chans = probe.outputChannels;
    ma_uint32 rate  = probe.outputSampleRate;
    ma_decoder_uninit(&probe);

    SoundSlot *ss   = &audio->sounds[slot];
    ss->pcm_data    = NULL;          /* nothing decoded: this IS the marker */
    ss->enc_data    = enc;           /* NULL when streaming straight off disk */
    ss->enc_size    = size;
    ss->enc_owned   = owned;
    ss->enc_pak     = keep_pak;
    ss->from_file   = from_file;
    ss->streaming   = true;
    ss->frame_count = frames;
    ss->channels    = chans;
    ss->sample_rate = rate;
    audio->sound_used[slot] = true;
    set_sound_slot_path(audio, slot, path);
    return (JceSound)(slot + 1);
}

/* ── Worker-decode + main-thread-register split ───────────────────── */

JceAudioCpu *jce_audio_decode_cpu(const JcePakArchive *pak, const char *path)
{
    if (!pak || !path) return NULL;

    const JcePakAsset *asset = jce_pak_find(pak, path);
    if (!asset) {
        LOG_ERROR("jce_audio", "asset '%s' not found in PAK", path);
        return NULL;
    }

    void *raw = JCE_MALLOC((size_t)asset->original_size);
    if (!raw) return NULL;
    size_t decoded = jce_pak_decompress(asset, raw, (size_t)asset->original_size);
    if (decoded == 0) {
        LOG_ERROR("jce_audio", "decompress failed for '%s'", path);
        JCE_FREE(raw);
        return NULL;
    }

    JceAudioCpu *c = jce_audio_decode_cpu_memory(raw, decoded, path);
    JCE_FREE(raw);
    return c;
}

JceSound jce_audio_upload_cpu(JceAudio *audio, JceAudioCpu *c)
{
    if (!c) return JCE_SOUND_INVALID;
    JceSound s = JCE_SOUND_INVALID;
    if (audio && c->pcm && c->pcm_bytes > 0)
        s = jce_audio_load_pcm(audio, c->pcm, c->pcm_bytes,
                               c->channels, c->sample_rate, c->bits);
    jce_audio_cpu_free(c);
    return s;
}

void jce_audio_cpu_free(JceAudioCpu *c)
{
    if (!c) return;
    if (c->pcm) JCE_FREE(c->pcm);
    JCE_FREE(c);
}

bool jce_audio_cpu_get_pcm(const JceAudioCpu *c,
                           const void **out_pcm, uint32_t *out_pcm_bytes,
                           uint16_t *out_channels, uint32_t *out_sample_rate,
                           uint16_t *out_bits_per_sample)
{
    if (!c || !c->pcm || c->pcm_bytes == 0) return false;
    if (out_pcm)             *out_pcm             = c->pcm;
    if (out_pcm_bytes)       *out_pcm_bytes       = c->pcm_bytes;
    if (out_channels)        *out_channels        = c->channels;
    if (out_sample_rate)     *out_sample_rate     = c->sample_rate;
    if (out_bits_per_sample) *out_bits_per_sample = c->bits;
    return true;
}

void jce_audio_unload(JceAudio *audio, JceSound snd)
{
    if (!audio || snd == JCE_SOUND_INVALID) return;
    int slot = (int)snd - 1;
    if (slot < 0 || slot >= JCE_MAX_SOUNDS || !audio->sound_used[slot]) return;

    /* Stop and uninit any voice using this sound. */
    for (int i = 0; i < JCE_MAX_VOICES; i++) {
        if (audio->voices[i].inited && audio->voices[i].sound_slot == slot)
            uninit_voice(&audio->voices[i]);
    }

    JCE_FREE(audio->sounds[slot].pcm_data);
    audio->sounds[slot].pcm_data = NULL;
    /* A streaming slot holds ENCODED bytes instead of PCM, so freeing only
     * pcm_data would leak the whole clip -- but ONLY when the bytes are ours.
     * A STORED pak entry was borrowed from the archive blob, and handing that
     * interior pointer to JCE_FREE would corrupt the heap.  Voices were
     * uninited above, so no decoder is still reading either way. */
    if (audio->sounds[slot].enc_owned)
        JCE_FREE(audio->sounds[slot].enc_data);
    if (audio->sounds[slot].enc_pak)
        jce_pak_close((JcePakArchive *)(uintptr_t)audio->sounds[slot].enc_pak);
    audio->sounds[slot].enc_data  = NULL;
    audio->sounds[slot].enc_owned = false;
    audio->sounds[slot].enc_pak   = NULL;
    audio->sounds[slot].enc_size  = 0;
    audio->sounds[slot].from_file = false;
    audio->sounds[slot].streaming = false;
    audio->sounds[slot].path[0]  = '\0';
    audio->sound_used[slot] = false;
}

/* -- Playback ------------------------------------------------------- */

/* JceVoice handle = (generation << 16) | (slot + 1). The +1 keeps a valid
   handle non-zero (0 == JCE_VOICE_INVALID); the generation makes a handle to
   a since-recycled slot resolve as stale instead of controlling a new sound. */
static JceVoice pack_voice(JceAudio *audio, int slot)
{
    uint32_t gen = audio->voices[slot].generation & 0xFFFFu;
    return (JceVoice)((gen << 16) | (((uint32_t)slot + 1u) & 0xFFFFu));
}

/* Decode + validate a handle to a live slot index, or -1 if stale/invalid. */
static int resolve_voice(const JceAudio *audio, JceVoice voice)
{
    if (!audio || voice == JCE_VOICE_INVALID) return -1;
    int idx = (int)((uint32_t)voice & 0xFFFFu) - 1;
    uint32_t gen = ((uint32_t)voice >> 16) & 0xFFFFu;
    if (idx < 0 || idx >= JCE_MAX_VOICES) return -1;
    if (!audio->voices[idx].inited) return -1;
    if ((audio->voices[idx].generation & 0xFFFFu) != gen) return -1;
    return idx;
}

/* `priority` is the INCOMING sound's, used both to choose a victim and to
 * refuse the allocation outright when nothing is stealable.  It is stamped
 * onto the slot on success, so a voice carries the priority it was played at
 * until someone calls jce_audio_voice_set_priority. */
static int alloc_voice(JceAudio *audio, int priority)
{
    int found = -1;

    /* 1: an unused slot. */
    for (int i = 0; i < JCE_MAX_VOICES && found < 0; i++)
        if (!audio->voices[i].inited) found = i;

    /* 2: reclaim a finished (no longer playing) voice. */
    if (found < 0) {
        for (int i = 0; i < JCE_MAX_VOICES && found < 0; i++) {
            if (!ma_sound_is_playing(&audio->voices[i].sound)) {
                uninit_voice(&audio->voices[i]);
                found = i;
            }
        }
    }

    /* 3: virtualization — the pool is full and everything is playing.
     *
     * The victim is the LEAST IMPORTANT voice, not merely the oldest.  Age
     * alone is what let a boss cue or a line of dialogue be stolen by
     * whatever happened to start after it, which is the whole reason
     * `priority` exists.  Ordering, worst victim first:
     *
     *   1. lowest priority
     *   2. at equal priority, prefer a ONE-SHOT over a loop -- background
     *      music being cut is the failure the age-only policy was already
     *      written to avoid, and that intent survives intact here
     *   3. at equal priority and equal loopiness, the oldest
     *
     * AND THE REFUSAL, which is the half that makes the protection real: a
     * voice is never stolen by a sound of strictly lower priority.  Picking a
     * better victim still evicts the boss cue once every voice IS a boss cue,
     * so when nothing is stealable the new sound is dropped instead. */
    if (found < 0) {
        int      best      = -1;
        int      best_prio = 0;
        bool     best_loop = false;
        uint64_t best_seq  = 0;
        for (int i = 0; i < JCE_MAX_VOICES; i++) {
            const VoiceSlot *c = &audio->voices[i];
            if (!c->inited) continue;
            if (c->priority > priority) continue;   /* outranks the newcomer */

            bool c_loop = ma_sound_is_looping((ma_sound *)&c->sound) != MA_FALSE;
            if (best < 0
                || c->priority < best_prio
                || (c->priority == best_prio && !c_loop && best_loop)
                || (c->priority == best_prio && c_loop == best_loop
                    && c->play_seq < best_seq)) {
                best = i; best_prio = c->priority;
                best_loop = c_loop; best_seq = c->play_seq;
            }
        }
        if (best < 0) return -1;      /* every live voice outranks this sound */
        uninit_voice(&audio->voices[best]);
        found = best;
    }

    if (found >= 0) {
        audio->voices[found].play_seq = ++audio->play_counter;
        audio->voices[found].priority = priority;
    }
    return found;
}

/* The last node in a voice's local chain — the edge that feeds the bus/global
   output.  When inserts are present the dsp node is terminal; otherwise the
   lpf (if any), else the sound itself. */
static ma_node *voice_output_src(const VoiceSlot *v)
{
    if (v->dsp.inited) return (ma_node *)&v->dsp.base;
    if (v->lpf_ok)     return (ma_node *)&v->lpf;
    return (ma_node *)&v->sound;
}

/* The node a voice should feed into: its assigned bus group when routed,
   otherwise the global output node (reverb if live, else endpoint). */
static ma_node *voice_target_node(JceAudio *audio, const VoiceSlot *v)
{
    if (v->bus >= 0 && v->bus < JCE_MAX_BUSES && audio->buses[v->bus].used)
        return (ma_node *)&audio->buses[v->bus].group;
    return audio_output_node(audio);
}

/* Insert a per-voice low-pass node (sound → lpf → target) for occlusion
   muffling, starting at bypass (22050 Hz). On ANY failure the sound keeps its
   default endpoint attachment, so audio still plays (just without the muffle).
   `target` is the voice's bus group or the global output node. */
static void voice_attach_lpf(JceAudio *audio, VoiceSlot *v)
{
    ma_engine *e = &audio->engine;
    ma_lpf_node_config cfg = ma_lpf_node_config_init(
        ma_engine_get_channels(e), ma_engine_get_sample_rate(e), 22050.0, 2);
    if (ma_lpf_node_init(ma_engine_get_node_graph(e), &cfg, NULL, &v->lpf) != MA_SUCCESS)
        return;
    ma_node *target = voice_target_node(audio, v);
    if (ma_node_attach_output_bus(&v->lpf, 0, target, 0) != MA_SUCCESS
        || ma_node_attach_output_bus(&v->sound, 0, &v->lpf, 0) != MA_SUCCESS) {
        ma_lpf_node_uninit(&v->lpf, NULL);
        return;
    }
    v->lpf_ok     = true;
    v->lpf_cutoff = 22050.0f;
}

JceVoice jce_audio_play(JceAudio *audio, JceSound snd,
                         bool loop, float volume, float pitch)
{
    /* The pre-priority signature, kept as the common case rather than
     * duplicated: every existing caller means "normal importance". */
    return jce_audio_play_priority(audio, snd, loop, volume, pitch,
                                   JCE_AUDIO_PRIORITY_NORMAL);
}

JceVoice jce_audio_play_priority(JceAudio *audio, JceSound snd,
                         bool loop, float volume, float pitch, int priority)
{
    JCE_PROFILE_ZONE_N("Audio::Play");
    if (!audio || snd == JCE_SOUND_INVALID) { JCE_PROFILE_ZONE_END; return JCE_VOICE_INVALID; }

    int buf_slot = (int)snd - 1;
    if (buf_slot < 0 || buf_slot >= JCE_MAX_SOUNDS
        || !audio->sound_used[buf_slot]) {
        JCE_PROFILE_ZONE_END;
        return JCE_VOICE_INVALID;
    }

    int vi = alloc_voice(audio, priority);
    if (vi < 0) {
        LOG_WARN("jce_audio", "no free voices");
        JCE_PROFILE_ZONE_END;
        return JCE_VOICE_INVALID;
    }

    SoundSlot *s = &audio->sounds[buf_slot];
    VoiceSlot *v = &audio->voices[vi];

    ma_data_source *src = NULL;
    if (s->streaming) {
        /* STREAMING: decode the sound's encoded bytes on demand.  Own decoder
         * per voice so two voices on one clip keep independent cursors -- the
         * same reason each non-streaming voice gets its own ma_audio_buffer.
         * The encoded blob itself is shared and read-only. */
        const ma_result dres =
            s->from_file ? ma_decoder_init_file(s->path, NULL, &v->dec)
                         : ma_decoder_init_memory(s->enc_data, s->enc_size,
                                                  NULL, &v->dec);
        if (dres != MA_SUCCESS) {
            LOG_ERROR("jce_audio", "stream decoder init failed for '%s'",
                      s->path);
            JCE_PROFILE_ZONE_END;
            return JCE_VOICE_INVALID;
        }
        v->dec_inited = true;
        src = &v->dec;
    } else {
        /* Create an audio buffer that references the sound's PCM data.
           Each voice gets its own buffer with an independent read cursor. */
        ma_audio_buffer_config buf_cfg = ma_audio_buffer_config_init(
            ma_format_s16, s->channels, s->frame_count, s->pcm_data, NULL);
        buf_cfg.sampleRate = s->sample_rate;

        if (ma_audio_buffer_init(&buf_cfg, &v->buffer) != MA_SUCCESS) {
            LOG_ERROR("jce_audio", "ma_audio_buffer_init failed");
            JCE_PROFILE_ZONE_END;
            return JCE_VOICE_INVALID;
        }
        src = &v->buffer;
    }

    if (ma_sound_init_from_data_source(&audio->engine,
            src, 0, NULL, &v->sound) != MA_SUCCESS) {
        LOG_ERROR("jce_audio", "ma_sound_init_from_data_source failed");
        if (v->dec_inited) { ma_decoder_uninit(&v->dec); v->dec_inited = false; }
        else               { ma_audio_buffer_uninit(&v->buffer); }
        JCE_PROFILE_ZONE_END;
        return JCE_VOICE_INVALID;
    }

    ma_sound_set_volume(&v->sound, volume);
    ma_sound_set_pitch(&v->sound, pitch);
    ma_sound_set_looping(&v->sound, loop ? MA_TRUE : MA_FALSE);
    v->vol_gated = false;   /* fresh voice: no stale gate from slot reuse */
    if (loop && volume <= 0.001f) {
        /* Born-silent looping voice (ambience bed / playlist track parked at
         * 0): start it GATED instead of mixing silence forever.  Without
         * this, voices whose volume never changes after play (so
         * jce_audio_set_volume never runs) bypass the volume gate entirely —
         * this scene ships 8 such beds.  jce_audio_set_volume un-gates the
         * moment the mixer raises them. */
        v->vol_gated = true;
    } else {
        ma_sound_start(&v->sound);
    }

    v->inited = true;
    v->sound_slot = buf_slot;
    voice_attach_lpf(audio, v);
    JCE_PROFILE_ZONE_END;
    return pack_voice(audio, vi);
}

/* ── Streaming source ──────────────────────────────────────────── */

/* miniaudio passes &v->stream_ds (NOT a VoiceSlot*) to vtable callbacks.
 * Recover the enclosing VoiceSlot via offsetof container_of. */
static inline VoiceSlot *voice_from_ds(ma_data_source *ds) {
    return (VoiceSlot *)((char *)ds - offsetof(VoiceSlot, stream_ds));
}

static ma_result stream_ds_on_read(ma_data_source *ds,
                                    void *out, ma_uint64 frame_count,
                                    ma_uint64 *frames_read)
{
    VoiceSlot *v = voice_from_ds(ds);
    if (!v->stream_on_read) {
        if (frames_read) *frames_read = 0;
        return MA_AT_END;
    }
    /* Cap to uint32 — miniaudio buffers are small per callback. */
    uint32_t want = frame_count > 0xffffffffull
        ? 0xffffffffu : (uint32_t)frame_count;
    uint32_t got = v->stream_on_read(v->stream_ud,(int16_t *)out,want);
    if (got > want) got = want;
    /* Short reads mean under-run/EOF, not the end of this live voice.
     * The owning transport decides when to stop; the device receives silence. */
    if (got < want)
        memset((int16_t *)out+(size_t)got*v->stream_channels,0,
               (size_t)(want-got)*v->stream_channels*sizeof(int16_t));
    v->stream_cursor += want;
    if (frames_read) *frames_read = want;
    return MA_SUCCESS;
}

static ma_result stream_ds_on_seek(ma_data_source *ds, ma_uint64 frame_index)
{
    (void)ds; (void)frame_index;
    /* Seeks are driven by the upstream JceAudioStream, not via miniaudio. */
    return MA_NOT_IMPLEMENTED;
}

static ma_result stream_ds_on_get_data_format(ma_data_source *ds,
                                              ma_format *format,
                                              ma_uint32 *channels,
                                              ma_uint32 *sample_rate,
                                              ma_channel *channel_map,
                                              size_t channel_map_cap)
{
    VoiceSlot *v = voice_from_ds(ds);
    if (format) *format = ma_format_s16;
    if (channels) *channels = v->stream_channels;
    if (sample_rate) *sample_rate = v->stream_samplerate;
    (void)channel_map; (void)channel_map_cap;
    return MA_SUCCESS;
}

static ma_result stream_ds_on_get_cursor(ma_data_source *ds,
                                          ma_uint64 *cursor)
{
    VoiceSlot *v = voice_from_ds(ds);
    if (cursor) *cursor = v->stream_cursor;
    return MA_SUCCESS;
}

static ma_data_source_vtable g_stream_vtable = {
    stream_ds_on_read,
    stream_ds_on_seek,
    stream_ds_on_get_data_format,
    stream_ds_on_get_cursor,
    NULL, /* onGetLength: unknown for streams */
    NULL, /* onSetLooping */
    0
};

JceVoice jce_audio_play_stream(JceAudio *audio,
                                JceAudioStreamPullFn on_read, void *ud,
                                uint16_t channels, uint32_t sample_rate,
                                float volume, float pitch)
{
    JCE_PROFILE_ZONE_N("Audio::PlayStream");
    if (!audio || !on_read || channels == 0 || sample_rate == 0) {
        JCE_PROFILE_ZONE_END;
        return JCE_VOICE_INVALID;
    }

    int vi = alloc_voice(audio, JCE_AUDIO_PRIORITY_NORMAL);
    if (vi < 0) {
        LOG_WARN("jce_audio", "no free voices for stream");
        JCE_PROFILE_ZONE_END;
        return JCE_VOICE_INVALID;
    }

    VoiceSlot *v = &audio->voices[vi];

    ma_data_source_config ds_cfg = ma_data_source_config_init();
    ds_cfg.vtable = &g_stream_vtable;
    if (ma_data_source_init(&ds_cfg, &v->stream_ds) != MA_SUCCESS) {
        LOG_ERROR("jce_audio", "ma_data_source_init failed");
        JCE_PROFILE_ZONE_END;
        return JCE_VOICE_INVALID;
    }

    v->is_stream         = true;
    v->stream_on_read    = on_read;
    v->stream_ud         = ud;
    v->stream_channels   = channels;
    v->stream_samplerate = sample_rate;
    v->stream_cursor     = 0;

    if (ma_sound_init_from_data_source(&audio->engine,
            &v->stream_ds, 0, NULL, &v->sound) != MA_SUCCESS) {
        LOG_ERROR("jce_audio", "ma_sound_init_from_data_source (stream) failed");
        ma_data_source_uninit(&v->stream_ds);
        v->is_stream = false;
        JCE_PROFILE_ZONE_END;
        return JCE_VOICE_INVALID;
    }

    ma_sound_set_volume(&v->sound, volume);
    ma_sound_set_pitch(&v->sound, pitch);
    ma_sound_set_looping(&v->sound, MA_FALSE);
    ma_sound_start(&v->sound);

    v->inited     = true;
    v->vol_gated  = false;  /* fresh voice: no stale gate from slot reuse */
    v->sound_slot = -1;
    voice_attach_lpf(audio, v);
    JCE_PROFILE_ZONE_END;
    return pack_voice(audio, vi);
}

void jce_audio_stop(JceAudio *audio, JceVoice voice)
{
    if (!audio || voice == JCE_VOICE_INVALID) return;
    int idx = resolve_voice(audio, voice);
    if (idx < 0 || idx >= JCE_MAX_VOICES || !audio->voices[idx].inited) return;

    uninit_voice(&audio->voices[idx]);
}

/* Occlusion muffle: set the per-voice low-pass cutoff (22050 = bypass). No-op
   if the voice has no lpf node. Skips reinit for sub-perceptual changes. */
void jce_audio_set_lowpass(JceAudio *audio, JceVoice voice, float cutoff_hz)
{
    int idx = resolve_voice(audio, voice);
    if (idx < 0) return;
    VoiceSlot *v = &audio->voices[idx];
    if (!v->lpf_ok) return;
    if (cutoff_hz < 20.0f)    cutoff_hz = 20.0f;
    if (cutoff_hz > 22050.0f) cutoff_hz = 22050.0f;
    if (cutoff_hz > v->lpf_cutoff - 25.0f && cutoff_hz < v->lpf_cutoff + 25.0f)
        return;
    ma_lpf_config lcfg = ma_lpf_config_init(
        ma_format_f32, ma_engine_get_channels(&audio->engine),
        ma_engine_get_sample_rate(&audio->engine), (double)cutoff_hz, 2);
    if (ma_lpf_node_reinit(&lcfg, &v->lpf) == MA_SUCCESS)
        v->lpf_cutoff = cutoff_hz;
}

void jce_audio_pause(JceAudio *audio, JceVoice voice)
{
    if (!audio || voice == JCE_VOICE_INVALID) return;
    int idx = resolve_voice(audio, voice);
    if (idx < 0 || idx >= JCE_MAX_VOICES || !audio->voices[idx].inited) return;

    audio->voices[idx].vol_gated = false;   /* explicit pause supersedes the gate */
    ma_sound_stop(&audio->voices[idx].sound);
}

void jce_audio_resume(JceAudio *audio, JceVoice voice)
{
    if (!audio || voice == JCE_VOICE_INVALID) return;
    int idx = resolve_voice(audio, voice);
    if (idx < 0 || idx >= JCE_MAX_VOICES || !audio->voices[idx].inited) return;

    audio->voices[idx].vol_gated = false;   /* explicit resume supersedes the gate */
    if (!ma_sound_is_playing(&audio->voices[idx].sound))
        ma_sound_start(&audio->voices[idx].sound);
}

void jce_audio_set_volume(JceAudio *audio, JceVoice voice, float volume)
{
    if (!audio || voice == JCE_VOICE_INVALID) return;
    int idx = resolve_voice(audio, voice);
    if (idx < 0 || idx >= JCE_MAX_VOICES || !audio->voices[idx].inited) return;

    VoiceSlot *v = &audio->voices[idx];
    ma_sound_set_volume(&v->sound, volume);

    /* Volume gate (LOOPING voices only): a looping voice at volume ~0 still
     * decodes + mixes every audio callback.  Crossfaded playlists / ambience
     * beds commonly park many silent loops (this project: 3 streamed music
     * tracks + ambience, all playing at 0) — cheap on desktop's audio thread,
     * but on wasm the callback runs on the MAIN thread and the dead decode
     * work becomes frame time.  Silent looping voices are transparently
     * stopped and restarted the moment their volume rises; one-shots are
     * left alone (stopping one would end it, not pause it). */
    if (volume <= 0.001f) {
        if (!v->vol_gated && ma_sound_is_looping(&v->sound)
            && ma_sound_is_playing(&v->sound)) {
            ma_sound_stop(&v->sound);
            v->vol_gated = true;
        }
    } else if (v->vol_gated) {
        v->vol_gated = false;
        if (!ma_sound_is_playing(&v->sound))
            ma_sound_start(&v->sound);
    }
}

void jce_audio_set_pitch(JceAudio *audio, JceVoice voice, float pitch)
{
    if (!audio || voice == JCE_VOICE_INVALID) return;
    int idx = resolve_voice(audio, voice);
    if (idx < 0 || idx >= JCE_MAX_VOICES || !audio->voices[idx].inited) return;

    ma_sound_set_pitch(&audio->voices[idx].sound, pitch);
}

void jce_audio_set_looping(JceAudio *audio, JceVoice voice, bool loop)
{
    if (!audio || voice == JCE_VOICE_INVALID) return;
    int idx = resolve_voice(audio, voice);
    if (idx < 0 || idx >= JCE_MAX_VOICES || !audio->voices[idx].inited) return;

    ma_sound_set_looping(&audio->voices[idx].sound, loop ? MA_TRUE : MA_FALSE);
}

bool jce_audio_is_playing(const JceAudio *audio, JceVoice voice)
{
    if (!audio || voice == JCE_VOICE_INVALID) return false;
    int idx = resolve_voice(audio, voice);
    if (idx < 0 || idx >= JCE_MAX_VOICES || !audio->voices[idx].inited) return false;

    return ma_sound_is_playing(&audio->voices[idx].sound) != 0;
}

/* -- Global --------------------------------------------------------- */

void jce_audio_set_master_volume(JceAudio *audio, float volume)
{
    if (!audio) return;
    ma_engine_set_volume(&audio->engine, volume);
}

void jce_audio_stop_all(JceAudio *audio)
{
    if (!audio) return;
    for (int i = 0; i < JCE_MAX_VOICES; i++)
        uninit_voice(&audio->voices[i]);
}

/* -- Mixer buses ---------------------------------------------------- */

bool jce_audio_bus_create(JceAudio *audio, const char *name)
{
    if (!audio || !name || !name[0]) return false;
    if (SDL_strcasecmp(name, "Master") == 0) return true; /* implicit endpoint */
    if (audio_find_bus(audio, name) >= 0) return true;     /* already exists */

    int slot = -1;
    for (int i = 0; i < JCE_MAX_BUSES; i++)
        if (!audio->buses[i].used) { slot = i; break; }
    if (slot < 0) {
        LOG_WARN("jce_audio", "bus pool exhausted (max %d)", JCE_MAX_BUSES);
        return false;
    }

    BusSlot *b = &audio->buses[slot];
    /* Group attaches to the engine endpoint by default; we re-route it to the
     * reverb node when one is live so bus audio is reverberated. */
    if (ma_sound_group_init(&audio->engine, 0, NULL, &b->group) != MA_SUCCESS) {
        LOG_ERROR("jce_audio", "ma_sound_group_init failed for bus '%s'", name);
        return false;
    }
    if (audio->reverb_inited)
        ma_node_attach_output_bus(&b->group, 0, &audio->reverb.base, 0);
    snprintf(b->name, sizeof(b->name), "%s", name);
    b->used = true;
    return true;
}

void jce_audio_bus_set_volume(JceAudio *audio, const char *name, float volume)
{
    if (!audio || !name) return;
    if (volume < 0.0f) volume = 0.0f;
    if (SDL_strcasecmp(name, "Master") == 0) {
        ma_engine_set_volume(&audio->engine, volume);
        return;
    }
    int idx = audio_find_bus(audio, name);
    if (idx < 0) return;
    ma_sound_group_set_volume(&audio->buses[idx].group, volume);
}

void jce_audio_voice_set_priority(JceAudio *audio, JceVoice voice, int priority)
{
    int idx = resolve_voice(audio, voice);
    if (idx < 0) return;
    /* Only the stored value moves.  Nothing is re-sorted here because the
     * policy is evaluated at ALLOCATION time -- this takes effect on the next
     * steal, which is the only moment priority can mean anything. */
    audio->voices[idx].priority = priority;
}

int jce_audio_voice_get_priority(const JceAudio *audio, JceVoice voice)
{
    int idx = resolve_voice(audio, voice);
    return idx < 0 ? JCE_AUDIO_PRIORITY_NORMAL : audio->voices[idx].priority;
}

void jce_audio_voice_set_bus(JceAudio *audio, JceVoice voice,
                             const char *bus_name)
{
    if (!audio || voice == JCE_VOICE_INVALID) return;
    int idx = resolve_voice(audio, voice);
    if (idx < 0) return;
    VoiceSlot *v = &audio->voices[idx];

    int bus = audio_find_bus(audio, bus_name);  /* -1 => direct/Master */
    v->bus = bus;

    /* Reattach the voice's terminal output edge (dsp/lpf/sound) to the new
     * target.  On failure the prior attachment stays, so audio keeps flowing
     * (just on the old bus). */
    ma_node *target = voice_target_node(audio, v);
    ma_node *src    = voice_output_src(v);
    ma_node_attach_output_bus(src, 0, target, 0);
}

/* -- Insert-effect DSP chains (FEATURE 5.1) ------------------------- */

/* Ensure a bus's insert node exists (splicing group → dsp → output) and
 * return its chain, or NULL on failure. */
static JceAudioDspChain *bus_chain_ensure(JceAudio *audio, const char *bus_name)
{
    int idx = audio_find_bus(audio, bus_name);
    if (idx < 0) return NULL;
    BusSlot *b = &audio->buses[idx];
    /* The dsp goes BEFORE the splitter when one exists, so the insert chain
     * feeds both the normal output and the send: group -> dsp -> splitter.
     * Targeting audio_output_node() unconditionally would re-attach the dsp
     * straight to the endpoint and silently orphan the splitter -- the send
     * would keep reporting its amount through get_send and carry no audio,
     * which is the exact failure mode this whole row is about. */
    ma_node *target = b->split_inited ? (ma_node *)&b->send_split.base
                                      : audio_output_node(audio);
    if (!dsp_node_ensure(audio, &b->dsp, (ma_node *)&b->group, target))
        return NULL;
    return b->dsp.chain;
}

/* Ensure a voice's insert node exists (splicing lpf/sound → dsp → target) and
 * return its chain, or NULL on failure. */
static JceAudioDspChain *voice_chain_ensure(JceAudio *audio, JceVoice voice)
{
    int idx = resolve_voice(audio, voice);
    if (idx < 0) return NULL;
    VoiceSlot *v = &audio->voices[idx];
    /* Upstream of the (to-be) dsp node: the lpf if present, else the sound. */
    ma_node *upstream = v->lpf_ok ? (ma_node *)&v->lpf : (ma_node *)&v->sound;
    if (!dsp_node_ensure(audio, &v->dsp, upstream, voice_target_node(audio, v)))
        return NULL;
    return v->dsp.chain;
}

/* -- Bus metering (feeds sidechain ducking) ------------------------- */

float jce_audio_bus_get_peak(JceAudio *audio, const char *name)
{
    if (!audio) return 0.0f;
    /* Standing the insert node up is what turns metering on: it is the node
     * that measures.  A metering-only bus therefore carries an EMPTY chain,
     * which dsp_node_process already handles as a plain copy.  Reusing
     * bus_chain_ensure keeps one splice path and one teardown path for both
     * uses -- a separate meter node would have duplicated both. */
    if (!bus_chain_ensure(audio, name)) return 0.0f;
    int idx = audio_find_bus(audio, name);
    if (idx < 0) return 0.0f;

    /* Read-and-clear, so the caller sees the peak since ITS last call and the
     * accumulator cannot hold a stale maximum forever. */
    int bits = SDL_SetAtomicInt(&audio->buses[idx].dsp.peak_bits, 0);
    float pk = 0.0f;
    memcpy(&pk, &bits, sizeof pk);
    return pk;
}

/* -- Offline rendering (no device) ---------------------------------- */

bool jce_audio_render_offline(JceAudio *audio, float *out, uint32_t frames)
{
    if (!audio || !audio->engine_inited || !out || frames == 0) return false;
    if (!audio->offline) {
        /* The shared device is already pumping this graph.  Refusing is the
         * whole point of the flag: a second pump would advance the same read
         * cursors from another thread and the caller would get a plausible
         * buffer built from frames the device also consumed. */
        LOG_WARN("jce_audio",
                 "render_offline on a device-pumped engine; "
                 "use jce_audio_create_offline()");
        return false;
    }

    /* Silence FIRST, then read over it.  An engine with nothing attached to
     * its endpoint reads ZERO frames -- miniaudio has no silence to mix, so
     * it produces none -- and a caller handed a short block would either pad
     * it or write a gap into whatever it is feeding.  The shared device
     * callback solves this the same way (memset, then sum), which is what
     * makes the master tap gapless by construction; the offline pump has no
     * business being less reliable than the thing it stands in for. */
    const ma_uint32 chan = ma_engine_get_channels(&audio->engine);
    memset(out, 0, (size_t)frames * (size_t)chan * sizeof(float));

    ma_uint64 read = 0;
    return ma_engine_read_pcm_frames(&audio->engine, out, frames, &read)
           == MA_SUCCESS;
}

int jce_audio_bus_add_effect(JceAudio *audio, const char *bus_name,
                             const JceAudioEffectDesc *desc)
{
    if (!audio || !desc) return -1;
    JceAudioDspChain *chain = bus_chain_ensure(audio, bus_name);
    if (!chain) return -1;
    return jce_audio_dsp_chain_add(chain, desc);
}

int jce_audio_voice_add_effect(JceAudio *audio, JceVoice voice,
                               const JceAudioEffectDesc *desc)
{
    if (!audio || !desc) return -1;
    JceAudioDspChain *chain = voice_chain_ensure(audio, voice);
    if (!chain) return -1;
    return jce_audio_dsp_chain_add(chain, desc);
}

bool jce_audio_bus_set_effect(JceAudio *audio, const char *bus_name,
                              uint32_t index, const JceAudioEffectDesc *desc)
{
    if (!audio || !desc) return false;
    int idx = audio_find_bus(audio, bus_name);
    if (idx < 0 || !audio->buses[idx].dsp.inited) return false;
    return jce_audio_dsp_chain_set(audio->buses[idx].dsp.chain, index, desc);
}

bool jce_audio_voice_set_effect(JceAudio *audio, JceVoice voice,
                                uint32_t index, const JceAudioEffectDesc *desc)
{
    if (!audio || !desc) return false;
    int idx = resolve_voice(audio, voice);
    if (idx < 0 || !audio->voices[idx].dsp.inited) return false;
    return jce_audio_dsp_chain_set(audio->voices[idx].dsp.chain, index, desc);
}

bool jce_audio_bus_remove_effect(JceAudio *audio, const char *bus_name,
                                 uint32_t index)
{
    if (!audio) return false;
    int idx = audio_find_bus(audio, bus_name);
    if (idx < 0 || !audio->buses[idx].dsp.inited) return false;
    return jce_audio_dsp_chain_remove(audio->buses[idx].dsp.chain, index);
}

bool jce_audio_voice_remove_effect(JceAudio *audio, JceVoice voice,
                                   uint32_t index)
{
    if (!audio) return false;
    int idx = resolve_voice(audio, voice);
    if (idx < 0 || !audio->voices[idx].dsp.inited) return false;
    return jce_audio_dsp_chain_remove(audio->voices[idx].dsp.chain, index);
}

uint32_t jce_audio_bus_effect_count(JceAudio *audio, const char *bus_name)
{
    if (!audio) return 0;
    int idx = audio_find_bus(audio, bus_name);
    if (idx < 0 || !audio->buses[idx].dsp.inited) return 0;
    return jce_audio_dsp_chain_count(audio->buses[idx].dsp.chain);
}

uint32_t jce_audio_voice_effect_count(JceAudio *audio, JceVoice voice)
{
    if (!audio) return 0;
    int idx = resolve_voice(audio, voice);
    if (idx < 0 || !audio->voices[idx].dsp.inited) return 0;
    return jce_audio_dsp_chain_count(audio->voices[idx].dsp.chain);
}

/* -- Global reverb -------------------------------------------------- */

bool jce_audio_reverb_process_offline(const JceAudioReverbParams *params,
                                      float *io, uint32_t frames, int channels,
                                      int sample_rate)
{
    if (!params || !io || frames == 0) return false;
    if (channels < 1) channels = 1;
    if (channels > JCE_FV_MAX_CH) channels = JCE_FV_MAX_CH;
    if (sample_rate <= 0) sample_rate = 48000;

    Freeverb fv;
    memset(&fv, 0, sizeof fv);
    if (!fv_alloc(&fv, channels, sample_rate)) { fv_free(&fv); return false; }

    /* The same preset -> tuning mapping jce_audio_set_reverb applies.  Kept
     * beside it rather than factored out only because the live path also
     * owns lazy node creation and routing; the ARITHMETIC is identical, and
     * an offline result computed from different arithmetic would not be
     * evidence about the live path. */
    float wet = params->wet_mix < 0.0f ? 0.0f : params->wet_mix;
    float dry = params->dry_mix > 1.0f ? 1.0f : params->dry_mix;
    float decay = params->decay_seconds;
    float roomsize = decay <= 0.0f ? 0.5f : decay / (decay + 1.2f);
    if (roomsize > 0.98f) roomsize = 0.98f;
    float damp = params->damping;
    if (params->lowpass_hz > 0.0f && params->lowpass_hz < 22050.0f) {
        float lp = 1.0f - (params->lowpass_hz / 22050.0f);
        if (lp > damp) damp = lp;
    }
    if (damp > 1.0f) damp = 1.0f;
    float predelay_ms = params->pre_delay_ms;
    if (predelay_ms <= 0.0f && params->room_size > 0.0f)
        predelay_ms = params->room_size / 0.34f;

    fv_set_params(&fv, wet, dry, roomsize, damp, params->diffusion,
                  predelay_ms, params->early_delay_ms, params->early_mix);

    /* De-interleave, process, re-interleave: fv_process_channel takes one
     * channel's contiguous samples, which is what the node path hands it. */
    float *sin_  = (float *)JCE_MALLOC((size_t)frames * sizeof(float));
    float *sout_ = (float *)JCE_MALLOC((size_t)frames * sizeof(float));
    if (!sin_ || !sout_) {
        JCE_FREE(sin_); JCE_FREE(sout_); fv_free(&fv);
        return false;
    }
    for (int c = 0; c < channels; ++c) {
        for (uint32_t s2 = 0; s2 < frames; ++s2)
            sin_[s2] = io[(size_t)s2 * (size_t)channels + (size_t)c];
        fv_process_channel(&fv, c, sin_, sout_, frames);
        for (uint32_t s2 = 0; s2 < frames; ++s2)
            io[(size_t)s2 * (size_t)channels + (size_t)c] = sout_[s2];
    }
    JCE_FREE(sin_);
    JCE_FREE(sout_);
    fv_free(&fv);
    return true;
}

void jce_audio_set_reverb(JceAudio *audio, const JceAudioReverbParams *params)
{
    if (!audio || !params) return;

    float wet = params->wet_mix;
    float dry = params->dry_mix;
    if (wet < 0.0f) wet = 0.0f;

    /* Lazily stand up the reverb node the first time a zone asks for wet > 0.
     * Until then the mix stays fully dry with zero overhead. */
    if (wet > 0.0001f && !audio->reverb_inited) {
        if (!audio_init_reverb(audio)) return;  /* stays dry on failure */
    }
    if (!audio->reverb_inited) return;

    /* Map the generic preset onto Freeverb tuning:
     *   roomsize  <- decay_seconds (longer tail => higher comb feedback)
     *   damp      <- damping, raised when lowpass_hz is low (dark rooms)   */
    float decay    = params->decay_seconds;
    float roomsize = decay <= 0.0f ? 0.5f
                   : decay / (decay + 1.2f);          /* 0..~0.98 asymptote */
    if (roomsize > 0.98f) roomsize = 0.98f;

    float damp = params->damping;
    if (params->lowpass_hz > 0.0f && params->lowpass_hz < 22050.0f) {
        float lp = 1.0f - (params->lowpass_hz / 22050.0f);  /* darker => more */
        if (lp > damp) damp = lp;
    }
    if (damp > 1.0f) damp = 1.0f;
    if (dry > 1.0f) dry = 1.0f;

    /* PRE-DELAY, from the field that carries it and the one the header always
     * said scaled it.  room_size is metres; sound covers ~34 cm per ms, so a
     * room's first surface answers after roughly size/0.34 ms -- used only
     * when the preset does not state a pre-delay itself, so an explicit value
     * always wins over the derived one. */
    float predelay_ms = params->pre_delay_ms;
    if (predelay_ms <= 0.0f && params->room_size > 0.0f)
        predelay_ms = params->room_size / 0.34f;

    /* EARLY REFLECTION.  Its own delay and level; a zero level costs one
     * branch per sample and is what every preset that does not ask for one
     * gets. */
    float early_ms   = params->early_delay_ms;
    float early_gain = params->early_mix;

    fv_set_params(&audio->reverb.fv, wet, dry, roomsize, damp,
                  params->diffusion, predelay_ms, early_ms, early_gain);
    audio->reverb_wet = wet;
}

JceSound jce_audio_load_memory(JceAudio *audio, const void *data,
                                uint32_t size, const char *hint_path)
{
    if (!audio || !data || size == 0) return JCE_SOUND_INVALID;
    /* Dedup by path: a scene firing the same one-shot every few seconds would
     * otherwise decode + allocate a fresh slot per call and exhaust the 64-slot
     * table (the editor-Play "cannot load" after ~a minute of play).  A clip
     * already resident replays from its shared slot. */
    if (hint_path && hint_path[0]) {
        int cached = find_sound_slot_by_path(audio, hint_path);
        if (cached >= 0) return (JceSound)(cached + 1);
    }
    JceAudioCpu *cpu = jce_audio_decode_cpu_memory(
        data, (size_t)size, hint_path ? hint_path : "<memory>");
    JceSound snd = jce_audio_upload_cpu(audio, cpu);
    if (snd != JCE_SOUND_INVALID && hint_path && hint_path[0])
        set_sound_slot_path(audio, (int)snd - 1, hint_path);
    return snd;
}

float jce_audio_get_duration(const JceAudio *audio, JceSound snd)
{
    if (!audio || snd == JCE_SOUND_INVALID) return 0.0f;
    int slot = (int)snd - 1;
    if (slot < 0 || slot >= JCE_MAX_SOUNDS || !audio->sound_used[slot]) return 0.0f;
    const SoundSlot *s = &audio->sounds[slot];
    if (s->sample_rate == 0) return 0.0f;
    return (float)s->frame_count / (float)s->sample_rate;
}

float jce_audio_get_time(const JceAudio *audio, JceVoice voice)
{
    if (!audio || voice == JCE_VOICE_INVALID) return 0.0f;
    int idx = resolve_voice(audio, voice);
    if (idx < 0 || idx >= JCE_MAX_VOICES || !audio->voices[idx].inited) return 0.0f;
    float cursor = 0.0f;
    ma_sound_get_cursor_in_seconds((ma_sound *)&audio->voices[idx].sound, &cursor);
    return cursor;
}

void jce_audio_seek(JceAudio *audio, JceVoice voice, float time_sec)
{
    if (!audio || voice == JCE_VOICE_INVALID) return;
    int idx = resolve_voice(audio, voice);
    if (idx < 0 || idx >= JCE_MAX_VOICES || !audio->voices[idx].inited) return;
    int slot = audio->voices[idx].sound_slot;
    if (slot < 0 || slot >= JCE_MAX_SOUNDS) return;
    ma_uint32 sr = audio->sounds[slot].sample_rate;
    if (sr == 0) return;
    ma_uint64 frame = (ma_uint64)(time_sec * (float)sr);
    if (frame > audio->sounds[slot].frame_count)
        frame = audio->sounds[slot].frame_count;
    ma_sound_seek_to_pcm_frame(&audio->voices[idx].sound, frame);
}

const int16_t *jce_audio_get_pcm_data(const JceAudio *audio, JceSound snd,
                                       uint32_t *out_frame_count,
                                       uint32_t *out_channels)
{
    if (!audio || snd == JCE_SOUND_INVALID) return NULL;
    int slot = (int)snd - 1;
    if (slot < 0 || slot >= JCE_MAX_SOUNDS || !audio->sound_used[slot]) return NULL;
    const SoundSlot *s = &audio->sounds[slot];
    if (!s->pcm_data) return NULL;
    if (out_frame_count) *out_frame_count = (uint32_t)s->frame_count;
    if (out_channels)    *out_channels    = s->channels;
    return (const int16_t *)s->pcm_data;
}

/* ── 3D positional audio ──────────────────────────────────────────── */

void jce_audio_set_listener(JceAudio *audio, const JceAudioListener *l)
{
    if (!audio || !l) return;
    /* miniaudio supports multiple listeners; we use index 0 only. */
    ma_engine_listener_set_position(&audio->engine, 0,
                                    l->position[0], l->position[1], l->position[2]);
    ma_engine_listener_set_direction(&audio->engine, 0,
                                     l->forward[0], l->forward[1], l->forward[2]);
    ma_engine_listener_set_world_up(&audio->engine, 0,
                                    l->up[0], l->up[1], l->up[2]);
    ma_engine_listener_set_velocity(&audio->engine, 0,
                                    l->velocity[0], l->velocity[1], l->velocity[2]);
}

void jce_audio_set_doppler_factor(JceAudio *audio, float factor)
{
    if (!audio) return;
    if (factor < 0.0f) factor = 0.0f;
    /* miniaudio's Doppler is per-sound; apply to every active voice. */
    for (int i = 0; i < JCE_MAX_VOICES; ++i) {
        if (audio->voices[i].inited)
            ma_sound_set_doppler_factor(&audio->voices[i].sound, factor);
    }
}

/* Splice the splitter in once: terminal -> splitter, splitter[0] -> the bus's
 * normal output.  Idempotent, and leaves the bus untouched on failure. */
static bool bus_split_ensure(JceAudio *audio, BusSlot *b)
{
    if (b->split_inited) return true;
    ma_engine *e = &audio->engine;
    ma_splitter_node_config cfg =
        ma_splitter_node_config_init(ma_engine_get_channels(e));
    /* One output for the bus's own path plus one per possible send. */
    cfg.outputBusCount = 1u + JCE_AUDIO_BUS_MAX_SENDS;
    if (ma_splitter_node_init(ma_engine_get_node_graph(e), &cfg, NULL,
                              &b->send_split) != MA_SUCCESS) {
        LOG_WARN("jce_audio", "aux send: splitter init failed for bus '%s'",
                 b->name);
        return false;
    }
    /* Order matters: attach the splitter's OWN output first, so the bus is
     * never left feeding a node that feeds nothing. */
    if (ma_node_attach_output_bus(&b->send_split.base, 0,
                                  audio_output_node(audio), 0) != MA_SUCCESS
        || ma_node_attach_output_bus(bus_terminal_node(b), 0,
                                     &b->send_split.base, 0) != MA_SUCCESS) {
        ma_splitter_node_uninit(&b->send_split, NULL);
        LOG_WARN("jce_audio", "aux send: splice failed for bus '%s'", b->name);
        return false;
    }
    /* Every send output starts silent and unassigned; set_send supplies both. */
    for (int k = 0; k < JCE_AUDIO_BUS_MAX_SENDS; ++k) {
        ma_node_set_output_bus_volume(&b->send_split.base,
                                      (ma_uint32)(k + 1), 0.0f);
        b->send_dest[k]   = -1;
        b->send_amount[k] = 0.0f;
    }
    b->split_inited = true;
    return true;
}

bool jce_audio_bus_set_send(JceAudio *audio, const char *from_bus,
                            const char *to_bus, float amount)
{
    if (!audio || !from_bus || !to_bus) return false;
    int from = audio_find_bus(audio, from_bus);
    int to   = audio_find_bus(audio, to_bus);
    /* A bus may not send to itself: the splitter's second output would feed
     * the node its own input comes from, which is a ring in the graph and
     * recurses with no bottom when frames are pulled.  jce_audio_mixer refuses
     * longer rings at the authoring layer; this is the device-side floor. */
    if (from < 0 || to < 0 || from == to) return false;
    if (amount < 0.0f) amount = 0.0f;

    BusSlot *b = &audio->buses[from];
    if (!bus_split_ensure(audio, b)) return false;

    /* Existing send to this destination wins its own slot back; otherwise the
     * first free one.  Looking for the destination FIRST is what makes a
     * repeated call re-scale instead of consuming a second slot. */
    int slot = -1;
    for (int k = 0; k < JCE_AUDIO_BUS_MAX_SENDS; ++k)
        if (b->send_dest[k] == to) { slot = k; break; }
    if (slot < 0)
        for (int k = 0; k < JCE_AUDIO_BUS_MAX_SENDS; ++k)
            if (b->send_dest[k] < 0) { slot = k; break; }
    if (slot < 0) {
        LOG_WARN("jce_audio", "aux send: bus '%s' already has %d sends",
                 from_bus, JCE_AUDIO_BUS_MAX_SENDS);
        return false;
    }

    if (amount <= 0.0f) {
        /* Retire: silence the output and free the slot.  The attachment is
         * left in place -- detaching and re-attaching a live graph edge buys
         * nothing when the gain is already zero. */
        ma_node_set_output_bus_volume(&b->send_split.base,
                                      (ma_uint32)(slot + 1), 0.0f);
        b->send_dest[slot]   = -1;
        b->send_amount[slot] = 0.0f;
        return true;
    }

    if (ma_node_attach_output_bus(&b->send_split.base, (ma_uint32)(slot + 1),
                                  (ma_node *)&audio->buses[to].group, 0)
        != MA_SUCCESS) {
        LOG_WARN("jce_audio", "aux send: '%s' -> '%s' attach failed",
                 from_bus, to_bus);
        return false;
    }
    ma_node_set_output_bus_volume(&b->send_split.base,
                                  (ma_uint32)(slot + 1), amount);
    b->send_dest[slot]   = to;
    b->send_amount[slot] = amount;
    return true;
}

float jce_audio_bus_get_send(const JceAudio *audio, const char *from_bus,
                             const char *to_bus)
{
    if (!audio || !from_bus || !to_bus) return 0.0f;
    int from = audio_find_bus((JceAudio *)audio, from_bus);
    int to   = audio_find_bus((JceAudio *)audio, to_bus);
    if (from < 0 || to < 0) return 0.0f;
    const BusSlot *b = &audio->buses[from];
    if (!b->split_inited) return 0.0f;
    for (int k = 0; k < JCE_AUDIO_BUS_MAX_SENDS; ++k)
        if (b->send_dest[k] == to) return b->send_amount[k];
    return 0.0f;
}

void jce_audio_voice_set_spatial_blend(JceAudio *audio, JceVoice voice,
                                       float blend)
{
    if (!audio || voice == JCE_VOICE_INVALID) return;
    int idx = resolve_voice(audio, voice);
    if (idx < 0 || idx >= JCE_MAX_VOICES || !audio->voices[idx].inited) return;

    ma_sound *snd = &audio->voices[idx].sound;
    if (!(blend > 0.0f)) {
        /* Fully 2D.  Identical to what set_3d(false) already did, so a source
         * authored at 0 mixes exactly as before. */
        ma_sound_set_spatialization_enabled(snd, MA_FALSE);
        ma_sound_set_min_gain(snd, 0.0f);
        return;
    }
    if (blend > 1.0f) blend = 1.0f;
    ma_sound_set_spatialization_enabled(snd, MA_TRUE);
    /* The floor under the spatializer's attenuation gain.  At blend 1 the
     * floor is 0 and distance attenuates all the way to silence, which is
     * full 3D; at blend 0.25 it is 0.75, so distance can only take a quarter
     * of the level.  This is a spatializer field, not the sound's volume --
     * ma_sound_set_min_gain forwards to ma_spatializer_set_min_gain -- so it
     * scales attenuation and leaves the authored volume alone. */
    ma_sound_set_min_gain(snd, 1.0f - blend);
}

void jce_audio_voice_set_3d(JceAudio *audio, JceVoice voice, bool spatial)
{
    if (!audio || voice == JCE_VOICE_INVALID) return;
    int idx = resolve_voice(audio, voice);
    if (idx < 0 || idx >= JCE_MAX_VOICES || !audio->voices[idx].inited) return;
    ma_sound_set_spatialization_enabled(&audio->voices[idx].sound,
                                         spatial ? MA_TRUE : MA_FALSE);
}

void jce_audio_voice_set_position(JceAudio *audio, JceVoice voice,
                                    float x, float y, float z)
{
    if (!audio || voice == JCE_VOICE_INVALID) return;
    int idx = resolve_voice(audio, voice);
    if (idx < 0 || idx >= JCE_MAX_VOICES || !audio->voices[idx].inited) return;
    ma_sound_set_position(&audio->voices[idx].sound, x, y, z);
}

void jce_audio_voice_set_velocity(JceAudio *audio, JceVoice voice,
                                    float vx, float vy, float vz)
{
    if (!audio || voice == JCE_VOICE_INVALID) return;
    int idx = resolve_voice(audio, voice);
    if (idx < 0 || idx >= JCE_MAX_VOICES || !audio->voices[idx].inited) return;
    ma_sound_set_velocity(&audio->voices[idx].sound, vx, vy, vz);
}

void jce_audio_voice_set_attenuation(JceAudio *audio, JceVoice voice,
                                       JceAudioAttenuation model,
                                       float min_distance, float max_distance,
                                       float rolloff)
{
    if (!audio || voice == JCE_VOICE_INVALID) return;
    int idx = resolve_voice(audio, voice);
    if (idx < 0 || idx >= JCE_MAX_VOICES || !audio->voices[idx].inited) return;

    ma_attenuation_model ma_model = ma_attenuation_model_inverse;
    switch (model) {
        case JCE_AUDIO_ATTEN_NONE:        ma_model = ma_attenuation_model_none;        break;
        case JCE_AUDIO_ATTEN_INVERSE:     ma_model = ma_attenuation_model_inverse;     break;
        case JCE_AUDIO_ATTEN_LINEAR:      ma_model = ma_attenuation_model_linear;      break;
        case JCE_AUDIO_ATTEN_EXPONENTIAL: ma_model = ma_attenuation_model_exponential; break;
    }
    ma_sound *snd = &audio->voices[idx].sound;
    ma_sound_set_attenuation_model(snd, ma_model);
    if (min_distance > 0.0f) ma_sound_set_min_distance(snd, min_distance);
    if (max_distance > 0.0f) ma_sound_set_max_distance(snd, max_distance);
    if (rolloff      > 0.0f) ma_sound_set_rolloff(snd, rolloff);
}

#else /* JCE_NO_AUDIO */

JceAudio *jce_audio_create(void) {
    LOG_WARN("jce_audio", "audio disabled (JCE_NO_AUDIO)");
    return NULL;
}
JceAudio *jce_audio_create_offline(void) {
    LOG_WARN("jce_audio", "audio disabled (JCE_NO_AUDIO)");
    return NULL;
}
void jce_audio_destroy(JceAudio *audio) { (void)audio; }
JceSound jce_audio_load(JceAudio *audio, JcePakArchive *pak, const char *path) {
    (void)audio; (void)pak; (void)path; return JCE_SOUND_INVALID;
}
JceSound jce_audio_load_pcm(JceAudio *audio, const void *pcm_data,
    uint32_t pcm_size, uint16_t channels, uint32_t sample_rate,
    uint16_t bits_per_sample) {
    (void)audio; (void)pcm_data; (void)pcm_size;
    (void)channels; (void)sample_rate; (void)bits_per_sample;
    return JCE_SOUND_INVALID;
}
JceSound jce_audio_load_streaming(JceAudio *audio, const JcePakArchive *pak,
                                  const char *path) {
    (void)audio; (void)pak; (void)path; return JCE_SOUND_INVALID;
}
void jce_audio_unload(JceAudio *audio, JceSound snd) { (void)audio; (void)snd; }
JceVoice jce_audio_play(JceAudio *audio, JceSound snd, bool loop, float volume, float pitch) {
    (void)audio; (void)snd; (void)loop; (void)volume; (void)pitch;
    return JCE_VOICE_INVALID;
}
/* The priority trio, stubbed HERE as well as in the real backend.  Nothing
 * compiles this branch -- no preset defines JCE_NO_AUDIO -- so a public symbol
 * declared unconditionally in jce_audio.h and defined only above would link
 * everywhere anyone builds and fail only for whoever first turns this on, with
 * no gate able to have told them.  This file already carries that defect for
 * four other symbols; it is not getting three more. */
JceVoice jce_audio_play_priority(JceAudio *audio, JceSound snd, bool loop,
    float volume, float pitch, int priority) {
    (void)audio; (void)snd; (void)loop; (void)volume; (void)pitch; (void)priority;
    return JCE_VOICE_INVALID;
}
void jce_audio_voice_set_priority(JceAudio *audio, JceVoice voice, int priority) {
    (void)audio; (void)voice; (void)priority;
}
void jce_audio_voice_set_spatial_blend(JceAudio *audio, JceVoice voice, float blend) {
    (void)audio; (void)voice; (void)blend;
}
bool jce_audio_bus_set_send(JceAudio *audio, const char *from_bus,
                            const char *to_bus, float amount) {
    (void)audio; (void)from_bus; (void)to_bus; (void)amount; return false;
}
float jce_audio_bus_get_send(const JceAudio *audio, const char *from_bus,
                             const char *to_bus) {
    (void)audio; (void)from_bus; (void)to_bus; return 0.0f;
}
int jce_audio_voice_get_priority(const JceAudio *audio, JceVoice voice) {
    (void)audio; (void)voice; return JCE_AUDIO_PRIORITY_NORMAL;
}
void jce_audio_stop(JceAudio *audio, JceVoice voice) { (void)audio; (void)voice; }
void jce_audio_pause(JceAudio *audio, JceVoice voice) { (void)audio; (void)voice; }
void jce_audio_resume(JceAudio *audio, JceVoice voice) { (void)audio; (void)voice; }
void jce_audio_set_volume(JceAudio *audio, JceVoice voice,
                          float volume)
{
    (void)audio; (void)voice; (void)volume;
}
void jce_audio_set_lowpass(JceAudio *audio, JceVoice voice,
                           float cutoff_hz)
{
    (void)audio; (void)voice; (void)cutoff_hz;
}
void jce_audio_set_pitch(JceAudio *audio, JceVoice voice,
                         float pitch)
{
    (void)audio; (void)voice; (void)pitch;
}
void jce_audio_set_looping(JceAudio *audio, JceVoice voice,
                           bool loop)
{
    (void)audio; (void)voice; (void)loop;
}
bool jce_audio_is_playing(const JceAudio *audio,
                          JceVoice voice)
{
    (void)audio; (void)voice;
    return false;
}
void jce_audio_set_master_volume(JceAudio *audio, float volume) { (void)audio; (void)volume; }
void jce_audio_stop_all(JceAudio *audio) { (void)audio; }
bool jce_audio_bus_create(JceAudio *audio, const char *name) {
    (void)audio; (void)name; return false;
}
void jce_audio_bus_set_volume(JceAudio *audio, const char *name, float volume) {
    (void)audio; (void)name; (void)volume;
}
void jce_audio_voice_set_bus(JceAudio *audio, JceVoice voice, const char *bus_name) {
    (void)audio; (void)voice; (void)bus_name;
}
int jce_audio_bus_add_effect(JceAudio *audio, const char *bus_name,
                             const JceAudioEffectDesc *desc) {
    (void)audio; (void)bus_name; (void)desc; return -1;
}
int jce_audio_voice_add_effect(JceAudio *audio, JceVoice voice,
                               const JceAudioEffectDesc *desc) {
    (void)audio; (void)voice; (void)desc; return -1;
}
bool jce_audio_bus_set_effect(JceAudio *audio, const char *bus_name,
                              uint32_t index, const JceAudioEffectDesc *desc) {
    (void)audio; (void)bus_name; (void)index; (void)desc; return false;
}
bool jce_audio_voice_set_effect(JceAudio *audio, JceVoice voice,
                                uint32_t index, const JceAudioEffectDesc *desc) {
    (void)audio; (void)voice; (void)index; (void)desc; return false;
}
bool jce_audio_bus_remove_effect(JceAudio *audio, const char *bus_name,
                                 uint32_t index) {
    (void)audio; (void)bus_name; (void)index; return false;
}
bool jce_audio_voice_remove_effect(JceAudio *audio, JceVoice voice,
                                   uint32_t index) {
    (void)audio; (void)voice; (void)index; return false;
}
float jce_audio_bus_get_peak(JceAudio *audio, const char *name) {
    (void)audio; (void)name; return 0.0f;
}
bool jce_audio_render_offline(JceAudio *audio, float *out, uint32_t frames) {
    (void)audio; (void)out; (void)frames; return false;
}
uint32_t jce_audio_bus_effect_count(JceAudio *audio, const char *bus_name) {
    (void)audio; (void)bus_name; return 0;
}
uint32_t jce_audio_voice_effect_count(JceAudio *audio, JceVoice voice) {
    (void)audio; (void)voice; return 0;
}
void jce_audio_set_reverb(JceAudio *audio, const JceAudioReverbParams *params) {
    (void)audio; (void)params;
}
bool jce_audio_reverb_process_offline(const JceAudioReverbParams *params,
                                      float *io, uint32_t frames, int channels,
                                      int sample_rate) {
    (void)params; (void)io; (void)frames; (void)channels; (void)sample_rate;
    return false;
}
JceSound jce_audio_load_memory(JceAudio *audio, const void *data,
    uint32_t size, const char *hint_path) {
    (void)audio; (void)data; (void)size; (void)hint_path;
    return JCE_SOUND_INVALID;
}
JceAudioCpu *jce_audio_decode_cpu_memory(const void *data, size_t size,
    const char *hint_path) {
    (void)data; (void)size; (void)hint_path; return NULL;
}
bool jce_audio_cpu_get_pcm(const JceAudioCpu *cpu,
    const void **out_pcm, uint32_t *out_pcm_bytes,
    uint16_t *out_channels, uint32_t *out_sample_rate,
    uint16_t *out_bits_per_sample) {
    (void)cpu;
    if (out_pcm)             *out_pcm             = NULL;
    if (out_pcm_bytes)       *out_pcm_bytes       = 0;
    if (out_channels)        *out_channels        = 0;
    if (out_sample_rate)     *out_sample_rate     = 0;
    if (out_bits_per_sample) *out_bits_per_sample = 0;
    return false;
}
float jce_audio_get_duration(const JceAudio *audio, JceSound snd) {
    (void)audio; (void)snd; return 0.0f;
}
float jce_audio_get_time(const JceAudio *audio, JceVoice voice) {
    (void)audio; (void)voice; return 0.0f;
}
void jce_audio_seek(JceAudio *audio, JceVoice voice, float time_sec) {
    (void)audio; (void)voice; (void)time_sec;
}
const int16_t *jce_audio_get_pcm_data(const JceAudio *audio, JceSound snd,
    uint32_t *out_frame_count, uint32_t *out_channels) {
    (void)audio; (void)snd;
    if (out_frame_count) *out_frame_count = 0;
    if (out_channels) *out_channels = 0;
    return NULL;
}
JceVoice jce_audio_play_stream(JceAudio *audio,
                                JceAudioStreamPullFn on_read, void *ud,
                                uint16_t channels, uint32_t sample_rate,
                                float volume, float pitch) {
    (void)audio; (void)on_read; (void)ud; (void)channels;
    (void)sample_rate; (void)volume; (void)pitch;
    return JCE_VOICE_INVALID;
}
void jce_audio_set_listener(JceAudio *audio, const JceAudioListener *l) {
    (void)audio; (void)l;
}
void jce_audio_set_doppler_factor(JceAudio *audio, float factor) {
    (void)audio; (void)factor;
}
void jce_audio_voice_set_3d(JceAudio *audio, JceVoice voice, bool spatial) {
    (void)audio; (void)voice; (void)spatial;
}
void jce_audio_voice_set_position(JceAudio *audio, JceVoice voice,
                                    float x, float y, float z) {
    (void)audio; (void)voice; (void)x; (void)y; (void)z;
}
void jce_audio_voice_set_velocity(JceAudio *audio, JceVoice voice,
                                    float vx, float vy, float vz) {
    (void)audio; (void)voice; (void)vx; (void)vy; (void)vz;
}
void jce_audio_voice_set_attenuation(JceAudio *audio, JceVoice voice,
                                       JceAudioAttenuation model,
                                       float min_distance, float max_distance,
                                       float rolloff) {
    (void)audio; (void)voice; (void)model;
    (void)min_distance; (void)max_distance; (void)rolloff;
}

#endif /* JCE_NO_AUDIO */
