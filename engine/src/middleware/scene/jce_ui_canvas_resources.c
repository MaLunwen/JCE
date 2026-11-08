/* See jce_ui_canvas_resources.h for what this is and why it is not in
 * jce_ui_canvas.c. */
#include "jce_ui_canvas_resources.h"

#include "jce_ui_canvas_widgets.h"   /* uc_get_font, for the fallback attach */

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_filesystem.h>   /* active-VFS tier (bundle Play) */
#include <jce/os/core/jce_i18n.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_str.h>
#include <jce/os/platform/jce_host_font.h>

#include <stdio.h>
#include <string.h>

#define LOG_TAG "ui_canvas"

/* ONE process-scoped object for how a canvas RESOLVES things, not four.
 *
 * The asset root, the default font, the fallback list and the generation that
 * invalidates the caches are one decision with one lifetime: every setter
 * bumps the same counter and every reader consults them together.  Four loose
 * statics were four entries in the duplication audit's global-state budget for
 * what a reader has to hold as a single idea anyway.
 *
 * Process-scoped rather than per-canvas because the shipped drop-in main sets
 * all of it while reading the project, BEFORE any canvas exists -- the same
 * reason jce_ui_canvas_set_default_font was process-scoped when it was
 * written.
 *
 * The editor points the asset root at the open project's source-assets dir, so
 * fonts referenced by UIText resolve from the project tree FIRST and then fall
 * back to the canvas pak; runtime pak-only boots (shipped exe, web) never set
 * it and keep the pak path.  The generation is what stops a stale font
 * outliving its project across a project switch. */
static struct {
    char     asset_root[512];
    char     default_font[256];
    /* Semicolon-separated; see jce_ui_canvas_set_font_fallbacks for what it
     * fixes and why it invalidates the same generation. */
    char     fallbacks[512];
    unsigned gen;
    unsigned system_font_gen;
    bool     system_fonts_resolved;
    char     system_latin[1024];
    char     system_cjk[1024];
    char     system_symbols[1024];
} s_uc;

void jce_ui_canvas_set_asset_root(const char *root_dir)
{
    const char *next = (root_dir && root_dir[0]) ? root_dir : "";
    if (strcmp(s_uc.asset_root, next) == 0) return;
    jce_strlcpy(s_uc.asset_root, next, sizeof s_uc.asset_root);
    s_uc.gen++;
}

void jce_ui_canvas_set_default_font(const char *font_path)
{
    const char *next = (font_path && font_path[0]) ? font_path : "";
    if (strcmp(s_uc.default_font, next) == 0) return;
    jce_strlcpy(s_uc.default_font, next, sizeof s_uc.default_font);
    s_uc.gen++;   /* invalidate font caches so the swap takes effect */
}

void jce_ui_canvas_set_font_fallbacks(const char *paths_semicolon)
{
    const char *next = (paths_semicolon && paths_semicolon[0])
                     ? paths_semicolon : "";
    if (strcmp(s_uc.fallbacks, next) == 0) return;
    jce_strlcpy(s_uc.fallbacks, next, sizeof s_uc.fallbacks);
    /* Same invalidation as the default font: a cached JceFont carries its
     * chain, so a font opened before the list changed would keep the old one
     * -- and the shape cache would keep serving runs itemized against it. */
    s_uc.gen++;
}

void jce_ui_canvas_invalidate_resources(void)
{
    ++s_uc.gen;
    LOG_DEBUG(LOG_TAG, "resources phase=invalidated generation=%u", s_uc.gen);
}

unsigned    uc_resource_generation(void)   { return s_uc.gen; }
const char *uc_resource_default_font(void) { return s_uc.default_font; }
const char *uc_resource_asset_root(void)   { return s_uc.asset_root; }

/* Open ONE font path through the three-tier ladder: project asset root on the
 * host FS, then the active VFS (bundle boots), then the canvas pak.
 *
 * Extracted from uc_get_font so the FALLBACK fonts go through exactly the same
 * ladder as the font they back.  A fallback resolved by a different rule is
 * the editor-vs-shipped divergence this tree keeps finding, one level down:
 * the primary would come from the project and its fallback from the editor's
 * own pak, and only one of the two builds would show it. */
/* "C:\..." / "\\server\share" on Windows, "/..." everywhere. */
static bool uc_path_is_absolute(const char *p)
{
    if (!p || !p[0]) return false;
    if (p[0] == '/' || p[0] == '\\') return true;
    return p[1] == ':' && (p[2] == '/' || p[2] == '\\');
}

