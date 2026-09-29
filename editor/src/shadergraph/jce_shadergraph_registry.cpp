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
    /* The destination the Normal Map node never had.  DT_VEC3, matching what
     * a tangent-space normal IS -- the node used to wrap its vec3 in a vec4
     * only because DT_COLOR was the only three-component-ish type any socket
     * used.  APPENDED, so an older .graph's link indices still mean what they
     * meant. */
    { SK_INPUT, DT_VEC3,  "Normal"    },
};
const Socket k_color  [] = { { SK_OUTPUT, DT_COLOR, "RGBA" } };
const Socket k_float  [] = { { SK_OUTPUT, DT_FLOAT, "Out"  } };
const Socket k_texture[] = {
    /* UV in, so a UV node can tile or offset this sample.  Unconnected
     * substitutes mat_uv, which is what every Texture node used to hard-code
     * and is still the right default. */
    { SK_INPUT,  DT_VEC2,  "UV",  "mat_uv" },
    { SK_OUTPUT, DT_COLOR, "RGBA" },
};
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
const Socket k_normal [] = {
    { SK_INPUT,  DT_VEC2, "UV", "mat_uv" },
    /* VEC3, not COLOR: it is a tangent-space normal, and the PBR Output's
     * Normal socket is VEC3 -- the type checker requires exact equality, so a
     * vec4-wrapped normal could not have been connected to it even after the
     * socket existed. */
    { SK_OUTPUT, DT_VEC3, "Normal" },
};
/* A UV is two numbers.  This socket was a single FLOAT called "Tile" that
 * codegen emitted as a bare constant -- nothing consumed it and nothing
 * could, because no node had a UV input. */
const Socket k_uv     [] = { { SK_OUTPUT, DT_VEC2, "UV" } };
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
/* A REAL Fresnel.
 *
 * The old sockets were { Bias : float } -> { Out : float } and the snippet was
 * pow(1.0 - Bias, 5.0): a curve of its input, with no N and no V, named after
 * an effect it could not produce.  Nothing correct can have depended on it, so
 * the sockets are REPLACED rather than appended -- and a saved graph that used
 * it fails the type check loudly (a float into a vec3 socket) instead of
 * quietly meaning something else.
 *
 * Both vectors default to the surface's own, which is the whole point of the
 * node; Power defaults to 5, the Schlick exponent. */
const Socket k_fresnel[] = {
    { SK_INPUT,  DT_VEC3,  "Normal",   "normalize(mat_normal_ws)" },
    { SK_INPUT,  DT_VEC3,  "View Dir",
      "normalize(u_cameraPos.xyz - mat_world_pos)" },
    { SK_INPUT,  DT_FLOAT, "Power",    "5.0" },
    { SK_OUTPUT, DT_FLOAT, "Out" },
};



/* ── Appended 2026-09-08 ──────────────────────────────────────────────
 * Socket NAMES are user-visible and socket ORDER is what a link means: a
 * .matgraph.json stores endpoints by index, so reordering a table silently
 * rewires every saved graph.  Append within a table; never insert. */

/* Geometry the fragment already has.  These read the arguments
 * jce_graph_material() is handed, so they cannot disagree with the surface
 * being shaded. */
const Socket k_g_vec3_out[] = { { SK_OUTPUT, DT_VEC3, "Out" } };
const Socket k_g_vec2_out[] = { { SK_OUTPUT, DT_VEC2, "Out" } };

/* Float maths. */
const Socket k_f1[] = { { SK_INPUT, DT_FLOAT, "In" },
                        { SK_OUTPUT, DT_FLOAT, "Out" } };
const Socket k_f2[] = { { SK_INPUT, DT_FLOAT, "A" },
                        { SK_INPUT, DT_FLOAT, "B" },
                        { SK_OUTPUT, DT_FLOAT, "Out" } };
/* Divide's B defaults to 1, not 0: the identity, and not a division by zero
 * for a node the author has only just dropped on the canvas. */
const Socket k_f_div[] = { { SK_INPUT, DT_FLOAT, "A" },
                           { SK_INPUT, DT_FLOAT, "B", "1.0" },
                           { SK_OUTPUT, DT_FLOAT, "Out" } };
