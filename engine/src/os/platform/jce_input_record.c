/*
 * jce_input_record.c  Input recorder / replayer implementation.
 *
 * Uses SDL_IOStream for file I/O (cross-platform via SDL3).
 * Format described in header.
 */

#include <jce/os/platform/jce_input_record.h>
#include <jce/os/core/jce_profiler.h>

#include "os/core/jce_memory.h"

#include <SDL3/SDL_iostream.h>
#include <string.h>

#define JIRC_MAGIC_0 'J'
#define JIRC_MAGIC_1 'I'
#define JIRC_MAGIC_2 'R'
#define JIRC_MAGIC_3 'C'
#define JIRC_HEADER_SIZE 16u

typedef enum { JIRC_MODE_RECORD, JIRC_MODE_REPLAY } JircMode;

struct JceInputRecorder {
    SDL_IOStream *io;
    JircMode      mode;
    uint64_t      frame_count;
};

static bool write_header(SDL_IOStream *io)
{
    uint8_t hdr[JIRC_HEADER_SIZE];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = JIRC_MAGIC_0;
    hdr[1] = JIRC_MAGIC_1;
    hdr[2] = JIRC_MAGIC_2;
    hdr[3] = JIRC_MAGIC_3;
    uint32_t v = JCE_INPUT_FRAME_VERSION;
    hdr[4] = (uint8_t)(v       & 0xFF);
    hdr[5] = (uint8_t)((v >> 8) & 0xFF);
    hdr[6] = (uint8_t)((v >> 16) & 0xFF);
    hdr[7] = (uint8_t)((v >> 24) & 0xFF);
    /* bytes 8..15 = pad zero */
    return SDL_WriteIO(io, hdr, JIRC_HEADER_SIZE) == JIRC_HEADER_SIZE;
}

static bool read_validate_header(SDL_IOStream *io)
{
    uint8_t hdr[JIRC_HEADER_SIZE];
    if (SDL_ReadIO(io, hdr, JIRC_HEADER_SIZE) != JIRC_HEADER_SIZE)
        return false;
    if (hdr[0] != JIRC_MAGIC_0 || hdr[1] != JIRC_MAGIC_1 ||
        hdr[2] != JIRC_MAGIC_2 || hdr[3] != JIRC_MAGIC_3)
        return false;
    uint32_t v = (uint32_t)hdr[4]
               | ((uint32_t)hdr[5] << 8)
               | ((uint32_t)hdr[6] << 16)
               | ((uint32_t)hdr[7] << 24);
    if (v != JCE_INPUT_FRAME_VERSION) return false;
    return true;
}

JceInputRecorder *jce_input_record_open(const char *path)
{
    if (!path) return NULL;
    SDL_IOStream *io = SDL_IOFromFile(path, "wb");
    if (!io) return NULL;
    if (!write_header(io)) { SDL_CloseIO(io); return NULL; }

    JceInputRecorder *r = (JceInputRecorder *)JCE_CALLOC(1, sizeof(*r));
    if (!r) { SDL_CloseIO(io); return NULL; }
    r->io   = io;
    r->mode = JIRC_MODE_RECORD;
    return r;
}

JceInputRecorder *jce_input_replay_open(const char *path)
{
    if (!path) return NULL;
    SDL_IOStream *io = SDL_IOFromFile(path, "rb");
    if (!io) return NULL;
    if (!read_validate_header(io)) { SDL_CloseIO(io); return NULL; }

    JceInputRecorder *r = (JceInputRecorder *)JCE_CALLOC(1, sizeof(*r));
    if (!r) { SDL_CloseIO(io); return NULL; }
    r->io   = io;
    r->mode = JIRC_MODE_REPLAY;
    return r;
}

void jce_input_record_close(JceInputRecorder *r)
{
    if (!r) return;
    if (r->io) SDL_CloseIO(r->io);
    JCE_FREE(r);
}

bool jce_input_record_tick(JceInputRecorder *r, const JceInput *input)
{
    if (!r || !input || r->mode != JIRC_MODE_RECORD) return false;
    JCE_PROFILE_ZONE_N("Input::RecordTick");
    JceInputFrame f;
    jce_input_capture(input, &f);
    if (SDL_WriteIO(r->io, &f, sizeof(f)) != sizeof(f)) { JCE_PROFILE_ZONE_END; return false; }
    r->frame_count++;
    /* Bug-repro robustness: flush periodically so a crash mid-session
     * (the very thing being recorded) still leaves a usable .jirc on
     * disk instead of losing the tail in stdio buffers. */
    if ((r->frame_count & 63u) == 0u)
        SDL_FlushIO(r->io);
    JCE_PROFILE_ZONE_END;
    return true;
}

bool jce_input_replay_tick(JceInputRecorder *r, JceInput *input)
{
    if (!r || !input || r->mode != JIRC_MODE_REPLAY) return false;
    JCE_PROFILE_ZONE_N("Input::ReplayTick");
    JceInputFrame f;
    if (SDL_ReadIO(r->io, &f, sizeof(f)) != sizeof(f)) { JCE_PROFILE_ZONE_END; return false; }
    if (!jce_input_apply(input, &f)) { JCE_PROFILE_ZONE_END; return false; }
    r->frame_count++;
    JCE_PROFILE_ZONE_END;
    return true;
}

uint64_t jce_input_record_frame_count(const JceInputRecorder *r)
{
    return r ? r->frame_count : 0;
}