/* System candidates are paths, not font owners. Resolve once per resource
 * generation; each Canvas retains its existing independent atlas/cache owner. */
static void uc_resolve_system_fonts(void)
{
    if (s_uc.system_fonts_resolved && s_uc.system_font_gen == s_uc.gen) return;
    jce_host_default_font(JCE_HOST_FONT_UI_LATIN, s_uc.system_latin,
                          sizeof s_uc.system_latin);
    jce_host_default_font(JCE_HOST_FONT_UI_CJK, s_uc.system_cjk,
                          sizeof s_uc.system_cjk);
    jce_host_default_font(JCE_HOST_FONT_UI_SYMBOLS, s_uc.system_symbols,
                          sizeof s_uc.system_symbols);
    s_uc.system_font_gen = s_uc.gen;
    s_uc.system_fonts_resolved = true;
}

bool uc_resource_system_ui_font(char *out, size_t out_size)
{
    if (!out || !out_size) return false;
    out[0] = '\0';
    uc_resolve_system_fonts();
    if (!s_uc.system_latin[0] || strlen(s_uc.system_latin) + 1 > out_size)
        return false;
    memcpy(out, s_uc.system_latin, strlen(s_uc.system_latin) + 1);
    return true;
}

static const char uc_stock_cjk_key[] = "@stock-cjk";
static const char uc_stock_cjk_path[] = "fonts/JCE-CJK-Fallback.ttf";

JceFont *uc_open_font_path(const JcePakArchive *pak, const char *p,
                           int px, bool sdf)
{
    /* Pre-render the engine's curated CJK/Latin-extended codepoints so UI
     * text matches what the editor / HUD already supports. */
    uint32_t cps[256];
    int n = jce_i18n_collect_codepoints(cps, 256);
    JceFont *f = NULL;

    /* Asked for AROUND the open and cleared straight after, because the
     * spread is process-scoped: leaving it set would silently make the
     * next unrelated font -- the editor debug overlay, a HUD -- a distance
     * field too, and that font would be drawn by the coverage shader.
     * 8px is FreeType own default and is comfortable for UI sizes: it is
     * how far outside the outline the field still carries a gradient, so
     * it bounds how far a glyph can be magnified before the edge runs out
     * of information. */
    if (sdf) jce_font_set_sdf_spread(8);

    /* The stock cache key is distinct from an authored asset path. Only
     * the Canvas PAK may provide the licensed fallback bytes. */
    if (strcmp(p, uc_stock_cjk_key) == 0) {
        if (pak) f = jce_font_open_ex(pak, uc_stock_cjk_path, (float)px, cps, n);
        if (sdf) jce_font_set_sdf_spread(0);
        return f;
    }

    /* An ABSOLUTE path is already a host path and must not be joined to the
     * asset root -- that is what a system UI font looks like, and joining it
     * would produce "<project>/C:/Windows/Fonts/segoeui.ttf", miss on all
     * three tiers, and fall back to the very font this bypasses. */
    if (uc_path_is_absolute(p)) {
        f = jce_font_open_file_ex(p, (float)px, cps, n);
    } else if (s_uc.asset_root[0]) {
        char full[768];
        snprintf(full, sizeof full, "%s/%s", s_uc.asset_root, p);
        f = jce_font_open_file_ex(full, (float)px, cps, n);
    }
    /* Active-VFS tier: in bundle Play (isolated preview) and shipped bundle
     * boots the font lives ONLY in the mounted bundle -- not on the host FS
     * (s_uc.asset_root unset) nor in the embedded pak (uc->pak).  Read it from
     * the active VFS the way meshes/textures load, then open from memory.
     * (The bundle stores keys case-folded; the VFS normalises the query, so a
     * mixed-case path like fonts/LXGWWenKai-Regular.ttf still resolves.) */
    if (!f) {
        JceFileSystem *afs = jce_fs_get_active();
        if (afs) {
            uint64_t vsz = 0;
            void *vbuf = jce_fs_read_all(afs, p, &vsz);
            if (vbuf) {
                if (vsz > 0)
                    f = jce_font_open_mem_ex(vbuf, (size_t)vsz, p,
                                             (float)px, cps, n);
                jce_fs_buffer_free(vbuf);
            }
        }
    }
    if (!f && pak)
        f = jce_font_open_ex(pak, p, (float)px, cps, n);

    if (sdf) jce_font_set_sdf_spread(0);
    return f;
}