const Socket k_f_pow[] = { { SK_INPUT, DT_FLOAT, "A" },
                           { SK_INPUT, DT_FLOAT, "B", "2.0" },
                           { SK_OUTPUT, DT_FLOAT, "Out" } };
const Socket k_f_clamp[] = { { SK_INPUT, DT_FLOAT, "In" },
                             { SK_INPUT, DT_FLOAT, "Min", "0.0" },
                             { SK_INPUT, DT_FLOAT, "Max", "1.0" },
                             { SK_OUTPUT, DT_FLOAT, "Out" } };
const Socket k_f_step[] = { { SK_INPUT, DT_FLOAT, "Edge", "0.5" },
                            { SK_INPUT, DT_FLOAT, "In" },
                            { SK_OUTPUT, DT_FLOAT, "Out" } };
const Socket k_f_smooth[] = { { SK_INPUT, DT_FLOAT, "Edge0", "0.0" },
                              { SK_INPUT, DT_FLOAT, "Edge1", "1.0" },
                              { SK_INPUT, DT_FLOAT, "In" },
                              { SK_OUTPUT, DT_FLOAT, "Out" } };
const Socket k_f_lerp[] = { { SK_INPUT, DT_FLOAT, "A" },
                            { SK_INPUT, DT_FLOAT, "B" },
                            { SK_INPUT, DT_FLOAT, "T", "0.5" },
                            { SK_OUTPUT, DT_FLOAT, "Out" } };
/* Remap takes both ranges as sockets rather than as a payload, so an upstream
 * node can drive them; a remap whose ends are constants is the common case but
 * not the interesting one. */
const Socket k_f_remap[] = { { SK_INPUT, DT_FLOAT, "In" },
                             { SK_INPUT, DT_FLOAT, "From Min", "0.0" },
                             { SK_INPUT, DT_FLOAT, "From Max", "1.0" },
                             { SK_INPUT, DT_FLOAT, "To Min",   "0.0" },
                             { SK_INPUT, DT_FLOAT, "To Max",   "1.0" },
                             { SK_OUTPUT, DT_FLOAT, "Out" } };

/* Colour maths. */
const Socket k_c2[] = { { SK_INPUT, DT_COLOR, "A" },
                        { SK_INPUT, DT_COLOR, "B" },
                        { SK_OUTPUT, DT_COLOR, "Out" } };
const Socket k_c_scale[] = { { SK_INPUT, DT_COLOR, "Color" },
                             { SK_INPUT, DT_FLOAT, "Scale", "1.0" },
                             { SK_OUTPUT, DT_COLOR, "Out" } };

/* Vectors.  Split and Combine are the pair that makes every other per-channel
 * operation expressible; without them a graph could not touch one channel of
 * anything. */
const Socket k_split[] = { { SK_INPUT,  DT_COLOR, "In" },
                           { SK_OUTPUT, DT_FLOAT, "R" },
                           { SK_OUTPUT, DT_FLOAT, "G" },
                           { SK_OUTPUT, DT_FLOAT, "B" },
                           { SK_OUTPUT, DT_FLOAT, "A" } };
const Socket k_combine[] = { { SK_INPUT, DT_FLOAT, "R" },
                             { SK_INPUT, DT_FLOAT, "G" },
                             { SK_INPUT, DT_FLOAT, "B" },
                             { SK_INPUT, DT_FLOAT, "A", "1.0" },
                             { SK_OUTPUT, DT_COLOR, "Out" } };
const Socket k_v3_2in_f[] = { { SK_INPUT, DT_VEC3, "A" },
                              { SK_INPUT, DT_VEC3, "B" },
                              { SK_OUTPUT, DT_FLOAT, "Out" } };
const Socket k_v3_2in_v[] = { { SK_INPUT, DT_VEC3, "A" },
                              { SK_INPUT, DT_VEC3, "B" },
                              { SK_OUTPUT, DT_VEC3, "Out" } };
const Socket k_v3_1in_v[] = { { SK_INPUT, DT_VEC3, "In" },
                              { SK_OUTPUT, DT_VEC3, "Out" } };
const Socket k_v3_1in_f[] = { { SK_INPUT, DT_VEC3, "In" },
                              { SK_OUTPUT, DT_FLOAT, "Out" } };

