/*
 * jce_shadergraph_registry.cpp — declarative node table.
 *
 * Adding a node in Phase B/C/D = append one entry here + (Phase C) fill
 * its glsl_snippet field.  No switch statement to maintain.
 */

#include "shadergraph/jce_shadergraph_registry.h"

#include <cstddef>

namespace jce_sg {

namespace {

/* Socket arrays — one per node type.  Names must stay stable across
 * versions (they're surfaced in tooltips and may end up as GLSL
 * uniform / varying identifiers in Phase C). */

const Socket k_output[] = {
    { SK_INPUT, DT_COLOR, "BaseColor" },
    { SK_INPUT, DT_FLOAT, "Metallic"  },
    { SK_INPUT, DT_FLOAT, "Roughness" },
    { SK_INPUT, DT_COLOR, "Emissive"  },
};
const Socket k_color  [] = { { SK_OUTPUT, DT_COLOR, "RGBA" } };
const Socket k_float  [] = { { SK_OUTPUT, DT_FLOAT, "Out"  } };
const Socket k_texture[] = { { SK_OUTPUT, DT_COLOR, "RGBA" } };
const Socket k_mul_c  [] = {
    { SK_INPUT,  DT_COLOR, "A" },
    { SK_INPUT,  DT_COLOR, "B" },
    { SK_OUTPUT, DT_COLOR, "Out" },
};
const Socket k_mul_f  [] = {
    { SK_INPUT,  DT_FLOAT, "A" },
    { SK_INPUT,  DT_FLOAT, "B" },
    { SK_OUTPUT, DT_FLOAT, "Out" },
};
const Socket k_add_f  [] = {
    { SK_INPUT,  DT_FLOAT, "A" },
    { SK_INPUT,  DT_FLOAT, "B" },
    { SK_OUTPUT, DT_FLOAT, "Out" },
};
const Socket k_normal [] = { { SK_OUTPUT, DT_COLOR, "Normal" } };
const Socket k_uv     [] = { { SK_OUTPUT, DT_FLOAT, "Tile"   } };
const Socket k_sub_f  [] = {
    { SK_INPUT,  DT_FLOAT, "A" },
    { SK_INPUT,  DT_FLOAT, "B" },
    { SK_OUTPUT, DT_FLOAT, "Out" },
};
const Socket k_lerp_c [] = {
    { SK_INPUT,  DT_COLOR, "A" },
    { SK_INPUT,  DT_COLOR, "B" },
    { SK_INPUT,  DT_FLOAT, "T" },
    { SK_OUTPUT, DT_COLOR, "Out" },
};
const Socket k_fresnel[] = {
    { SK_INPUT,  DT_FLOAT, "Bias" },
    { SK_OUTPUT, DT_FLOAT, "Out"  },
};

#define ENTRY(T, LBL, SARR, SNIPPET) \
    { T, LBL, SARR, (int)(sizeof(SARR) / sizeof((SARR)[0])), SNIPPET }

/* GLSL snippet templates.
 *
 * Syntax:
 *   $0, $1, $2  - substituted with the upstream variable expression for
 *                 the node's 0th, 1st, 2nd INPUT socket (in registry
 *                 declaration order).  When an input is unconnected,
 *                 codegen substitutes a zero/identity constant of the
 *                 expected dtype.
 *
 * Nodes producing constants from their payload (NT_COLOR, NT_FLOAT,
 * NT_UV) and the singleton NT_OUTPUT are special-cased inside codegen
 * and leave `glsl_snippet` NULL.
 *
 * Texture samplers are bound to the same slots fs_pbr.sc uses:
 *   s_albedo  (slot 0) for NT_TEXTURE
 *   s_normalMap (slot 2) for NT_NORMAL_MAP
 * Multi-sampler / configurable bindings are deferred to Phase D.
 */
const NodeMeta k_table[] = {
    ENTRY(NT_OUTPUT,     "PBR Output",        k_output , nullptr),
    ENTRY(NT_COLOR,      "Color",             k_color  , nullptr),
    ENTRY(NT_FLOAT,      "Float",             k_float  , nullptr),
    ENTRY(NT_TEXTURE,    "Texture",           k_texture, "texture2D(s_albedo, mat_uv)"),
    ENTRY(NT_MUL_C,      "Multiply (Color)",  k_mul_c  , "($0 * $1)"),
    ENTRY(NT_MUL_F,      "Multiply (Float)",  k_mul_f  , "($0 * $1)"),
    ENTRY(NT_ADD_F,      "Add (Float)",       k_add_f  , "($0 + $1)"),
    ENTRY(NT_NORMAL_MAP, "Normal Map",        k_normal , "vec4(texture2D(s_normalMap, mat_uv).xyz * 2.0 - vec3_splat(1.0), 1.0)"),
    ENTRY(NT_UV,         "UV (Tile/Offset)",  k_uv     , nullptr),
    ENTRY(NT_SUB_F,      "Subtract (Float)",  k_sub_f  , "($0 - $1)"),
    ENTRY(NT_LERP_C,     "Lerp (Color)",      k_lerp_c , "mix($0, $1, vec4_splat($2))"),
    ENTRY(NT_FRESNEL,    "Fresnel (Preview)", k_fresnel, "pow(1.0 - $0, 5.0)"),
};
const int k_table_count = (int)(sizeof(k_table) / sizeof(k_table[0]));

#undef ENTRY

/* User-addable types (excludes NT_OUTPUT). */
const NodeType k_addable[] = {
    NT_COLOR, NT_FLOAT, NT_TEXTURE, NT_NORMAL_MAP, NT_UV,
    NT_MUL_C, NT_MUL_F, NT_ADD_F, NT_SUB_F, NT_LERP_C, NT_FRESNEL,
};
const int k_addable_count = (int)(sizeof(k_addable) / sizeof(k_addable[0]));

} /* anonymous namespace */

const NodeMeta *meta_for(NodeType t)
{
    for (int i = 0; i < k_table_count; ++i)
        if (k_table[i].type == t) return &k_table[i];
    return nullptr;
}

const Socket *sockets_for(NodeType t, int *out_count)
{
    const NodeMeta *m = meta_for(t);
    if (!m) { if (out_count) *out_count = 0; return nullptr; }
    if (out_count) *out_count = m->socket_count;
    return m->sockets;
}

const char *label_for(NodeType t)
{
    const NodeMeta *m = meta_for(t);
    return m ? m->label : "?";
}

const NodeType *addable_types(int *out_count)
{
    if (out_count) *out_count = k_addable_count;
    return k_addable;
}

} /* namespace jce_sg */
