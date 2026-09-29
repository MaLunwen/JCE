/*
 * graph_nodes.sh — helper functions for Shader Graph node snippets.
 *
 * A node's `glsl_snippet` in jce_shadergraph_registry.cpp is ONE expression,
 * substituted inline.  Anything a node needs that is not expressible as one
 * expression -- a loop, a hash, a lookup -- lives here as a function the
 * snippet calls.
 *
 * WHY A FILE AND NOT MORE SNIPPET.  A procedural node inlined as a giant
 * expression is unreadable in the Shader Inspector, which shows the generated
 * source, and it is duplicated once per use.  A function is emitted once and
 * reads as what it is.
 *
 * Everything here is prefixed jce_g_ so a generated variable (n<id>_o<n>) or a
 * user's own name can never collide with it.
 *
 * PORTABILITY.  Written against the GLSL 1.20 floor the desktop GL backend
 * compiles at: no integer bitwise ops, no textureLod in the fragment stage, no
 * derivatives assumed.  The hash is float-only for that reason -- the usual
 * uint-xor-shift hash does not compile on that profile, and finding that out
 * from a shipped project's build log is the wrong place to find it out.
 */
#ifndef JCE_GRAPH_NODES_SH
#define JCE_GRAPH_NODES_SH

/* ── hashing ──────────────────────────────────────────────────────────
 * Sine-based, because the integer path is not available at the GL floor.
 * The large constants are the usual ones; they are arbitrary but must stay
 * fixed, since changing them changes every noise pattern already authored. */
float jce_g_hash11(float p)
{
    return fract(sin(p * 127.1) * 43758.5453123);
}

float jce_g_hash21(vec2 p)
{
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}

vec2 jce_g_hash22(vec2 p)
{
    vec2 q = vec2(dot(p, vec2(127.1, 311.7)), dot(p, vec2(269.5, 183.3)));
    return fract(sin(q) * 43758.5453123);
}

/* ── value noise ─────────────────────────────────────────────────────
 * Bilinear interpolation of a hashed lattice with the smoothstep fade.
 * Range [0,1]. */
float jce_g_noise(vec2 uv, float scale)
{
    vec2 p = uv * scale;
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 w = f * f * (vec2_splat(3.0) - 2.0 * f);

    float a = jce_g_hash21(i + vec2(0.0, 0.0));
    float b = jce_g_hash21(i + vec2(1.0, 0.0));
    float c = jce_g_hash21(i + vec2(0.0, 1.0));
    float d = jce_g_hash21(i + vec2(1.0, 1.0));

    return mix(mix(a, b, w.x), mix(c, d, w.x), w.y);
}

/* Fractal sum of the above.  Four octaves fixed: the count has to be a
 * compile-time constant on the GL floor, and four is where the added detail
 * stops being visible at the texel densities a material is authored at. */
float jce_g_fbm(vec2 uv, float scale)
{
    float sum = 0.0;
    float amp = 0.5;
    vec2  p   = uv * scale;
    for (int o = 0; o < 4; ++o) {
        sum += amp * jce_g_noise(p, 1.0);
        p   *= 2.0;
        amp *= 0.5;
    }
    return sum;
}

/* ── voronoi ─────────────────────────────────────────────────────────
 * Distance to the nearest of one feature point per cell, over the 3x3
 * neighbourhood.  Returns the distance, not the cell id: the distance is what
 * a material uses (cracks, scales, cells) and the id needs an integer output
 * socket the graph does not have. */
float jce_g_voronoi(vec2 uv, float scale)
{
    vec2 p = uv * scale;
    vec2 i = floor(p);
    vec2 f = fract(p);
    float best = 8.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 g = vec2(float(x), float(y));
            vec2 o = jce_g_hash22(i + g);
            vec2 r = g + o - f;
            best = min(best, dot(r, r));
        }
    }
    return sqrt(best);
}

/* ── patterns ────────────────────────────────────────────────────────── */

/* Checker, 0 or 1.  mod(floor(u)+floor(v), 2) without the integer ops. */
float jce_g_checker(vec2 uv, float scale)
{
    vec2 p = floor(uv * scale);
    return mod(p.x + p.y, 2.0);
}

/* ── uv ──────────────────────────────────────────────────────────────── */

/* Rotate uv about a centre, angle in TURNS (0..1) rather than radians: a
 * material author types 0.25 for a quarter turn far more often than 1.5708. */
vec2 jce_g_uv_rotate(vec2 uv, vec2 centre, float turns)
{
    float a = turns * 6.28318530718;
    float s = sin(a);
    float c = cos(a);
    vec2  d = uv - centre;
    return vec2(d.x * c - d.y * s, d.x * s + d.y * c) + centre;
}

/* Polar coordinates about a centre: x = radius, y = angle in turns [0,1). */
vec2 jce_g_uv_polar(vec2 uv, vec2 centre)
{
    vec2  d = uv - centre;
    float r = length(d);
    float a = atan2(d.y, d.x) * 0.15915494309;   /* /(2pi) */
    return vec2(r, fract(a + 1.0));
}

/* ── colour ──────────────────────────────────────────────────────────── */

/* Rec.709 luma, the same weights the engine's own tonemap path uses. */
float jce_g_luma(vec3 c)
{
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

#endif /* JCE_GRAPH_NODES_SH */