/* UV.  Centre defaults to the middle of the tile, which is what "rotate this"
 * means; (0,0) would rotate about a corner. */
const Socket k_uv_rot[] = { { SK_INPUT, DT_VEC2, "UV", "mat_uv" },
                            { SK_INPUT, DT_VEC2, "Centre", "vec2(0.5, 0.5)" },
                            { SK_INPUT, DT_FLOAT, "Turns", "0.0" },
                            { SK_OUTPUT, DT_VEC2, "Out" } };
const Socket k_uv_polar[] = { { SK_INPUT, DT_VEC2, "UV", "mat_uv" },
                              { SK_INPUT, DT_VEC2, "Centre", "vec2(0.5, 0.5)" },
                              { SK_OUTPUT, DT_VEC2, "Out" } };

/* Procedural.  Scale defaults to 4, not 0: a scale of zero collapses every
 * pattern to one constant, which reads as "the node does nothing". */
const Socket k_proc[] = { { SK_INPUT, DT_VEC2, "UV", "mat_uv" },
                          { SK_INPUT, DT_FLOAT, "Scale", "4.0" },
                          { SK_OUTPUT, DT_FLOAT, "Out" } };
/* Gradient runs along one uv axis; Axis picks it (< 0.5 = U, else V) rather
 * than this being two nodes. */
const Socket k_gradient[] = { { SK_INPUT, DT_VEC2, "UV", "mat_uv" },
                              { SK_INPUT, DT_FLOAT, "Axis", "0.0" },
                              { SK_OUTPUT, DT_FLOAT, "Out" } };

/* Colour utility. */
const Socket k_luma[] = { { SK_INPUT, DT_COLOR, "Color" },
                          { SK_OUTPUT, DT_FLOAT, "Out" } };
