/*
 * jce_json_classify.h  Content-based JSON asset-type classification.
 *
 * The engine reads/writes 20+ distinct JSON formats (scene, prefab, anim
 * state machine, particles, materials, render settings, ...).  This sniffs
 * a file's TOP-LEVEL keys (plus a couple of path hints for formats with no
 * self-identifying schema, e.g. i18n string tables) and returns which
 * format it is — used by the Code Viewer's type badge so a user always
 * knows WHAT kind of JSON they are looking at.
 *
 * Discriminators mirror the actual parsers (jce_scene_components_json.c,
 * jce_anim_sm.c, jce_particles.c, jce_render_settings.c, ...).  Order
 * matters: prefab is tested BEFORE scene because both carry the
 * {contract} envelope.
 */

#ifndef JCE_JSON_CLASSIFY_H
#define JCE_JSON_CLASSIFY_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum JceJsonKind {
    JCE_JSONK_NOT_JSON = 0,     /* not classified / not a .json file  */
    JCE_JSONK_INVALID,          /* .json that fails to parse          */
    JCE_JSONK_GENERIC,          /* valid JSON, no known schema        */
    JCE_JSONK_SCENE,            /* wrapped / bare-wrapped / flat      */
    JCE_JSONK_PREFAB,
    JCE_JSONK_ANIM_SM,
    JCE_JSONK_AVATAR_MASK,
    JCE_JSONK_PARTICLES,
    JCE_JSONK_MATERIAL,
    JCE_JSONK_PHYSMAT,
    JCE_JSONK_RENDER_SETTINGS,
    JCE_JSONK_RENDER_PIPELINE,
    JCE_JSONK_PHYS_LAYERS,
    JCE_JSONK_PROJECT,
    JCE_JSONK_PROJECT_SETTINGS,
    JCE_JSONK_TAGS_LAYERS,
    JCE_JSONK_IMPOSTOR,
    JCE_JSONK_SPRITES,
    JCE_JSONK_GRAPH,            /* shader / VFX / material graph      */
    JCE_JSONK_I18N,
    JCE_JSONK_EDITOR_PREFS,
    JCE_JSONK_COUNT
} JceJsonKind;

/* Classify `text` (len bytes, may be a truncated read — the top-level keys
 * still parse for every format we emit).  `path` supplies extension/dir
 * hints; may be NULL. */
JceJsonKind jce_json_classify(const char *text, int len, const char *path);

/* Localized, human-readable badge label ("Scene", "Anim State Machine"...). */
const char *jce_json_kind_label(JceJsonKind k);

/* Badge fill color (IM_COL32-packed ABGR as ImGui expects). */
unsigned int jce_json_kind_color(JceJsonKind k);

#ifdef __cplusplus
}
#endif

#endif /* JCE_JSON_CLASSIFY_H */
