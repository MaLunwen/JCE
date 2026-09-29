/*
 * jce_shadergraph_types.h  —  Phase A shader-graph data model.
 *
 * Pure data structures with **zero ImGui / bgfx dependency** so future
 * phases (codegen, shaderc invocation, headless tests) can reuse them.
 *
 * UI-only state (scroll/selection/box-select/pending link) lives on
 * `JceShaderGraph` for now but is *not* serialised — io layer ignores it.
 * A later refactor may extract it into a separate `JceShaderGraphEditState`.
 *
 *   Phase A  : extract this file from jce_panel_material_graph.cpp.
 *   Phase B  : UI may move to imnodes — model unchanged.
 *   Phase C  : DT_VEC2/VEC3/VEC4/SAMPLER2D used by codegen.
 *   Phase D  : registry.glsl_snippet drives shader source emission.
 */

#pragma once

#include <cstdint>
#include <set>
#include <vector>

namespace jce_sg {

/* ---------------- enums ---------------- */

/* Node types.  Existing values (0..11) match the legacy NodeType enum
 * persisted in .matgraph.json — DO NOT renumber. New nodes append. */
enum NodeType {
    NT_OUTPUT     = 0,
    NT_COLOR      = 1,
    NT_FLOAT      = 2,
    NT_TEXTURE    = 3,
    NT_MUL_C      = 4,
    NT_MUL_F      = 5,
    NT_ADD_F      = 6,
    NT_NORMAL_MAP = 7,
    NT_UV         = 8,
    NT_SUB_F      = 9,
    NT_LERP_C     = 10,
    NT_FRESNEL    = 11,

    /* ── Appended 2026-09-08.  APPEND ONLY: the value is what a .matgraph.json
     * stores, so renumbering silently turns every saved graph into a different
     * graph.  Grouped by what they are, numbered by when they arrived. */

    /* Geometry the fragment already has.  These read the arguments the graph
     * material function is handed, so they cost nothing to add and cannot be
     * out of sync with the surface being shaded. */
    NT_POSITION_WS  = 12,   /* world position                     -> vec3  */
    NT_NORMAL_WS    = 13,   /* interpolated world normal          -> vec3  */
    NT_VIEW_DIR     = 14,   /* unit vector surface -> camera      -> vec3  */
    NT_SCREEN_UV    = 15,   /* fragment position / view rect      -> vec2  */

    /* Float maths.  The four that existed (mul/add/sub and lerp on colour)
     * left a graph unable to express saturate, a power, a step or a remap --
     * which is most of what a material actually does between its inputs. */
    NT_DIV_F        = 16,
    NT_POW_F        = 17,
    NT_SQRT_F       = 18,
    NT_ABS_F        = 19,
    NT_MIN_F        = 20,
    NT_MAX_F        = 21,
    NT_CLAMP_F      = 22,
    NT_SATURATE_F   = 23,
    NT_ONE_MINUS_F  = 24,
    NT_FRAC_F       = 25,
    NT_FLOOR_F      = 26,
    NT_SIN_F        = 27,
    NT_COS_F        = 28,
    NT_STEP_F       = 29,
    NT_SMOOTHSTEP_F = 30,
    NT_LERP_F       = 31,
    NT_REMAP_F      = 32,

    /* Colour maths beside the two that existed. */
    NT_ADD_C        = 33,
    NT_SUB_C        = 34,
    NT_SCALE_C      = 35,   /* vec4 * float */

    /* Vectors.  Split and Combine are the pair that makes every other channel
     * operation expressible; without them a graph could not touch one channel
     * of anything. */
    NT_SPLIT        = 36,   /* vec4 -> R, G, B, A */
    NT_COMBINE      = 37,   /* R, G, B, A -> vec4 */
    NT_DOT3         = 38,
    NT_CROSS3       = 39,
    NT_NORMALIZE3   = 40,
    NT_LENGTH3      = 41,
    NT_DISTANCE3    = 42,

    /* UV. */
    NT_UV_ROTATE    = 43,
    NT_UV_POLAR     = 44,

