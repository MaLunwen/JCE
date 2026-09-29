/*
 * jce_bundle_pack_cook.c -- see jce_bundle_pack_cook.h for why this is its
 * own translation unit.
 */
#include "jce_bundle_pack_cook.h"

#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_str.h>        /* jce_strcasecmp */
#include "jce_asset_cooker.h"
#include "jce_bundle_contract_internal.h"
#include "jce_cook_policy.h"

#include <xxhash.h>
/* cgltf is the engine's single glTF authority -- used here ONLY to probe an
 * already-authored .gltf/.glb before handing it to the assimp-backed mesh
 * converter (see the COOK_CLASS_MODEL branch below). */
#include <cgltf.h>

/* Mesh->GLB converter (jce_bundle_mesh_convert.cpp, same layer). */
JCE_API int jce_bundle_convert_to_glb(const uint8_t *src, size_t src_sz,
                                      const char *ext_hint,
                                      uint8_t **out, size_t *out_sz);
#include <jce/resource/jce_asset_format.h>
#include <jce/os/core/jce_json.h>

#include "os/core/jce_memory.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* Map an import.json target_format string to a JCEASSET_TEXFMT_* value.
 * Returns -1 for "auto"/unknown so the caller falls back to platform auto. */
static int cook_parse_texfmt(const char *s)
{
    if (!s || !s[0]) return -1;
    if (strcmp(s, "rgba8") == 0) return JCEASSET_TEXFMT_RGBA8;
    if (strcmp(s, "bc1")   == 0) return JCEASSET_TEXFMT_BC1;
    if (strcmp(s, "bc3")   == 0) return JCEASSET_TEXFMT_BC3;
    if (strcmp(s, "bc5")   == 0) return JCEASSET_TEXFMT_BC5;
    if (strcmp(s, "bc7")   == 0) return JCEASSET_TEXFMT_BC7;
    if (strcmp(s, "astc")  == 0 || strcmp(s, "astc_4x4") == 0)
        return JCEASSET_TEXFMT_ASTC_4x4;
    return -1;
}

/* Texture-format policy (normal-map heuristic + per-platform auto format)
 * lives in jce_cook_policy.h — shared with jce_asset_cooker.c so the two
 * cook paths can never drift apart. */

/* Read & parse a sibling "<vpath>.import.json" preset, if present. Returns a
 * cJSON root the caller must cJSON_Delete, or NULL when absent/unparseable. */
static cJSON *cook_read_import_json(const char *vpath,
                                    const char *resource_root,
                                    PackResolveFn resolve_fn,
                                    void *resolve_user,
                                    const ExternalMap *emap)
{
    char sp[1408];
    snprintf(sp, sizeof(sp), "%s.import.json", vpath);
    size_t sz = 0;
    uint8_t *buf = read_asset(sp, resource_root, resolve_fn, resolve_user,
                              emap, &sz);
    if (!buf) return NULL;
    cJSON *root = cJSON_ParseWithLength((const char *)buf, sz);
    JCE_FREE(buf);
    return root;
}

/* Would round-tripping this glTF/GLB blob through the assimp mesh converter
 * destroy data cgltf would otherwise have loaded at runtime?
 *
 * True for anything the converter's two writers cannot express: both the
 * indexed-glb writer and assimp's glb2 exporter fallback emit plain
 * POSITION/NORMAL/UV0 primitives, so skins, animation clips and morph targets
 * do not survive the trip.  Detection runs on cgltf so the answer comes from
 * the same parser that owns the format at runtime; cgltf_parse reads only the
 * JSON header (plus the GLB chunk table) — no buffer load, no image decode —
 * which keeps this cheap enough for the parallel cook.  Reentrant: no globals,
 * allocations go through cgltf's default malloc/free.
 *
 * A parse failure returns false so a malformed/unusual input keeps the exact
 * pre-existing behaviour (hand it to assimp and let that path report). */
