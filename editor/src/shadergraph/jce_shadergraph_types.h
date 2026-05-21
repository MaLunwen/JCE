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
    /* Reserve 12..31 for Phase B/C additions. */
    NT_COUNT_HINT = 32  /* sizing hint only; not a real type */
};

enum SocketKind {
    SK_INPUT  = 0,
    SK_OUTPUT = 1
};

/* Data types.  DT_FLOAT/DT_COLOR are the legacy pair driving current
 * panel behaviour.  Extended types are reserved for Phase C codegen
 * and must NOT appear in any current node's socket table (otherwise
 * connections would silently mismatch). */
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