const Socket k_desat[] = { { SK_INPUT, DT_COLOR, "Color" },
                           { SK_INPUT, DT_FLOAT, "Amount", "1.0" },
                           { SK_OUTPUT, DT_COLOR, "Out" } };

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
    /* The sampler is chosen per node from Node::text (see sampler_for_node in
      * the codegen); the snippet's $SAMPLER is substituted there.  It used to
      * be the literal s_albedo, so two Texture nodes aimed at different maps
      * sampled the same one. */
    ENTRY(NT_TEXTURE,    "Texture (Material Map)", k_texture, "texture2D($SAMPLER, $0)"),
    ENTRY(NT_MUL_C,      "Multiply (Color)",  k_mul_c  , "($0 * $1)"),
    ENTRY(NT_MUL_F,      "Multiply (Float)",  k_mul_f  , "($0 * $1)"),
    ENTRY(NT_ADD_F,      "Add (Float)",       k_add_f  , "($0 + $1)"),
    ENTRY(NT_NORMAL_MAP, "Normal Map",        k_normal , "(texture2D(s_normalMap, $0).xyz * 2.0 - vec3_splat(1.0))"),
    ENTRY(NT_UV,         "UV (Tile/Offset)",  k_uv     , nullptr),   /* payload-driven */
    ENTRY(NT_SUB_F,      "Subtract (Float)",  k_sub_f  , "($0 - $1)"),
    ENTRY(NT_LERP_C,     "Lerp (Color)",      k_lerp_c , "mix($0, $1, vec4_splat($2))"),
    ENTRY(NT_FRESNEL,    "Fresnel",           k_fresnel,
          "pow(clamp(1.0 - abs(dot(normalize($0), normalize($1))), 0.0, 1.0), max($2, 0.001))"),

    /* ── Appended 2026-09-08 ─────────────────────────────────────────
     * Geometry: what jce_graph_material() already receives, exposed as nodes.
     * mat_world_pos / mat_normal_ws are its parameters; u_cameraPos comes
     * from fs_pbr_decl.sh and u_viewRect from bgfx_shader.sh. */
    ENTRY(NT_POSITION_WS,  "Position (World)",  k_g_vec3_out, "mat_world_pos"),
    ENTRY(NT_NORMAL_WS,    "Normal (World)",    k_g_vec3_out, "normalize(mat_normal_ws)"),
    ENTRY(NT_VIEW_DIR,     "View Direction",    k_g_vec3_out,
          "normalize(u_cameraPos.xyz - mat_world_pos)"),
    ENTRY(NT_SCREEN_UV,    "Screen UV",         k_g_vec2_out, "mat_screen_uv"),

    /* Float maths. */
    ENTRY(NT_DIV_F,        "Divide",            k_f_div,
          "($0 / (abs($1) > 1e-6 ? $1 : 1e-6))"),
    ENTRY(NT_POW_F,        "Power",             k_f_pow, "pow(max($0, 0.0), $1)"),
    ENTRY(NT_SQRT_F,       "Square Root",       k_f1, "sqrt(max($0, 0.0))"),
    ENTRY(NT_ABS_F,        "Absolute",          k_f1, "abs($0)"),
    ENTRY(NT_MIN_F,        "Minimum",           k_f2, "min($0, $1)"),
    ENTRY(NT_MAX_F,        "Maximum",           k_f2, "max($0, $1)"),
    ENTRY(NT_CLAMP_F,      "Clamp",             k_f_clamp, "clamp($0, $1, $2)"),
    ENTRY(NT_SATURATE_F,   "Saturate",          k_f1, "clamp($0, 0.0, 1.0)"),
    ENTRY(NT_ONE_MINUS_F,  "One Minus",         k_f1, "(1.0 - $0)"),
    ENTRY(NT_FRAC_F,       "Fraction",          k_f1, "fract($0)"),
    ENTRY(NT_FLOOR_F,      "Floor",             k_f1, "floor($0)"),
    ENTRY(NT_SIN_F,        "Sine",              k_f1, "sin($0)"),
    ENTRY(NT_COS_F,        "Cosine",            k_f1, "cos($0)"),
    ENTRY(NT_STEP_F,       "Step",              k_f_step, "step($0, $1)"),
    ENTRY(NT_SMOOTHSTEP_F, "Smoothstep",        k_f_smooth, "smoothstep($0, $1, $2)"),
    ENTRY(NT_LERP_F,       "Lerp (Float)",      k_f_lerp, "mix($0, $1, $2)"),
    /* The guard is not decoration: equal From ends are a division by zero, and
     * on a GPU that is a NaN that spreads silently through everything
     * downstream rather than an error anyone sees. */
    ENTRY(NT_REMAP_F,      "Remap",             k_f_remap,
          "($3 + ($4 - $3) * clamp(($0 - $1) / (abs($2 - $1) > 1e-6 ? ($2 - $1) : 1e-6), 0.0, 1.0))"),

    /* Colour maths. */
    ENTRY(NT_ADD_C,        "Add (Color)",       k_c2, "($0 + $1)"),
    ENTRY(NT_SUB_C,        "Subtract (Color)",  k_c2, "($0 - $1)"),
    ENTRY(NT_SCALE_C,      "Scale (Color)",     k_c_scale, "($0 * vec4_splat($1))"),

    /* Vectors.  Split emits one line per output socket and $O is the output
     * ordinal, so four channels read as four lines without four snippets. */
    ENTRY(NT_SPLIT,        "Split (RGBA)",      k_split, "($0)[$O]"),
    ENTRY(NT_COMBINE,      "Combine (RGBA)",    k_combine, "vec4($0, $1, $2, $3)"),
    ENTRY(NT_DOT3,         "Dot Product",       k_v3_2in_f, "dot($0, $1)"),
    ENTRY(NT_CROSS3,       "Cross Product",     k_v3_2in_v, "cross($0, $1)"),
    /* normalize(0) is NaN, and a node whose input is simply unconnected must
     * not poison the graph downstream of it. */
    ENTRY(NT_NORMALIZE3,   "Normalize",         k_v3_1in_v,
          "normalize($0 + vec3(0.0, 0.0, 1e-8))"),
    ENTRY(NT_LENGTH3,      "Length",            k_v3_1in_f, "length($0)"),
    ENTRY(NT_DISTANCE3,    "Distance",          k_v3_2in_f, "distance($0, $1)"),

    /* UV. */
    ENTRY(NT_UV_ROTATE,    "Rotate UV",         k_uv_rot, "jce_g_uv_rotate($0, $1, $2)"),
    ENTRY(NT_UV_POLAR,     "Polar UV",          k_uv_polar, "jce_g_uv_polar($0, $1)"),

    /* Procedural (engine/shaders/include/graph_nodes.sh). */
    ENTRY(NT_NOISE,        "Noise",             k_proc, "jce_g_noise($0, $1)"),
    ENTRY(NT_FBM,          "Noise (Fractal)",   k_proc, "jce_g_fbm($0, $1)"),
    ENTRY(NT_VORONOI,      "Voronoi",           k_proc, "jce_g_voronoi($0, $1)"),
    ENTRY(NT_CHECKER,      "Checker",           k_proc, "jce_g_checker($0, $1)"),
    ENTRY(NT_GRADIENT,     "Gradient",          k_gradient,
          "mix(($0).x, ($0).y, step(0.5, $1))"),

    /* Colour utility. */
    ENTRY(NT_LUMA,         "Luminance",         k_luma, "jce_g_luma(($0).rgb)"),
    ENTRY(NT_DESATURATE,   "Desaturate",        k_desat,
          "vec4(mix(($0).rgb, vec3_splat(jce_g_luma(($0).rgb)), clamp($1, 0.0, 1.0)), ($0).a)"),
};
const int k_table_count = (int)(sizeof(k_table) / sizeof(k_table[0]));