static bool gltf_assimp_roundtrip_is_lossy(const uint8_t *raw, size_t raw_size)
{
    cgltf_options options;
    cgltf_data   *data = NULL;
    memset(&options, 0, sizeof options);
    if (cgltf_parse(&options, raw, (cgltf_size)raw_size, &data)
            != cgltf_result_success || !data)
        return false;

    bool lossy = (data->skins_count > 0) || (data->animations_count > 0);
    for (cgltf_size m = 0; !lossy && m < data->meshes_count; ++m) {
        const cgltf_mesh *mesh = &data->meshes[m];
        if (mesh->weights_count > 0) { lossy = true; break; }
        for (cgltf_size p = 0; p < mesh->primitives_count; ++p) {
            if (mesh->primitives[p].targets_count > 0) { lossy = true; break; }
        }
    }

    cgltf_free(data);
    return lossy;
}

/* Cook one gathered asset.  On success frees `raw` and returns a freshly
 * JCE_MALLOC'd cooked buffer (out_size set), keeping the original vpath
 * (runtime loaders content-sniff).  On any non-cook / failure case returns
 * `raw` unchanged so the asset still ships uncooked. */
uint8_t *jce_bundle_cook_asset(const char *vpath, uint8_t *raw, size_t raw_size,
                           const char *resource_root,
                           PackResolveFn resolve_fn, void *resolve_user,
                           const ExternalMap *emap, int target_platform,
                           size_t *out_size, CookStatus *st)
{
    if (out_size) *out_size = raw_size;
    if (st) { st->failed = false; st->err[0] = '\0'; }
    if (!raw || raw_size == 0) return raw;

    CookClass cls = jce_bundle_classify_cook(vpath);
    if (cls == COOK_CLASS_NONE) return raw;

    /* LUT strip PNGs must ship as raw PNG bytes — jce_texture_load_lut_3d
     * calls jce_texture_decode_cpu which decodes the PNG directly; a
     * .jceasset wrapper (even RGBA8) is opaque to that loader.  Any block
     * compression (BC3/ASTC) is also lossy-corrupted for a precision LUT.
     * Return the raw bytes untouched so the PAK contains a plain PNG. */
    if (cls == COOK_CLASS_TEXTURE && jce_cook_path_is_lut(vpath)) {
        return raw;   /* passthrough — keep exact PNG bytes */
    }

    cJSON *imp = cook_read_import_json(vpath, resource_root, resolve_fn,
                                       resolve_user, emap);

    if (cls == COOK_CLASS_TEXTURE) {
        JceCookOptions opt = JCE_COOK_DEFAULT;
        opt.platform         = (JceCookPlatform)target_platform;
        opt.generate_mipmaps = true;
        opt.texture_format   = jce_cook_auto_texture_format(vpath,
                                                            target_platform);
        opt.max_texture_size = 0;
        /* Build-bundles iteration favours speed: range-fit block encode is
         * ~5-7x faster than the cluster-fit default at a modest quality cost.
         * An .import.json "quality" overrides per texture for hero/UI art. */
        opt.encode_quality   = JCE_COOK_ENCODE_FAST;
        /* Colour space for mip averaging.  Positive signal only -- see
         * jce_cook_path_is_srgb; unknown stays false, which is the behaviour
         * every bundle built before this had. */
        opt.texture_srgb     = jce_cook_path_is_srgb(vpath);
        if (imp) {
            const cJSON *tf = cJSON_GetObjectItemCaseSensitive(imp, "target_format");
            const cJSON *gm = cJSON_GetObjectItemCaseSensitive(imp, "gen_mips");
            const cJSON *ms = cJSON_GetObjectItemCaseSensitive(imp, "max_size");
            const cJSON *q  = cJSON_GetObjectItemCaseSensitive(imp, "quality");
            if (cJSON_IsString(tf)) {
                int f = cook_parse_texfmt(tf->valuestring);
                if (f >= 0) opt.texture_format = f;   /* -1 => keep auto */
            }
            if (cJSON_IsBool(gm)) opt.generate_mipmaps = cJSON_IsTrue(gm);
            if (cJSON_IsNumber(ms) && ms->valuedouble > 0)
                opt.max_texture_size = (int)ms->valuedouble;
            if (cJSON_IsString(q)) {
                if (jce_strcasecmp(q->valuestring, "fast") == 0)
                    opt.encode_quality = JCE_COOK_ENCODE_FAST;
                else if (jce_strcasecmp(q->valuestring, "high") == 0 ||
                         jce_strcasecmp(q->valuestring, "default") == 0)
                    opt.encode_quality = JCE_COOK_ENCODE_DEFAULT;
                else if (jce_strcasecmp(q->valuestring, "highest") == 0)
                    opt.encode_quality = JCE_COOK_ENCODE_HIGHEST;
            }

            /* "colorSpace": "srgb" | "linear" -- the KNOWN answer, and it
             * outranks the filename guess above because it was written by
             * code that actually knew: the model importer emits it beside
             * every texture it extracts from a GLB, tagged with the material
             * slot the texture filled.
             *
             * A NEW key on purpose.  The sidecar already has an "srgb"
             * boolean, written by the editor's import-preset panel and read
             * by nobody, and it defaults to TRUE -- so every sidecar already
             * on disk claims sRGB, including the ones sitting next to normal
             * maps.  Starting to honour that key would corrupt exactly the
             * data this change is trying to protect.  "colorSpace" has no
             * legacy writers, so reading it is safe by construction. */
            const cJSON *cs = cJSON_GetObjectItemCaseSensitive(imp, "colorSpace");
            if (cJSON_IsString(cs))
                (void)jce_cook_colour_space_parse(cs->valuestring,
                                                  &opt.texture_srgb);
        }
        JceCookResult r = jce_cook_texture(raw, raw_size, &opt);
        if (imp) cJSON_Delete(imp);
        if (!r.success || !r.data) {
            if (st) { st->failed = true;
                snprintf(st->err, sizeof(st->err), "%s",
                         r.error[0] ? r.error : "unknown"); }
            jce_cook_result_free(&r);
            return raw;
        }
        JCE_FREE(raw);
        if (out_size) *out_size = r.size;
        return (uint8_t *)r.data;   /* JCE_MALLOC'd by the cooker */
    }

    if (cls == COOK_CLASS_AUDIO) {
        JceCookOptions opt = JCE_COOK_DEFAULT;
        opt.platform = (JceCookPlatform)target_platform;
        /* No sidecar here: the bundle packer cooks from a blob it
         * already holds, with no asset path to look beside. */
        JceCookResult r = jce_cook_audio(raw, raw_size, &opt, NULL);
        if (imp) cJSON_Delete(imp);
        if (!r.success || !r.data) {
            if (st) { st->failed = true;
                snprintf(st->err, sizeof(st->err), "%s",
                         r.error[0] ? r.error : "unknown"); }
            jce_cook_result_free(&r);
            return raw;
        }
        /* COOKING AUDIO IS A TRADE, AND IT MUST NOT BE A LOSING ONE.
         *
         * jce_cook_audio decodes to raw s16 PCM: for a .wav that is a small
         * win (it drops container overhead -- measured 491,394 -> 349,109 on
         * the one bundled clip in the dogfood project, 0.7x), and for a .ogg,
         * .mp3 or .opus it is a ~10x LOSS.  Three minutes of 44.1 kHz stereo
         * is 31 MB of PCM whatever it was encoded from, and PCM does not
         * compress its way back: the pak stores already-compressed audio with
         * compression NONE precisely because there is nothing left to take.
         *
         * The main asset pak has always shipped those sources verbatim.  Only
         * this path decoded them, so the SAME FILE shipped encoded through one
         * route and decoded through the other, and nothing said so.
         *
         * The runtime needs no change either way: jce_audio_decode_cpu_memory
         * already branches on jce_asset_is_cooked and decodes encoded sources
         * from the PAK.  So when the cook inflates, keep the source. */
        if (r.size > raw_size) {
            pack_log_info("audio: %s kept encoded (%zu B; cooking to PCM "
                          "would be %zu B, %.1fx)",
                          vpath ? vpath : "(asset)", raw_size, r.size,
                          raw_size ? (double)r.size / (double)raw_size : 0.0);
            jce_cook_result_free(&r);
            return raw;                 /* out_size unchanged: source bytes */
        }
        JCE_FREE(raw);
        if (out_size) *out_size = r.size;
        return (uint8_t *)r.data;
    }

    /* COOK_CLASS_MODEL — convert any Assimp-readable mesh to GLB (+meshopt).
     * Already-GLB inputs are re-run through the converter so the meshopt
     * dedup/vertex-cache pass still applies — EXCEPT the rigged/morphed ones
     * the round-trip would mangle (see the cgltf guard below); if conversion
     * fails we ship the source bytes verbatim (a .gltf/.glb still loads at
     * runtime). */
    if (imp) cJSON_Delete(imp);   /* model import.json (scale/normals) is
                                     honoured by the editor importer, not the
                                     bundle-time GLB converter; consult here
                                     only to detect presence. */
    {
        const char *dot = strrchr(vpath, '.');
        const char *ext_hint = dot ? dot + 1 : "";

        /* glTF dual-authority guard: an already-authored .gltf/.glb is cgltf's
         * asset, and the converter above is an ASSIMP path.  Re-importing it
         * only to re-export it is a lossy round-trip — both the indexed-glb
         * writer and assimp's glb2 exporter emit POSITION/NORMAL/UV0 geometry
         * only, so a rigged character silently comes out of the bundle with no
         * skeleton, no animations and no morph targets.  Probe the source with
         * cgltf (the engine's single glTF authority; header parse only — no
         * buffer load, no image decode) and ship those inputs verbatim.  Static
         * glTF still goes through the converter, so the meshopt dedup +
         * auto-LOD + auto-meshlet cook is unchanged for the common case. */
        if ((ends_with_ci(vpath, ".glb") || ends_with_ci(vpath, ".gltf")) &&
            gltf_assimp_roundtrip_is_lossy(raw, raw_size))
            return raw;

        uint8_t *glb = NULL;
        size_t   glb_sz = 0;
        if (jce_bundle_convert_to_glb(raw, raw_size, ext_hint, &glb, &glb_sz)
            && glb && glb_sz > 0) {
            JCE_FREE(raw);
            if (out_size) *out_size = glb_sz;
            return glb;
        }
        if (st) { st->failed = true;
            snprintf(st->err, sizeof(st->err), "convert-to-GLB failed"); }
        return raw;
    }
}

