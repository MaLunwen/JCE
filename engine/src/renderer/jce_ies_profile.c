/*
 * jce_ies_profile.c  IES LM-63 photometric profile parser + LUT baker.
 *
 * Implements the TILT=NONE subset of the IESNA LM-63 (1986/1991/1995/2002)
 * text format which covers ~all manufacturer cookie files we have seen.
 *
 *   ── Format reminder (TILT=NONE) ──
 *     <free-form header lines / keywords up to and including>
 *     TILT=NONE
 *     <10 numbers, free-form whitespace separated>
 *         num_lamps  lumens_per_lamp  multiplier
 *         num_vertical_angles  num_horizontal_angles
 *         photometric_type    units_type
 *         width  length  height
 *     <ballast_factor  future_use  input_watts>           (3 numbers)
 *     <num_vertical_angles floats — vertical angles>
 *     <num_horizontal_angles floats — horizontal angles>
 *     <num_horizontal_angles * num_vertical_angles floats — candela values
 *      ordered horizontal-major (slice 0: all vert, slice 1: all vert, ...)>
 *
 *   The values returned by the parser are *normalised* (peak candela
 *   maps to 1.0).  Distance falloff is applied by the spot-light shader's
 *   inverse-square term, so we strip the absolute candela magnitude here.
 *
 * Layer: Renderer (L3). C99. No third-party deps.
 */

#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/renderer/jce_ies_profile.h>
#include <jce/renderer/jce_texture.h>

#include "os/core/jce_memory.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "jce_ies"

/* -- tiny tokenizer over a fixed text buffer ------------------------- */

typedef struct {
    const char *cur;
    const char *end;
} IesTok;

static bool ies_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',';
}

static void ies_skip_ws(IesTok *t)
{
    while (t->cur < t->end && ies_is_space(*t->cur)) t->cur++;
}

/* Read one whitespace-delimited token into out_buf (NUL terminated).
 * Returns false at EOF (no token). */
static bool ies_next_token(IesTok *t, char *out, size_t out_cap)
{
    ies_skip_ws(t);
    if (t->cur >= t->end) return false;
    size_t n = 0;
    while (t->cur < t->end && !ies_is_space(*t->cur)) {
        if (n + 1 < out_cap) out[n++] = *t->cur;
        t->cur++;
    }
    out[n] = '\0';
    return n > 0;
}

static bool ies_parse_float(IesTok *t, float *out)
{
    char buf[64];
    if (!ies_next_token(t, buf, sizeof(buf))) return false;
    char *endp = NULL;
    double v = strtod(buf, &endp);
    if (endp == buf) return false;
    *out = (float)v;
    return true;
}

static bool ies_parse_int(IesTok *t, int *out)
{
    char buf[64];
    if (!ies_next_token(t, buf, sizeof(buf))) return false;
    char *endp = NULL;
    long v = strtol(buf, &endp, 10);
    if (endp == buf) return false;
    *out = (int)v;
    return true;
}

/* Scan forward to (and consume) a line starting with the literal
 * "TILT=".  Returns the value just after '=' as 0-terminated buf.
 * Returns false if not found before EOF. */
static bool ies_seek_tilt(IesTok *t, char *out, size_t out_cap)
{
    while (t->cur < t->end) {
        /* Move to the next newline or token boundary. */
        const char *line_start = t->cur;
        const char *line_end = t->cur;
        while (line_end < t->end && *line_end != '\n') line_end++;

        /* Look for "TILT=" within the line. */
        size_t len = (size_t)(line_end - line_start);
        if (len >= 5) {
            for (size_t i = 0; i + 5 <= len; i++) {
                if ((line_start[i] == 'T' || line_start[i] == 't') &&
                    (line_start[i+1] == 'I' || line_start[i+1] == 'i') &&
                    (line_start[i+2] == 'L' || line_start[i+2] == 'l') &&
                    (line_start[i+3] == 'T' || line_start[i+3] == 't') &&
                    line_start[i+4] == '=')
                {
                    /* Copy value (up to whitespace / line end). */
                    const char *vp = line_start + i + 5;
                    size_t n = 0;
                    while (vp < line_end && !ies_is_space(*vp)) {
                        if (n + 1 < out_cap) out[n++] = *vp;
                        vp++;
                    }
                    out[n] = '\0';
                    t->cur = (line_end < t->end) ? (line_end + 1) : t->end;
                    return true;
                }
            }
        }
        t->cur = (line_end < t->end) ? (line_end + 1) : t->end;
    }
    return false;
}

