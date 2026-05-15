/*
 * jce_sdf_font.c  SDF font glyph table + UTF-8 layout.
 *
 * UTF-8 decoder is the standard one-shot variant (no validation
 * beyond ASCII / 2-/3-/4-byte recognition).  Layout pen advances
 * horizontally; newline resets x and advances y by line-height.
 */

#include <jce/renderer/jce_sdf_font.h>

#include <string.h>

void jce_sdf_font_init(JceSdfFont *f, const char *name,
                        const char *atlas_path,
                        uint16_t aw, uint16_t ah)
{
    if (!f) return;
    memset(f, 0, sizeof(*f));
    if (name)       strncpy(f->name, name, sizeof(f->name) - 1);
    if (atlas_path) strncpy(f->atlas_path, atlas_path, sizeof(f->atlas_path) - 1);
    f->atlas_w        = aw;
    f->atlas_h        = ah;
    f->sdf_pixel_range = 4.0f;
}

bool jce_sdf_font_add_glyph(JceSdfFont *f, const JceSdfGlyph *g)
{
    if (!f || !g) return false;
    /* Replace existing glyph with same codepoint. */
    for (uint32_t i = 0; i < f->glyph_count; ++i) {
        if (f->glyphs[i].active && f->glyphs[i].codepoint == g->codepoint) {
            f->glyphs[i] = *g;
            f->glyphs[i].active = true;
            return true;
        }
    }
    if (f->glyph_count >= JCE_SDF_FONT_MAX_GLYPHS) return false;
    f->glyphs[f->glyph_count] = *g;
    f->glyphs[f->glyph_count].active = true;
    f->glyph_count++;
    return true;
}

const JceSdfGlyph *jce_sdf_font_find(const JceSdfFont *f, uint32_t cp)
{
    if (!f) return NULL;
    for (uint32_t i = 0; i < f->glyph_count; ++i) {
        if (f->glyphs[i].active && f->glyphs[i].codepoint == cp)
            return &f->glyphs[i];
    }
    return NULL;
}

/* Decode one UTF-8 code point starting at *p.  Advances *p past the
 * consumed bytes.  Returns 0 on end-of-string. */
static uint32_t utf8_next(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;
    if (!s[0]) return 0;
    uint32_t cp = 0;
    int n = 0;
    if      (s[0] < 0x80) { cp = s[0];          n = 1; }
    else if ((s[0] & 0xE0) == 0xC0) { cp = s[0] & 0x1F; n = 2; }
    else if ((s[0] & 0xF0) == 0xE0) { cp = s[0] & 0x0F; n = 3; }
    else if ((s[0] & 0xF8) == 0xF0) { cp = s[0] & 0x07; n = 4; }
    else                            { cp = '?';        n = 1; }
    for (int i = 1; i < n; ++i) {
        if ((s[i] & 0xC0) != 0x80) { cp = '?'; n = i; break; }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    *p += n;
    return cp;
}

static float line_height(const JceSdfFont *f)
{
    return (float)(f->ascent - f->descent + f->line_gap);
}

uint32_t jce_sdf_layout_utf8(const JceSdfFont *f, const char *text,
                               float ox, float oy, float pt,
                               float *out_pos, float *out_uv,
                               uint32_t cap)
{
    if (!f || !text || !out_pos || !out_uv || cap == 0 || f->em_size == 0)
        return 0;
    float scale = pt / (float)f->em_size;
    float pen_x = ox;
    float pen_y = oy;
    const char *p = text;
    uint32_t emitted = 0;
    while (emitted < cap) {
        uint32_t cp = utf8_next(&p);
        if (cp == 0) break;
        if (cp == '\n') {
            pen_x = ox;
            pen_y += line_height(f) * scale;
            continue;
        }
        const JceSdfGlyph *g = jce_sdf_font_find(f, cp);
        if (!g) continue;

        float qx = pen_x + (float)g->bearing_x * scale;
        float qy = pen_y - (float)g->bearing_y * scale;
        float qw = (float)g->width  * scale;
        float qh = (float)g->height * scale;

        float *pos = out_pos + emitted * 8;
        float *uv  = out_uv  + emitted * 8;
        pos[0] = qx;        pos[1] = qy;
        pos[2] = qx + qw;   pos[3] = qy;
        pos[4] = qx + qw;   pos[5] = qy + qh;
        pos[6] = qx;        pos[7] = qy + qh;
        float u0 = (float)g->atlas_x / (float)f->atlas_w;
        float v0 = (float)g->atlas_y / (float)f->atlas_h;
        float u1 = (float)(g->atlas_x + g->width)  / (float)f->atlas_w;
        float v1 = (float)(g->atlas_y + g->height) / (float)f->atlas_h;
        uv[0] = u0; uv[1] = v0;
        uv[2] = u1; uv[3] = v0;
        uv[4] = u1; uv[5] = v1;
        uv[6] = u0; uv[7] = v1;

        pen_x += (float)g->advance * scale;
        emitted++;
    }
    return emitted;
}

void jce_sdf_measure_utf8(const JceSdfFont *f, const char *text,
                            float pt, float *ow, float *oh)
{
    if (!f || !text || f->em_size == 0) {
        if (ow) *ow = 0;
        if (oh) *oh = 0;
        return;
    }
    float scale = pt / (float)f->em_size;
    float max_w = 0.0f;
    float cur_w = 0.0f;
    float h = line_height(f) * scale;
    const char *p = text;
    while (1) {
        uint32_t cp = utf8_next(&p);
        if (cp == 0) break;
        if (cp == '\n') {
            if (cur_w > max_w) max_w = cur_w;
            cur_w = 0;
            h += line_height(f) * scale;
            continue;
        }
        const JceSdfGlyph *g = jce_sdf_font_find(f, cp);
        if (!g) continue;
        cur_w += (float)g->advance * scale;
    }
    if (cur_w > max_w) max_w = cur_w;
    if (ow) *ow = max_w;
    if (oh) *oh = h;
}
