/*
 * jce_view_bands.c  Runtime overlap guard for bgfx view-id bands.
 * See jce_view_bands.h for why this exists.
 */

#include "renderer/jce_view_bands.h"

#include <jce/os/core/jce_log.h>

#include <stdlib.h>
#include <string.h>

#define LOG_TAG "jce_views"

/* A frame renders the scene several times (editor viewport, game view, the
 * material-graph preview, asset thumbnails), each claiming its own bands, plus
 * postfx and the editor's absolute ids. 64 leaves headroom; overflow degrades
 * to "stop checking", never to a false report. */
#define JCE_VIEW_BAND_MAX 64u

typedef struct {
    const char *owner;
    uint16_t    first;
    uint16_t    count;
} JceViewBandClaim;

static JceViewBandClaim s_claims[JCE_VIEW_BAND_MAX];
static uint32_t         s_claim_count;
static uint32_t         s_conflicts;
static bool             s_overflowed;

bool jce_view_bands_enabled(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("JCE_VIEW_BAND_CHECK");
        if (e && e[0]) {
            v = (e[0] != '0') ? 1 : 0;
        } else {
#if defined(JCE_BUILD_VARIANT_DIST)
            v = 0;   /* ship build: the guard is a development tool */
#else
            v = 1;
#endif
        }
    }
    return v != 0;
}

void jce_view_bands_begin_frame(void)
{
    s_claim_count = 0u;
    s_conflicts   = 0u;
    s_overflowed  = false;
}

uint32_t jce_view_bands_conflict_count(void)
{
    return s_conflicts;
}

bool jce_view_bands_claim(const char *owner,
                                   uint16_t first, uint16_t count)
{
    uint32_t i;
    uint32_t last;
    bool ok = true;

    if (!jce_view_bands_enabled() || !owner || count == 0u) {
        return true;
    }
    /* Saturate rather than wrap: a band that runs past 65535 is a bug of its
     * own, but it must not be reported as overlapping everything below it. */
    last = (uint32_t)first + (uint32_t)count - 1u;
    if (last > 0xFFFFu) {
        last = 0xFFFFu;
    }

    for (i = 0u; i < s_claim_count; ++i) {
        const JceViewBandClaim *c = &s_claims[i];
        const uint32_t c_last = (uint32_t)c->first + (uint32_t)c->count - 1u;
        if (last < c->first || (uint32_t)first > c_last) {
            continue;                     /* disjoint */
        }
        if (c->owner == owner || strcmp(c->owner, owner) == 0) {
            continue;                     /* same subsystem, re-entrant render */
        }
        {
            const uint32_t lo = (first > c->first) ? first : c->first;
            const uint32_t hi = (last  < c_last)   ? last  : c_last;
            ++s_conflicts;
            LOG_ERROR(LOG_TAG,
                "view-band overlap: '%s' [%u..%u] collides with '%s' [%u..%u] "
                "on views %u..%u. bgfx view state is last-write-wins, so one "
                "of these two will silently stop rendering. Move one band; "
                "view ORDER cannot fix an ownership conflict.",
                owner, (unsigned)first, (unsigned)last,
                c->owner, (unsigned)c->first, (unsigned)c_last,
                (unsigned)lo, (unsigned)hi);
        }
        ok = false;
    }

    if (s_claim_count < JCE_VIEW_BAND_MAX) {
        s_claims[s_claim_count].owner = owner;
        s_claims[s_claim_count].first = first;
        s_claims[s_claim_count].count = count;
        ++s_claim_count;
    } else if (!s_overflowed) {
        s_overflowed = true;
        LOG_WARN(LOG_TAG,
                 "view-band table full (%u claims) — further bands this frame "
                 "are unchecked.", (unsigned)JCE_VIEW_BAND_MAX);
    }
    return ok;
}