    /* Procedural, backed by engine/shaders/include/graph_nodes.sh. */
    NT_NOISE        = 45,
    NT_FBM          = 46,
    NT_VORONOI      = 47,
    NT_CHECKER      = 48,
    NT_GRADIENT     = 49,   /* linear ramp along a uv axis */

    /* Colour utility. */
    NT_LUMA         = 50,
    NT_DESATURATE   = 51,

    NT_COUNT_HINT   = 96  /* sizing hint only; not a real type */
};

enum SocketKind {
    SK_INPUT  = 0,
    SK_OUTPUT = 1
};

/* Data types.
 *
 * DT_FLOAT/DT_COLOR were the legacy pair.  DT_VEC2 and DT_VEC3 are now in
 * live socket tables (UV coordinates, tangent- and world-space normals,
 * positions); the codegen has emitted the right GLSL type for them all along.
 * DT_VEC4 is DT_COLOR under another name and the two connect freely -- they
 * compile to the same type, and refusing the link would be a rule about
 * vocabulary rather than about types.  DT_SAMPLER2D and DT_NORMAL remain
 * reserved: no socket declares them. */
enum DataType {
    DT_FLOAT     = 0,
    DT_COLOR     = 1,  /* == legacy "color" = RGBA */
    DT_VEC2      = 2,  /* reserved Phase C */
    DT_VEC3      = 3,  /* reserved Phase C */
    DT_VEC4      = 4,  /* reserved Phase C (alias of DT_COLOR semantics) */
    DT_SAMPLER2D = 5,  /* reserved Phase C */
    DT_NORMAL    = 6   /* reserved Phase C (alias of DT_VEC3) */
};

/* ---------------- structs ---------------- */

/* Tiny POD vec used for node positions / scroll / box-select corners.
 * Distinct from ImVec2 so this header stays ImGui-free; trivially
 * convertible via brace-init at the UI boundary. */
struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct Socket {
    SocketKind  kind;
    DataType    dtype;
    const char *name;
    /* GLSL for this INPUT socket when nothing is connected to it.  NULL means
     * the dtype's zero.
     *
     * Zero is the wrong answer more often than it looks.  An unconnected UV
     * means "this fragment's UV", not vec2(0,0) -- a texture node with zero
     * UV samples one texel and reads as a broken texture rather than as a
     * missing wire.  That case was already special-cased by DTYPE in the
     * codegen, which only worked because every vec2 socket happened to be a
     * UV; a Rotate UV node's Centre is a vec2 and means (0.5,0.5).  Likewise
     * a Fresnel with nothing plugged in should use the surface's own normal
     * and view direction, and a procedural node's Scale of 0 is a constant
     * pattern.  Per socket, because that is where the answer lives. */
    const char *default_expr = nullptr;
};

struct Node {
    int      id = 0;
    NodeType type = NT_FLOAT;
    Vec2     pos  = { 0.0f, 0.0f };
    /* Constant payloads (only one is meaningful per type). */
    float    color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float    scalar   = 0.0f;
    char     text[256] = { 0 };   /* texture path / future identifier */
};

struct Link {
    int from_node = 0;
    int from_sock = 0;
    int to_node   = 0;
    int to_sock   = 0;
};

/* The graph itself.
 *
 * Persistent (serialised) fields:  nodes, links, next_id, path
 * Runtime / UI fields (NOT serialised, may be reset on load):
 *   pending_from_*, scroll, selected, box_*
 */
struct Graph {
    std::vector<Node> nodes;
    std::vector<Link> links;
    int  next_id = 1;
    char path[260] = { 0 };

    /* Pending link source (-1 = none). */
    int  pending_from_node = -1;
    int  pending_from_sock = -1;

    /* Canvas scroll offset in screen-space pixels. */
    Vec2 scroll = { 0.0f, 0.0f };

    /* Selection set + box-select rubber band. */
    std::set<int> selected;
    bool box_active = false;
    Vec2 box_start  = { 0.0f, 0.0f };
};

} /* namespace jce_sg */
