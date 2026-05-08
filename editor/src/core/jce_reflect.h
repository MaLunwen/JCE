/*
 * jce_reflect.h  Minimal C-friendly component reflection.
 *
 * Goal: replace handwritten ImGui blocks in jce_panel_inspector.cpp
 * with a metadata-driven drawer (Unity SerializedProperty / UE
 * UPROPERTY equivalent at a much smaller scale).
 *
 * Usage (registration, in a .cpp file):
 *
 *     JCE_REFLECT_BEGIN(JceTransform, "Transform")
 *         JCE_FIELD(JceTransform, position, JCE_FT_VEC3,    "Position")
 *         JCE_FIELD(JceTransform, rotation, JCE_FT_QUAT,    "Rotation")
 *         JCE_FIELD(JceTransform, scale,    JCE_FT_VEC3,    "Scale")
 *     JCE_REFLECT_END()
 *
 * Then inspector code can do:
 *
 *     const JceReflectType *t = jce_reflect_find("Transform");
 *     jce_reflect_draw(t, transform_ptr);
 *
 * This is intentionally additive — existing handwritten draw_comp_*
 * paths still work.  Migration is done component-by-component so we
 * never break the editor.
 */

#ifndef JCE_REFLECT_H
#define JCE_REFLECT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JCE_FT_NONE = 0,
    JCE_FT_BOOL,
    JCE_FT_INT,
    JCE_FT_FLOAT,
    JCE_FT_VEC2,
    JCE_FT_VEC3,
    JCE_FT_VEC4,
    JCE_FT_QUAT,        /* edited as Euler degrees */
    JCE_FT_COLOR3,      /* float[3] colour swatch */
    JCE_FT_COLOR4,      /* float[4] colour swatch */
    JCE_FT_STRING,      /* fixed-size char[] (single line) */
    JCE_FT_STRING_MULTILINE, /* fixed-size char[] rendered as multi-line text box */
    JCE_FT_ENUM_INT,    /* int with named labels (extra: const char* const*) */
    JCE_FT_ASSET_REF,   /* fixed-size char[] holding asset path + Browse + DnD target */
    JCE_FT_STRUCT_NESTED, /* recurse into another registered reflect type (by display name) */
    JCE_FT_ARRAY,       /* inline array of POD elements (extra: element_type/size/max/count_offset) */
    JCE_FT_COUNT
} JceFieldType;

typedef struct JceReflectField {
    const char         *name;       /* C identifier */
    const char         *label;      /* UI label (may equal name) */
    JceFieldType        type;
    size_t              offset;     /* offsetof(struct, member) */
    size_t              size;       /* sizeof(member) — used for STRING/ASSET_REF */
    /* Optional: float clamp for FLOAT/VEC*/
    float               vmin;
    float               vmax;
    float               vstep;
    /* Optional: NULL-terminated array of enum labels (when type==ENUM_INT). */
    const char *const  *enum_labels;
    int                 enum_count;
    /* Optional metadata for new field types (additive — old macros default 0). */
    const char         *nested_type_name; /* JCE_FT_STRUCT_NESTED: display name */
    const char         *asset_kind;       /* JCE_FT_ASSET_REF: e.g. "texture" / "model" / "material" / "" = any */
    JceFieldType        element_type;     /* JCE_FT_ARRAY: element kind */
    size_t              element_size;     /* JCE_FT_ARRAY: sizeof(element) */
    size_t              count_offset;     /* JCE_FT_ARRAY: offsetof(struct, counter int/size_t sibling) */
    int                 max_count;        /* JCE_FT_ARRAY: array capacity */
    /* Optional Unity-style attribute: hover tooltip text.  NULL = none. */
    const char         *tooltip;
} JceReflectField;

typedef struct JceReflectType {
    const char            *display_name;
    const char            *type_name;     /* C struct name */
    size_t                 size;
    const JceReflectField *fields;
    int                    field_count;
    const void            *default_instance; /* optional: blob the size of the struct holding defaults; if non-NULL, inspector shows right-click "Reset to Default" per field. */
} JceReflectType;

/* Registry — call once at startup (or via static initializer pattern). */
void                  jce_reflect_register(const JceReflectType *type);
const JceReflectType *jce_reflect_find(const char *display_name);
int                   jce_reflect_count(void);
const JceReflectType *jce_reflect_at(int idx);