/* Attach the configured fallbacks to a freshly opened font.
 *
 * The fallbacks land in the SAME cache table as ordinary fonts, so they are
 * closed by the same generation sweep and one CJK face is opened once however
 * many labels name it.  Closing order inside that sweep does not matter:
 * jce_font_close does not touch a fallback, and a primary closed after one of
 * its fallbacks never dereferences the pointer again.
 *
 * A fallback that fails to open is skipped and said so once: silently drawing
 * tofu is what this whole path exists to stop, so "the font you named is not
 * there" must not look like "your text has no glyphs". */
void uc_attach_fallbacks(JceUICanvas *uc, JceFont *f, const char *primary_path,
                         int px, bool sdf)
{
    if (!f) return;
    if (!s_uc.fallbacks[0]) {
        /* An explicit component/project font or fallback list keeps its own
         * policy. Only the ordinary system/stock default gains CJK/Symbols leaves. */
        if (s_uc.default_font[0] || !primary_path) return;
        uc_resolve_system_fonts();
        if (strcmp(primary_path, UC_DEFAULT_FONT) &&
            strcmp(primary_path, s_uc.system_latin)) return;
        bool cjk = false, symbols = false;
        if (s_uc.system_cjk[0]) {
            cjk = strcmp(primary_path, s_uc.system_cjk) == 0;
        }
        if (s_uc.system_cjk[0] && !cjk) {
            JceFont *fb = uc_get_font_leaf(uc, s_uc.system_cjk, px, sdf);
            cjk = fb && jce_font_add_fallback(f, fb);
            if (!cjk)
                LOG_WARN(LOG_TAG, "font phase=system_cjk_rejected primary=%s "
                         "path=%s px=%d generation=%u", primary_path,
                         s_uc.system_cjk, px, s_uc.gen);
        }
        if (s_uc.system_symbols[0]) {
            symbols = strcmp(primary_path, s_uc.system_symbols) == 0 ||
                (cjk && strcmp(s_uc.system_symbols, s_uc.system_cjk) == 0);
        }
        if (s_uc.system_symbols[0] && !symbols) {
            JceFont *fb = uc_get_font_leaf(uc, s_uc.system_symbols, px, sdf);
            symbols = fb && jce_font_add_fallback(f, fb);
            if (!symbols)
                LOG_WARN(LOG_TAG, "font phase=system_symbols_rejected primary=%s "
                         "path=%s px=%d generation=%u", primary_path,
                         s_uc.system_symbols, px, s_uc.gen);
        }
        if (!cjk || !symbols) {
            JceFont *fb = uc_get_font_leaf(uc, uc_stock_cjk_key, px, sdf);
            if (fb && jce_font_add_fallback(f, fb))
                LOG_INFO(LOG_TAG, "font phase=stock_fallback_ready owner=canvas_resources "
                         "path=%s px=%d generation=%u", uc_stock_cjk_path, px, s_uc.gen);
            else
                LOG_WARN(LOG_TAG, "font phase=stock_fallback_unavailable owner=canvas_resources "
                         "cjk=%u symbols=%u generation=%u", (unsigned)cjk,
                         (unsigned)symbols, s_uc.gen);
        }
        return;
    }

    const char *cur = s_uc.fallbacks;
    while (*cur) {
        const char *sep = strchr(cur, ';');
        size_t len = sep ? (size_t)(sep - cur) : strlen(cur);
        char one[256];
        if (len > 0 && len < sizeof one) {
            memcpy(one, cur, len);
            one[len] = '\0';
            /* Leaf cache entries never receive the global list themselves.
             * A primary may also be selected as a leaf elsewhere; those roles
             * own distinct objects so no later attachment creates a cycle. */
            JceFont *fb = primary_path && strcmp(primary_path, one) == 0
                ? f : uc_get_font_leaf(uc, one, px, sdf);
            if (fb && fb != f) {
                if (!jce_font_add_fallback(f, fb))
                    LOG_WARN(LOG_TAG, "font phase=fallback_rejected path=%s "
                             "px=%d reason=chain_contract", one, px);
            } else if (!fb) {
                LOG_WARN(LOG_TAG, "font fallback not found: %s @%dpx",
                         one, px);
            }
        }
        if (!sep) break;
        cur = sep + 1;
    }
}
