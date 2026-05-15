/*
 * jce_widget_inspector_fields.h  Drop-in ImGui drawers for Unity-
 * style Inspector field types not covered by jce_reflect's enum.
 *
 * Existing JceFieldType handles bool / int / float / vec2-4 / quat /
 * color / string / enum / asset-ref / array.  These widgets add the
 * remaining Unity types game code authors expect:
 *
 *   - Vector2Int / Vector3Int  (integer-only XY[Z])
 *   - Rect                     (x, y, w, h)
 *   - Bounds                   (center xyz, extents xyz)
 *   - LayerMask                (32 bits — name table + flag popup)
 *   - Tag picker               (string from a registered name list)
 *   - Reset-to-default helper  (right-click → revert to default)
 *
 * Each widget mutates its argument in place and returns `true` when
 * the user edited the value this frame.
 */

#ifndef JCE_WIDGET_INSPECTOR_FIELDS_H
#define JCE_WIDGET_INSPECTOR_FIELDS_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Vector2Int / Vector3Int. */
bool jce_widget_vec2int(const char *label, int32_t v[2]);
bool jce_widget_vec3int(const char *label, int32_t v[3]);

/* Rect: x, y, w, h floats. */
bool jce_widget_rect(const char *label, float rect[4]);

/* Bounds: center.xyz + extents.xyz. */
bool jce_widget_bounds(const char *label, float center[3], float extents[3]);

/* LayerMask: 32-bit flag field.  `layer_names` is an array of 32
 * NUL-terminated strings — unused layers can be NULL or "". */
bool jce_widget_layer_mask(const char *label, uint32_t *mask,
                           const char *const *layer_names);

/* Tag picker: combo of `tag_names` (NULL-terminated array).  `value`
 * is a fixed-size char buffer of `cap` bytes that receives the
 * selected tag. */
bool jce_widget_tag_picker(const char *label, char *value, size_t cap,
                           const char *const *tag_names);

/* ── Reset-to-default helper ─────────────────────────────────── */

/* Append a right-click context menu to the previous widget.  When
 * the user picks "Reset", `revert_fn` is invoked.  `user_data` is
 * passed through.  Returns true if the user just triggered a reset. */
typedef void (*JceFieldResetFn)(void *user_data);
bool jce_widget_field_reset_context(const char *id,
                                     JceFieldResetFn revert_fn,
                                     void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* JCE_WIDGET_INSPECTOR_FIELDS_H */