#undef ENTRY

/* User-addable types (excludes NT_OUTPUT). */
const NodeType k_addable[] = {
    /* Inputs */
    NT_COLOR, NT_FLOAT, NT_TEXTURE, NT_NORMAL_MAP, NT_UV,
    NT_POSITION_WS, NT_NORMAL_WS, NT_VIEW_DIR, NT_SCREEN_UV,
    /* Float maths */
    NT_MUL_F, NT_ADD_F, NT_SUB_F, NT_DIV_F, NT_POW_F, NT_SQRT_F, NT_ABS_F,
    NT_MIN_F, NT_MAX_F, NT_CLAMP_F, NT_SATURATE_F, NT_ONE_MINUS_F,
    NT_FRAC_F, NT_FLOOR_F, NT_SIN_F, NT_COS_F, NT_STEP_F, NT_SMOOTHSTEP_F,
    NT_LERP_F, NT_REMAP_F,
    /* Colour maths */
    NT_MUL_C, NT_ADD_C, NT_SUB_C, NT_SCALE_C, NT_LERP_C,
    /* Vectors */
    NT_SPLIT, NT_COMBINE, NT_DOT3, NT_CROSS3, NT_NORMALIZE3, NT_LENGTH3,
    NT_DISTANCE3,
    /* UV */
    NT_UV_ROTATE, NT_UV_POLAR,
    /* Procedural */
    NT_NOISE, NT_FBM, NT_VORONOI, NT_CHECKER, NT_GRADIENT,
    /* Colour utility */
    NT_LUMA, NT_DESATURATE, NT_FRESNEL,
};
const int k_addable_count = (int)(sizeof(k_addable) / sizeof(k_addable[0]));

} /* anonymous namespace */

void output_socket_default(int slot, float out_rgba[4])
{
    /* Chosen so a graph that drives nothing shades like an ordinary plain
     * surface rather than like a mistake: white base, non-metal, mid
     * roughness, no emission, identity tangent-space normal. */
    static const float k[5][4] = {
        { 1.0f, 1.0f, 1.0f, 1.0f },   /* 0 BaseColor */
        { 0.0f, 0.0f, 0.0f, 1.0f },   /* 1 Metallic  (.x) */
        { 0.5f, 0.0f, 0.0f, 1.0f },   /* 2 Roughness (.x) */
        { 0.0f, 0.0f, 0.0f, 1.0f },   /* 3 Emissive  (rgb) */
        { 0.0f, 0.0f, 1.0f, 0.0f },   /* 4 Normal    (xyz, tangent space) */
    };
    if (slot < 0 || slot > 4) {
        out_rgba[0] = out_rgba[1] = out_rgba[2] = out_rgba[3] = 1.0f;
        return;
    }
    for (int i = 0; i < 4; ++i) out_rgba[i] = k[slot][i];
}

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