/* Linear interpolation lookup in a sorted-ascending angle array. */
static float ies_lerp_at_angle(const float *angles, const float *values,
                               int n, float a)
{
    if (n <= 0) return 0.0f;
    if (a <= angles[0])     return values[0];
    if (a >= angles[n - 1]) return values[n - 1];
    /* Linear scan is fine for n <= ~256. */
    for (int i = 1; i < n; i++) {
        if (a <= angles[i]) {
            float a0 = angles[i - 1], a1 = angles[i];
            float v0 = values[i - 1], v1 = values[i];
            float t = (a1 - a0) > 1e-6f ? (a - a0) / (a1 - a0) : 0.0f;
            return v0 + (v1 - v0) * t;
        }
    }
    return values[n - 1];
}

bool jce_ies_parse(const char *text, size_t text_len,
                   JceIesProfile *out_meta, float *out_samples)
{
    if (out_meta) memset(out_meta, 0, sizeof(*out_meta));
    if (!text) return false;
    if (text_len == 0) text_len = strlen(text);
    if (text_len == 0) return false;

    IesTok t = { text, text + text_len };

    /* 1. Header — read until TILT=NONE. */
    char tilt[32];
    if (!ies_seek_tilt(&t, tilt, sizeof(tilt))) {
        LOG_WARN(LOG_TAG, "missing TILT= line");
        return false;
    }
    /* Case-insensitive compare for "NONE". */
    bool tilt_none = (tilt[0] == 'N' || tilt[0] == 'n') &&
                     (tilt[1] == 'O' || tilt[1] == 'o') &&
                     (tilt[2] == 'N' || tilt[2] == 'n') &&
                     (tilt[3] == 'E' || tilt[3] == 'e') &&
                     tilt[4] == '\0';
    if (!tilt_none) {
        LOG_WARN(LOG_TAG, "unsupported TILT=%s (only NONE is supported in v1)", tilt);
        return false;
    }

    /* 2. Ten leading numbers. */
    int num_lamps = 0, num_vert = 0, num_horiz = 0, photo_type = 0, units = 0;
    float lumens = 0, multiplier = 0, w = 0, l = 0, h = 0;
    if (!ies_parse_int(&t,   &num_lamps))   return false;
    if (!ies_parse_float(&t, &lumens))      return false;
    if (!ies_parse_float(&t, &multiplier))  return false;
    if (!ies_parse_int(&t,   &num_vert))    return false;
    if (!ies_parse_int(&t,   &num_horiz))   return false;
    if (!ies_parse_int(&t,   &photo_type))  return false;
    if (!ies_parse_int(&t,   &units))       return false;
    if (!ies_parse_float(&t, &w))           return false;
    if (!ies_parse_float(&t, &l))           return false;
    if (!ies_parse_float(&t, &h))           return false;

    if (num_vert <= 0 || num_vert > 4096 ||
        num_horiz <= 0 || num_horiz > 4096) {
        LOG_WARN(LOG_TAG, "implausible angle counts vert=%d horiz=%d",
                 num_vert, num_horiz);
        return false;
    }
    (void)num_lamps; (void)photo_type; (void)units;
    (void)w; (void)l; (void)h;

    /* 3. Three more numbers (ballast factor, future use, input watts). */
    float bf = 0, fut = 0, watts = 0;
    if (!ies_parse_float(&t, &bf))    return false;
    if (!ies_parse_float(&t, &fut))   return false;
    if (!ies_parse_float(&t, &watts)) return false;
    (void)bf; (void)fut; (void)watts;

    /* 4. Vertical angles. */
    float *vert = (float *)JCE_CALLOC((size_t)num_vert, sizeof(float));
    if (!vert) return false;
    for (int i = 0; i < num_vert; i++) {
        if (!ies_parse_float(&t, &vert[i])) { JCE_FREE(vert); return false; }
    }

    /* 5. Horizontal angles. */
    float *horiz = (float *)JCE_CALLOC((size_t)num_horiz, sizeof(float));
    if (!horiz) { JCE_FREE(vert); return false; }
    for (int i = 0; i < num_horiz; i++) {
        if (!ies_parse_float(&t, &horiz[i])) {
            JCE_FREE(vert); JCE_FREE(horiz); return false;
        }
    }

    /* 6. Candela grid: num_horiz slices of num_vert samples. */
    size_t grid_count = (size_t)num_horiz * (size_t)num_vert;
    float *grid = (float *)JCE_CALLOC(grid_count, sizeof(float));
    if (!grid) { JCE_FREE(vert); JCE_FREE(horiz); return false; }
    float max_cd = 0.0f;
    for (size_t i = 0; i < grid_count; i++) {
        if (!ies_parse_float(&t, &grid[i])) {
            JCE_FREE(vert); JCE_FREE(horiz); JCE_FREE(grid); return false;
        }
        grid[i] *= multiplier;
        if (grid[i] > max_cd) max_cd = grid[i];
    }

    if (out_meta) {
        out_meta->num_vertical   = (uint32_t)num_vert;
        out_meta->num_horizontal = (uint32_t)num_horiz;
        out_meta->max_candela    = max_cd;
    }

    /* 7. Bake LUT (256 samples across the full vertical range). */
    if (out_samples) {
        /* Pick the slice closest to the geometric centre.  For an axially
         * symmetric file (num_horiz==1) this is just slice 0. */
        int slice = num_horiz / 2;
        const float *slice_vals = &grid[(size_t)slice * (size_t)num_vert];

        float vmin = vert[0];
        float vmax = vert[num_vert - 1];
        if (vmax <= vmin) { vmax = vmin + 1.0f; }
        float inv_max = (max_cd > 1e-6f) ? (1.0f / max_cd) : 0.0f;

        for (uint32_t i = 0; i < JCE_IES_LUT_SIZE; i++) {
            float u = (float)i / (float)(JCE_IES_LUT_SIZE - 1);
            float a = vmin + u * (vmax - vmin);
            float cd = ies_lerp_at_angle(vert, slice_vals, num_vert, a);
            float n = cd * inv_max;
            if (n < 0.0f) n = 0.0f;
            if (n > 1.0f) n = 1.0f;
            out_samples[i] = n;
        }
    }

    JCE_FREE(vert); JCE_FREE(horiz); JCE_FREE(grid);
    return true;
}

