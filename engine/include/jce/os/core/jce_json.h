/*
 * jce_json.h  Thin facade over cJSON.
 *
 * Goals:
 *   1. Public engine/editor code does not include <cjson/cJSON.h>.
 *   2. Common access patterns (typed-default get, vec3/vec4, "any-of-keys")
 *      collapse to one helper instead of being re-implemented per TU.
 *   3. File I/O (parse a path, print to a path) is one call.
 *
 * `JceJson` is an opaque handle.  It is a forward-declared alias of
 * cJSON, the underlying parser library.  Public consumers cannot access
 * any field on it without separately including <cjson/cJSON.h> and
 * linking the cjson library — which is forbidden by Phase A of the ABI
 * firewall.  Internally, engine bridge TUs that already include cjson
 * may treat JceJson and cJSON as the same struct.
 */

#ifndef JCE_JSON_H
#define JCE_JSON_H


#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stddef.h>

JCE_EXTERN_C_BEGIN

/* Opaque alias: layout is cJSON.  Do NOT introspect from public code. */
typedef struct cJSON JceJson;

/* ── Lifecycle ─────────────────────────────────────────────────────── */

/* Parse text (NUL-terminated) or buffer of `len` bytes.  Returns NULL on
 * failure.  When `len == 0`, treats `text` as NUL-terminated. */
JCE_API JceJson *jce_json_parse(const char *text, size_t len);

/* Read a file from disk and parse it.  Returns NULL on I/O or parse fail. */
JCE_API JceJson *jce_json_parse_file(const char *path);

/* Construct empty container nodes. */
JCE_API JceJson *jce_json_object(void);
JCE_API JceJson *jce_json_array(void);
JCE_API JceJson *jce_json_number(double v);
JCE_API JceJson *jce_json_string(const char *v);
JCE_API JceJson *jce_json_bool(bool v);

/* Serialise a tree.  Returned string must be freed with
 * jce_json_free_string().  When `pretty` is false, output is compact. */
JCE_API char    *jce_json_print(const JceJson *root, bool pretty);

/* Serialise to a file.  When `take_ownership` is true the tree is freed
 * regardless of success.  Returns true on success. */
JCE_API bool JCE_CALL jce_json_write_file(const char *path, JceJson *root,
                             bool pretty, bool take_ownership);

JCE_API void JCE_CALL jce_json_free(JceJson *root);
JCE_API void JCE_CALL jce_json_free_string(char *s);

/* ── Type tests / navigation ───────────────────────────────────────── */

JCE_API bool JCE_CALL jce_json_is_object(const JceJson *j);
JCE_API bool JCE_CALL jce_json_is_array(const JceJson *j);
JCE_API bool JCE_CALL jce_json_is_number(const JceJson *j);
JCE_API bool JCE_CALL jce_json_is_string(const JceJson *j);
JCE_API bool JCE_CALL jce_json_is_bool(const JceJson *j);

JceJson *jce_json_get(const JceJson *obj, const char *key);   /* case-sensitive */
JCE_API bool JCE_CALL jce_json_has(const JceJson *obj, const char *key);

/* Remove (and free) a member by key.  No-op when the key is absent.  Use
 * before a jce_json_set_* call to replace a key without leaving a duplicate
 * (the set helpers append rather than overwrite). */
JCE_API void JCE_CALL jce_json_remove(JceJson *obj, const char *key);

JCE_API int JCE_CALL jce_json_array_size(const JceJson *arr);
JCE_API JceJson *jce_json_array_at(const JceJson *arr, int index);

/* Object iteration.  Use as:
 *   for (JceJson *it = jce_json_first_child(obj); it;
 *        it = jce_json_next_sibling(it)) {
 *       const char *key = jce_json_member_key(it);
 *       ...
 *   }
 */
JCE_API JceJson    *jce_json_first_child  (const JceJson *obj);
JCE_API JceJson    *jce_json_next_sibling (const JceJson *node);
const char *jce_json_member_key   (const JceJson *node);  /* may be NULL for array items */
JCE_API const char *jce_json_string_value (const JceJson *node, const char *def);
JCE_API double JCE_CALL jce_json_number_value(const JceJson *node, double def);

/* ── Typed accessors with defaults ─────────────────────────────────── */

JCE_API double JCE_CALL jce_json_get_number(const JceJson *obj, const char *key, double def);
JCE_API int JCE_CALL jce_json_get_int(const JceJson *obj, const char *key, int def);
JCE_API bool JCE_CALL jce_json_get_bool(const JceJson *obj, const char *key, bool def);
const char *jce_json_get_string(const JceJson *obj, const char *key,
                                const char *def);

/* "Any-of" variants: try each key in order; returns first hit or default. */
JCE_API double JCE_CALL jce_json_get_number_any(const JceJson *obj,
                                    const char *const *keys, int n, double def);
const char *jce_json_get_string_any(const JceJson *obj,
                                    const char *const *keys, int n,
                                    const char *def);

/* Read N consecutive floats from an array member.  Missing or
 * non-numeric elements fall back to `def[i]` (or 0.0f when def==NULL). */
JCE_API void JCE_CALL jce_json_get_floats(const JceJson *obj, const char *key,
                             float *out, int n, const float *def);

/* Read "<prefix>X","<prefix>Y","<prefix>Z" pattern (e.g. posX/posY/posZ).
 * Falls back to def[i] component-wise. */
JCE_API void JCE_CALL jce_json_get_xyz(const JceJson *obj, const char *prefix,
                          float out[3], const float def[3]);
JCE_API void JCE_CALL jce_json_get_xyzw(const JceJson *obj, const char *prefix,
                           float out[4], const float def[4]);

/* ── Builders (also collapse the cJSON_AddX one-liner family) ─────── */

JCE_API void JCE_CALL jce_json_set_number(JceJson *obj, const char *key, double v);
JCE_API void JCE_CALL jce_json_set_int(JceJson *obj, const char *key, int v);
JCE_API void JCE_CALL jce_json_set_bool(JceJson *obj, const char *key, bool v);
JCE_API void JCE_CALL jce_json_set_string(JceJson *obj, const char *key, const char *v);
/* Attach a child object/array; ownership transfers to parent. */
JCE_API void JCE_CALL jce_json_set_child(JceJson *obj, const char *key, JceJson *child);
JCE_API void JCE_CALL jce_json_array_push(JceJson *arr, JceJson *item);
JCE_API void JCE_CALL jce_json_array_push_number(JceJson *arr, double v);
JCE_API void JCE_CALL jce_json_array_push_string(JceJson *arr, const char *v);

/* Symmetric helpers for the xyz / xyzw write side. */
JCE_API void JCE_CALL jce_json_set_xyz(JceJson *obj, const char *prefix, const float v[3]);
JCE_API void JCE_CALL jce_json_set_xyzw(JceJson *obj, const char *prefix, const float v[4]);
JCE_API void JCE_CALL jce_json_set_float_array(JceJson *obj, const char *key,
                                  const float *v, int n);

JCE_EXTERN_C_END

#endif /* JCE_JSON_H */
