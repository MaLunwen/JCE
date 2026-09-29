/* See jce_ui_canvas_resources.h for what this is and why it is not in
 * jce_ui_canvas.c. */
#include "jce_ui_canvas_resources.h"

#include "jce_ui_canvas_widgets.h"   /* uc_get_font, for the fallback attach */

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_filesystem.h>   /* active-VFS tier (bundle Play) */
#include <jce/os/core/jce_i18n.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_str.h>

#include <stdlib.h>   /* getenv, for the system font dir */
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

/* The system UI font, as an absolute host path.
 *
 * WHY THIS EXISTS.  UC_DEFAULT_FONT is "fonts/JCE.ttf", and that string is
 * what every UIText with an empty font_path resolved to.  JCE.ttf is a
 * hand-drawn glyph set that ships with the engine's sample content -- it is
 * artwork, not a UI face -- so the engine's silent default was a decorative
 * font, and every scene copied from a sample inherited it by name as well.
 * A default that is also the only obvious example becomes the answer
 * everybody gives.
 *
 * So an unspecified UI font now means "the one this machine uses for its own
 * UI", and JCE.ttf is only what you get when you ask for it.
 *
 * Deliberately a short, fixed list rather than a font-config dependency:
 * this runs in the engine, on every platform it ships to, and the failure
 * mode of guessing wrong is one more fallback step -- not a missing feature.
 * The names are the default UI face of each platform, newest first, then a
 * broadly-installed Latin face.  Returns false if none is readable, and the
 * caller then falls back exactly as it did before. */
static bool uc_system_ui_font(char *out, size_t out_size)
{
#if JCE_PLATFORM_WINDOWS
    const char *dir = getenv("WINDIR");
    static const char *names[] = { "segoeui.ttf", "tahoma.ttf",
                                   "arial.ttf", NULL };
    char probe[1024];
    int i;
    if (!dir || !dir[0]) dir = "C:\\Windows";
    for (i = 0; names[i]; ++i) {
        snprintf(probe, sizeof probe, "%s\\Fonts\\%s", dir, names[i]);
        if (jce_fs_host_exists_file(probe)) {
            jce_strlcpy(out, probe, out_size);
            return true;
        }
    }
#elif JCE_PLATFORM_APPLE
    static const char *paths[] = {
        "/System/Library/Fonts/SFNS.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
        "/Library/Fonts/Arial.ttf", NULL };
    int i;
    for (i = 0; paths[i]; ++i) {
        if (jce_fs_host_exists_file(paths[i])) {
            jce_strlcpy(out, paths[i], out_size);
            return true;
        }
    }
#else
    static const char *paths[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf", NULL };
    int i;
    for (i = 0; paths[i]; ++i) {
        if (jce_fs_host_exists_file(paths[i])) {
            jce_strlcpy(out, paths[i], out_size);
            return true;
        }
    }
#endif
    (void)out; (void)out_size;
    return false;
}

bool uc_resource_system_ui_font(char *out, size_t out_size)
{
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    return uc_system_ui_font(out, out_size);
}

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
void uc_attach_fallbacks(JceUICanvas *uc, JceFont *f, int px, bool sdf)
{
    if (!f || !s_uc.fallbacks[0]) return;

    const char *cur = s_uc.fallbacks;
    while (*cur) {
        const char *sep = strchr(cur, ';');
        size_t len = sep ? (size_t)(sep - cur) : strlen(cur);
        char one[256];
        if (len > 0 && len < sizeof one) {
            memcpy(one, cur, len);
            one[len] = '\0';
            JceFont *fb = uc_get_font(uc, one, px, sdf);
            if (fb && fb != f) {
                if (!jce_font_add_fallback(f, fb))
                    LOG_WARN(LOG_TAG, "font fallback chain full, dropping %s",
                             one);
            } else if (!fb) {
                LOG_WARN(LOG_TAG, "font fallback not found: %s @%dpx",
                         one, px);
            }
        }
        if (!sep) break;
        cur = sep + 1;
    }
}