/* C++ side: ImGui drawer.  Returns true if any field changed this frame. */
#ifdef __cplusplus
} /* extern "C" */
bool jce_reflect_draw(const JceReflectType *type, void *instance);
extern "C" {
#endif

/* ── Registration helpers (C99-friendly designated init) ─────────── */

#define JCE_REFLECT_BEGIN(STRUCT, DISPLAY)                                  \
    static const JceReflectField JCE_PASTE(g_jce_fields_, STRUCT)[] = {

#define JCE_FIELD(STRUCT, MEMBER, TYPE, LABEL)                              \
    { #MEMBER, LABEL, TYPE, offsetof(STRUCT, MEMBER),                       \
      sizeof(((STRUCT *)0)->MEMBER), 0.0f, 0.0f, 0.0f, NULL, 0,             \
      NULL, NULL, JCE_FT_NONE, 0, 0, 0 },

#define JCE_FIELD_RANGE(STRUCT, MEMBER, TYPE, LABEL, MN, MX, STEP)          \
    { #MEMBER, LABEL, TYPE, offsetof(STRUCT, MEMBER),                       \
      sizeof(((STRUCT *)0)->MEMBER), (MN), (MX), (STEP), NULL, 0,           \
      NULL, NULL, JCE_FT_NONE, 0, 0, 0 },

#define JCE_FIELD_ENUM(STRUCT, MEMBER, LABEL, LABELS, COUNT)                \
    { #MEMBER, LABEL, JCE_FT_ENUM_INT, offsetof(STRUCT, MEMBER),            \
      sizeof(((STRUCT *)0)->MEMBER), 0.0f, 0.0f, 0.0f, (LABELS), (COUNT),   \
      NULL, NULL, JCE_FT_NONE, 0, 0, 0 },

#define JCE_FIELD_ASSET(STRUCT, MEMBER, LABEL, KIND)                        \
    { #MEMBER, LABEL, JCE_FT_ASSET_REF, offsetof(STRUCT, MEMBER),           \
      sizeof(((STRUCT *)0)->MEMBER), 0.0f, 0.0f, 0.0f, NULL, 0,             \
      NULL, (KIND), JCE_FT_NONE, 0, 0, 0 },

#define JCE_FIELD_NESTED(STRUCT, MEMBER, LABEL, NESTED_TYPE_DISPLAY)        \
    { #MEMBER, LABEL, JCE_FT_STRUCT_NESTED, offsetof(STRUCT, MEMBER),       \
      sizeof(((STRUCT *)0)->MEMBER), 0.0f, 0.0f, 0.0f, NULL, 0,             \
      (NESTED_TYPE_DISPLAY), NULL, JCE_FT_NONE, 0, 0, 0 },

#define JCE_FIELD_ARRAY(STRUCT, MEMBER, LABEL, ELEM_TYPE, ELEM_SIZE,        \
                        COUNT_MEMBER, MAX_COUNT)                            \
    { #MEMBER, LABEL, JCE_FT_ARRAY, offsetof(STRUCT, MEMBER),               \
      sizeof(((STRUCT *)0)->MEMBER), 0.0f, 0.0f, 0.0f, NULL, 0,             \
      NULL, NULL, (ELEM_TYPE), (ELEM_SIZE),                                 \
      offsetof(STRUCT, COUNT_MEMBER), (MAX_COUNT) },

/* Unity-style attribute macros — additive on top of JCE_FIELD*.  Wrap
 * an existing field declaration's value to attach metadata. */

/* Field with hover tooltip (Unity's [Tooltip("...")]). */
#define JCE_FIELD_TOOLTIP(STRUCT, MEMBER, TYPE, LABEL, TOOLTIP)             \
    { #MEMBER, LABEL, TYPE, offsetof(STRUCT, MEMBER),                       \
      sizeof(((STRUCT *)0)->MEMBER), 0.0f, 0.0f, 0.0f, NULL, 0,             \
      NULL, NULL, JCE_FT_NONE, 0, 0, 0, TOOLTIP },

/* Range slider (Unity's [Range(min,max)]).  Caller picks step. */
#define JCE_FIELD_RANGE_TT(STRUCT, MEMBER, TYPE, LABEL, MN, MX, STEP, TT)   \
    { #MEMBER, LABEL, TYPE, offsetof(STRUCT, MEMBER),                       \
      sizeof(((STRUCT *)0)->MEMBER), (MN), (MX), (STEP), NULL, 0,           \
      NULL, NULL, JCE_FT_NONE, 0, 0, 0, TT },

/* Multi-line text area (Unity's [Multiline] / [TextArea]). */
#define JCE_FIELD_MULTILINE(STRUCT, MEMBER, LABEL)                          \
    { #MEMBER, LABEL, JCE_FT_STRING_MULTILINE, offsetof(STRUCT, MEMBER),    \
      sizeof(((STRUCT *)0)->MEMBER), 0.0f, 0.0f, 0.0f, NULL, 0,             \
      NULL, NULL, JCE_FT_NONE, 0, 0, 0, NULL },

#define JCE_REFLECT_END(STRUCT, DISPLAY)                                    \
    };                                                                      \
    static const JceReflectType JCE_PASTE(g_jce_type_, STRUCT) = {          \
        DISPLAY, #STRUCT, sizeof(STRUCT),                                   \
        JCE_PASTE(g_jce_fields_, STRUCT),                                   \
        (int)(sizeof(JCE_PASTE(g_jce_fields_, STRUCT)) /                    \
              sizeof(JceReflectField)),                                     \
        NULL                                                                \
    };

/* Variant: associates a static-lifetime defaults blob with the type so the
 * inspector can offer right-click "Reset to Default" per field. */
#define JCE_REFLECT_END_DEFAULTS(STRUCT, DISPLAY, DEFAULTS_PTR)             \
    };                                                                      \
    static const JceReflectType JCE_PASTE(g_jce_type_, STRUCT) = {          \
        DISPLAY, #STRUCT, sizeof(STRUCT),                                   \
        JCE_PASTE(g_jce_fields_, STRUCT),                                   \
        (int)(sizeof(JCE_PASTE(g_jce_fields_, STRUCT)) /                    \
              sizeof(JceReflectField)),                                     \
        (DEFAULTS_PTR)                                                      \
    };

#define JCE_PASTE_(a, b) a##b
#define JCE_PASTE(a, b)  JCE_PASTE_(a, b)

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* JCE_REFLECT_H */
