/*
 * jce_model_import_settings.c  See jce_model_import_settings.h.
 */

#include "jce_model_import_settings.h"

#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>

/* A plain C header of #defines -- which is why this is a .c and not the
 * .cpp it started as.  tools/lint/check_engine_cxx_bridges.py caught
 * that on its first run: a .cpp in this engine has to be a thin wrapper
 * over a C++ library with no C API, and aiProcess_* is not that. */
#include <assimp/postprocess.h>

#include <stdio.h>
#include <string.h>

#define LOG_TAG "model_import"

void jce_model_import_settings_default(JceModelImportSettings *out)
{
    if (!out) return;
    /* These four booleans ARE the constants the importer used to pass, and the
     * scale is the identity.  Changing one changes every model in every
     * project that has no sidecar, so they are written here once and the
     * importer reads them rather than repeating the list. */
    out->scale        = 1.0f;
    out->gen_normals  = true;
    out->gen_tangents = true;
    out->flip_uv      = true;
    out->merge_meshes = true;
    out->present      = false;
}

bool jce_model_import_settings_load(const char *model_path,
                                               JceModelImportSettings *out)
{
    if (!out) return false;
    jce_model_import_settings_default(out);
    if (!model_path || !model_path[0]) return false;

    char side[1024];
    const int n = snprintf(side, sizeof(side), "%s.import.json", model_path);
    if (n <= 0 || (size_t)n >= sizeof(side)) return false;

    uint64_t sz = 0;
    char *buf = (char *)jce_fs_host_read_all(side, &sz);
    if (!buf) return false;                       /* no sidecar: defaults */
    if (sz == 0 || sz > (1u << 20)) { jce_fs_buffer_free(buf); return false; }

    JceJson *root = jce_json_parse(buf, (size_t)sz);
    jce_fs_buffer_free(buf);
    if (!root) {
        LOG_WARN(LOG_TAG, "invalid JSON in import sidecar, using defaults: %s",
                 side);
        return false;
    }

    /* kind 1 = model in the panel's enum; a TEXTURE sidecar sits beside a
     * texture and carries entirely different keys, so reading its booleans
     * (all absent, therefore all default) would be harmless but meaningless.
     * Refusing is the honest answer and keeps `present` truthful. */
    const int kind = (int)jce_json_get_number(root, "kind", -1.0);
    if (kind != 1) {
        jce_json_free(root);
        return false;
    }

    const double sc = jce_json_get_number(root, "scale", 1.0);
    out->scale        = (sc > 0.0) ? (float)sc : 1.0f;
    out->gen_normals  = jce_json_get_bool(root, "gen_normals",  true);
    out->gen_tangents = jce_json_get_bool(root, "gen_tangents", true);
    out->flip_uv      = jce_json_get_bool(root, "flip_uv",      true);
    out->merge_meshes = jce_json_get_bool(root, "merge_meshes", true);
    out->present      = true;
    jce_json_free(root);
    return true;
}

unsigned jce_model_import_assimp_flags(
    const JceModelImportSettings *s, const char *ext_hint)
{
    JceModelImportSettings def;
    if (!s) { jce_model_import_settings_default(&def); s = &def; }

    /* Triangulate is not a setting.  Every downstream consumer -- the mesh
     * upload, the collider builder, the picker -- indexes triangles, so a
     * quad-bearing scene is not a different look, it is a crash waiting for a
     * quad. */
    unsigned f = aiProcess_Triangulate;
    if (s->gen_normals)  f |= aiProcess_GenSmoothNormals;
    if (s->gen_tangents) f |= aiProcess_CalcTangentSpace;
    if (s->flip_uv)      f |= aiProcess_FlipUVs;
    if (s->merge_meshes) f |= aiProcess_PreTransformVertices;

    /* FBX is authored in centimetres; the flag makes assimp apply the file's
     * own UnitScaleFactor so geometry arrives in metres.  This is NOT the
     * user's `scale` and must not be folded into it: one is a unit fix the
     * file asks for, the other is an authored multiplier. */
    if (ext_hint) {
        const size_t n = strlen(ext_hint);
        if (n >= 3) {
            const char *t = ext_hint + n - 3;
            if ((t[0] == 'f' || t[0] == 'F') &&
                (t[1] == 'b' || t[1] == 'B') &&
                (t[2] == 'x' || t[2] == 'X'))
                f |= aiProcess_GlobalScale;
        }
    }
    return f;
}