/* ── Parallel asset cook ──────────────────────────────────────────────
 *
 * cook_asset is the slow part of a build (BC block-encode = seconds per 2K
 * texture; Assimp+meshopt for large models), and each asset cooks fully
 * independently, so the per-bundle asset loop fans out onto the shared job
 * pool.  Safety (see the per-field reasoning in the loop below):
 *   - emap and resolve_fn/user are READ-ONLY during cooking — shared, no lock.
 *   - each job writes only its own CookJob slot — no shared counter.
 *   - workers NEVER call die()/longjmp or pack_log: g_ctx is thread-local and
 *     belongs to the driver thread, and the editor's log sink is not
 *     thread-safe.  read_asset / ext_rewrite_asset (longjmp on OOM) and
 *     pack_strdup run on the driver BEFORE dispatch; cook_asset itself only
 *     returns NULL/raw on failure (never longjmps), and its internal LOG/warn
 *     lines are intentionally dropped on workers and reconstructed by the
 *     driver from job metadata after the group completes (deterministic order).
 * The lone non-reentrant encoder path (NVTT BC7/BC6H process-global flags) is
 * serialised inside jce_tex_encode; the default BC3/BC5/ASTC policy never hits
 * it, so the common case stays fully parallel. */
void jce_bundle_cook_jobs_range(uint32_t begin, uint32_t end, void *user)
{
    CookCtx *c = (CookCtx *)user;
    for (uint32_t k = begin; k < end; ++k) {
        CookJob *j = &c->jobs[k];
        j->buf = jce_bundle_cook_asset(j->vpath, j->buf, j->in_size, c->resource_root,
                            c->resolve_fn, c->resolve_user, c->emap,
                            c->target_platform, &j->out_size, &j->status);
    }
}
