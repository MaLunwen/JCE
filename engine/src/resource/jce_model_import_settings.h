/*
 * jce_model_import_settings.h  The per-asset model import options the editor
 * writes and the importer never read.
 *
 * WHAT WAS WRONG.  editor/src/panels/jce_panel_import_presets.cpp writes ten
 * keys into `<asset>.import.json` for every model an import preset is applied
 * to.  The importer's assimp post-process flags were CONSTANTS:
 *
 *     aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_FlipUVs
 *     | aiProcess_CalcTangentSpace | aiProcess_PreTransformVertices
 *
 * so gen_normals, gen_tangents, flip_uv and merge_meshes were all forced ON
 * whatever the preset said, and `scale` reached nothing at all.  A designer set
 * "do not flip UVs" on a folder of models, the panel wrote it to disk, and
 * every one of them still imported flipped -- with no error, because the file
 * was written exactly as asked; only nothing read it.
 *
 * (The five collider keys are NOT here: the collider generator does exist and
 * is reachable, so gen_collider / col_* are a different story.)
 *
 * THE DEFAULTS ARE TODAY'S CONSTANTS.  A model with no sidecar, or one whose
 * sidecar omits a key, imports byte-identically to before -- which is the only
 * way a change to an importer can be landed without re-importing every asset
 * in every project to see what moved.
 *
 * Split out so the JSON -> struct step and the struct -> flags step are both
 * pure and can be asserted headlessly.  Deciding inside the loader would mean
 * the only way to check a flag is to import a file and look at the vertices,
 * and vertices that are subtly wrong look exactly like vertices that are right.
 *
 * Layer: Resource.  Internal to the importer.
 */

#ifndef JCE_MODEL_IMPORT_SETTINGS_H
#define JCE_MODEL_IMPORT_SETTINGS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Uniform scale applied to positions AFTER assimp.  <= 0 means "not
     * authored" and is treated as 1.0 -- a zero scale would collapse the mesh
     * to a point, which is never what a half-filled sidecar means. */
    float scale;
    bool  gen_normals;    /* aiProcess_GenSmoothNormals   -- default true */
    bool  gen_tangents;   /* aiProcess_CalcTangentSpace   -- default true */
    bool  flip_uv;        /* aiProcess_FlipUVs            -- default true */
    bool  merge_meshes;   /* aiProcess_PreTransformVertices -- default true */
    bool  present;        /* a sidecar was found AND parsed */
} JceModelImportSettings;

/* Today's hard-coded behaviour, exactly. */
void jce_model_import_settings_default(JceModelImportSettings *out);

/* Read `<model_path>.import.json`.  Missing file, unreadable file, bad JSON and
 * a sidecar for a TEXTURE (kind != 1) all leave `out` at the defaults and
 * return false -- an import that cannot read its options must behave like one
 * that has none, not like one whose options are all false. */
bool jce_model_import_settings_load(const char *model_path,
                                    JceModelImportSettings *out);

/* The assimp post-process mask these settings ask for, including
 * aiProcess_Triangulate (never optional -- the mesh pipeline is triangles) and
 * the FBX unit-scale flag when `ext_hint` names an .fbx. */
unsigned jce_model_import_assimp_flags(const JceModelImportSettings *s,
                                       const char *ext_hint);

#ifdef __cplusplus
}
#endif

#endif /* JCE_MODEL_IMPORT_SETTINGS_H */