/* -- LUT bake --------------------------------------------------------- */

static JceTexture ies_upload_lut(const float *samples)
{
    /* Pack 256 R8 samples into an RGBA8 256x1 texture for backend
     * portability (1-D textures are flaky on some bgfx targets; 256x1
     * 2D is universally supported and cost is identical). */
    uint8_t pixels[JCE_IES_LUT_SIZE * 4];
    for (uint32_t i = 0; i < JCE_IES_LUT_SIZE; i++) {
        float v = samples[i];
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        uint8_t b = (uint8_t)(v * 255.0f + 0.5f);
        pixels[i * 4 + 0] = b;
        pixels[i * 4 + 1] = b;
        pixels[i * 4 + 2] = b;
        pixels[i * 4 + 3] = 255;
    }
    return jce_texture_from_rgba(pixels, JCE_IES_LUT_SIZE, 1);
}

JceTexture jce_ies_bake_lut_from_memory(const char *text, size_t text_len)
{
    if (!text) return JCE_TEXTURE_INVALID;
    JceIesProfile meta;
    float samples[JCE_IES_LUT_SIZE];
    if (!jce_ies_parse(text, text_len, &meta, samples))
        return JCE_TEXTURE_INVALID;
    return ies_upload_lut(samples);
}

JceTexture jce_ies_bake_lut_from_file(const char *asset_path)
{
    if (!asset_path || !*asset_path) return JCE_TEXTURE_INVALID;

    uint64_t sz = 0;
    void *buf = NULL;
    JceFileSystem *fs = jce_fs_get_active();
    if (fs)
        buf = jce_fs_read_all(fs, asset_path, &sz);
    if (!buf || sz == 0) {
        /* Host-path fallback (used by editor / tools). */
        if (buf) jce_fs_buffer_free(buf);
        buf = jce_fs_host_read_all(asset_path, &sz);
    }
    if (!buf || sz == 0) {
        LOG_WARN(LOG_TAG, "cannot read IES file: %s", asset_path);
        if (buf) jce_fs_buffer_free(buf);
        return JCE_TEXTURE_INVALID;
    }

    JceTexture tex = jce_ies_bake_lut_from_memory((const char *)buf, (size_t)sz);
    jce_fs_buffer_free(buf);
    return tex;
}

void jce_ies_free_lut(JceTexture h)
{
    if (jce_texture_valid(h))
        jce_texture_destroy(h);
}
