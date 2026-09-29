/*
 * jce_navmesh.c -- NavMesh runtime impl (grid + A*).
 */

#include <jce/middleware/ai/jce_navmesh.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_profiler.h>

#include "os/core/jce_memory.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct JceNavMesh {
    int       gx;
    int       gz;
    float     cell;
    float     origin_x;
    float     origin_z;
    uint8_t  *walkable;   /* row-major: idx = z * gx + x */
    /* The clearance this mesh was CARVED for, in metres, straight out of the
     * bake settings.  0 = a file written before this was read back, which is
     * "unknown" and not "zero": an unknown clearance must not reject anybody. */
    float     agent_height;
};

static JceNavMesh *load_root(JceJson *root)
{
    JceNavMesh *nm = JCE_NEW(JceNavMesh);
    if (!nm) return NULL;

    /* Settings (for fall-backs). */
    JceJson *cfg = jce_json_get(root, "settings");
    float def_cell = 0.30f;
    float def_min[3] = { 0.0f, 0.0f, 0.0f };
    float agent_h = 0.0f;
    if (cfg) {
        def_cell = (float)jce_json_get_number(cfg, "cellSize", 0.30);
        jce_json_get_floats(cfg, "boundsMin", def_min, 3, NULL);
        /* The bake has always written this; nothing read it back, so a mesh
         * carved for 1.8m of clearance accepted a 2.4m agent without comment. */
        agent_h = (float)jce_json_get_number(cfg, "agentHeight", 0.0);
    }

    JceJson *res = jce_json_get(root, "result");
    if (!res) {
        LOG_WARN("navmesh", "no 'result' block; load failed");
        JCE_FREE(nm);
        return NULL;
    }
    nm->gx       = jce_json_get_int(res, "gx", 0);
    nm->gz       = jce_json_get_int(res, "gz", 0);
    nm->cell     = (float)jce_json_get_number(res, "cell",    def_cell);
    nm->origin_x = (float)jce_json_get_number(res, "originX", def_min[0]);
    nm->origin_z = (float)jce_json_get_number(res, "originZ", def_min[2]);
    if (nm->gx <= 0 || nm->gz <= 0 || nm->cell <= 0.0001f) {
        LOG_WARN("navmesh", "bad grid dimensions");
        JCE_FREE(nm);
        return NULL;
    }

    size_t n = (size_t)nm->gx * (size_t)nm->gz;
    nm->walkable = (uint8_t *)JCE_CALLOC(1, n);
    if (!nm->walkable) { JCE_FREE(nm); return NULL; }
    const char *bits = jce_json_get_string(res, "bits", "");
    size_t bn = bits ? strlen(bits) : 0;
    if (bn > n) bn = n;
    for (size_t i = 0; i < bn; ++i)
        nm->walkable[i] = (bits[i] == '1') ? 1 : 0;
    nm->agent_height = (agent_h > 0.0f) ? agent_h : 0.0f;
    return nm;
}

JceNavMesh *jce_navmesh_load_text(const char *text, size_t len)
{
    if (!text || len == 0) return NULL;
    JceJson *root = jce_json_parse(text, len);
    if (!root) return NULL;
    JceNavMesh *nm = load_root(root);
    jce_json_free(root);
    return nm;
}

JceNavMesh *jce_navmesh_load_file(const char *path)
{
    if (!path) return NULL;
    JceJson *root = jce_json_parse_file(path);
    if (!root) {
        LOG_WARN("navmesh", "parse failed: %s", path);
        return NULL;
    }
    JceNavMesh *nm = load_root(root);
    jce_json_free(root);
    return nm;
}

void jce_navmesh_free(JceNavMesh *nm)
{
    if (!nm) return;
    JCE_FREE(nm->walkable);
    JCE_FREE(nm);
}

int   jce_navmesh_grid_x(const JceNavMesh *nm) { return nm ? nm->gx : 0; }
int   jce_navmesh_grid_z(const JceNavMesh *nm) { return nm ? nm->gz : 0; }
float jce_navmesh_agent_height(const JceNavMesh *nm)
{
    return nm ? nm->agent_height : 0.0f;
}

float jce_navmesh_cell  (const JceNavMesh *nm) { return nm ? nm->cell : 0.0f; }

void jce_navmesh_origin(const JceNavMesh *nm, float *out_x, float *out_z)
{
    if (out_x) *out_x = nm ? nm->origin_x : 0.0f;
    if (out_z) *out_z = nm ? nm->origin_z : 0.0f;
}

static bool world_to_cell(const JceNavMesh *nm, float wx, float wz, int *cx, int *cz)
{
    if (!nm) return false;
    float fx = (wx - nm->origin_x) / nm->cell;
    float fz = (wz - nm->origin_z) / nm->cell;
    int ix = (int)floorf(fx);
    int iz = (int)floorf(fz);
    if (ix < 0 || iz < 0 || ix >= nm->gx || iz >= nm->gz) return false;
    *cx = ix; *cz = iz;
    return true;
}

bool jce_navmesh_is_walkable(const JceNavMesh *nm, float wx, float wz)
{
    int cx, cz;
    if (!world_to_cell(nm, wx, wz, &cx, &cz)) return false;
    return nm->walkable[(size_t)cz * nm->gx + cx] != 0;
}

/* ───── A* on the grid (8-connected) ──────────────────────────── */

typedef struct { int x, z; float f; } AStarOpen;

static void heap_push(AStarOpen *heap, int *count, AStarOpen v)
{
    int i = (*count)++;
    heap[i] = v;
    while (i > 0) {
        int p = (i - 1) / 2;
        if (heap[p].f <= heap[i].f) break;
        AStarOpen t = heap[p]; heap[p] = heap[i]; heap[i] = t;
        i = p;
    }
}

static AStarOpen heap_pop(AStarOpen *heap, int *count)
{
    AStarOpen top = heap[0];
    --(*count);
    if (*count > 0) {
        heap[0] = heap[*count];
        int i = 0;
        for (;;) {
            int l = i * 2 + 1, r = i * 2 + 2, m = i;
            if (l < *count && heap[l].f < heap[m].f) m = l;
            if (r < *count && heap[r].f < heap[m].f) m = r;
            if (m == i) break;
            AStarOpen t = heap[m]; heap[m] = heap[i]; heap[i] = t;
            i = m;
        }
    }
    return top;
}

static float heuristic(int ax, int az, int bx, int bz)
{
    float dx = (float)(ax - bx), dz = (float)(az - bz);
    return sqrtf(dx * dx + dz * dz);
}

/* rank-6: persistent A* scratch (Recast/Detour dtNodePool model).  The old path
 * did 4 CALLOC (gscore/parent/closed/open) + an O(cells) memset + a 5th CALLOC
 * (trail) per query, then freed all five — ~1.3 MB malloc+memset+free for a
 * 256x256 grid PER FindPath, thrashing the allocator under crowd replanning.
 * Instead keep the buffers resident (grown once to the largest grid seen) and
 * reset per query with a generation token: a cell is "unseen" iff its stamp !=
 * the current token, so the per-query reset is O(1) (a token bump) instead of
 * O(cells) memset.  Single-threaded: the sole caller (jce_nav_agent) updates
 * agents serially; parallel pathfinding would need one scratch per thread. */
static float     *s_as_gscore;
static int       *s_as_parent;
static AStarOpen *s_as_open;
static int       *s_as_trail;
static uint32_t  *s_as_seen;     /* gscore/parent valid this query iff == s_as_token */
static uint32_t  *s_as_closed;   /* closed this query iff == s_as_token            */
static size_t     s_as_cap;      /* capacity in cells                              */
static uint32_t   s_as_token;    /* per-query generation                           */

static bool astar_scratch_ensure(size_t cells)
{
    if (cells <= s_as_cap && s_as_gscore) return true;
    float     *ng = (float *)JCE_REALLOC(s_as_gscore, cells * sizeof(float));
    int       *np = (int   *)JCE_REALLOC(s_as_parent, cells * sizeof(int));
    AStarOpen *no = (AStarOpen *)JCE_REALLOC(s_as_open, cells * sizeof(AStarOpen));
    int       *nt = (int   *)JCE_REALLOC(s_as_trail, cells * sizeof(int));
    uint32_t  *ns = (uint32_t *)JCE_REALLOC(s_as_seen,   cells * sizeof(uint32_t));
    uint32_t  *nc = (uint32_t *)JCE_REALLOC(s_as_closed, cells * sizeof(uint32_t));
    if (ng) s_as_gscore = ng;
    if (np) s_as_parent = np;
    if (no) s_as_open   = no;
    if (nt) s_as_trail  = nt;
    if (ns) s_as_seen   = ns;
    if (nc) s_as_closed = nc;
    if (!ng || !np || !no || !nt || !ns || !nc) return false;
    /* The grown stamp region must read as "older than any live token" — zero
     * both stamp arrays and reset the token so no stale value aliases a mark. */
    memset(s_as_seen,   0, cells * sizeof(uint32_t));
    memset(s_as_closed, 0, cells * sizeof(uint32_t));
    s_as_token = 0;
    s_as_cap   = cells;
    return true;
}

int jce_navmesh_find_path(const JceNavMesh *nm,
                          float sx, float sz, float gx, float gz,
                          float *out_xz, int max_points)
{
    if (!nm || !out_xz || max_points <= 0) return 0;
    int s_cx, s_cz, g_cx, g_cz;
    if (!world_to_cell(nm, sx, sz, &s_cx, &s_cz)) return 0;
    if (!world_to_cell(nm, gx, gz, &g_cx, &g_cz)) return 0;
    if (!nm->walkable[(size_t)s_cz * nm->gx + s_cx]) return 0;
    if (!nm->walkable[(size_t)g_cz * nm->gx + g_cx]) return 0;
    if (s_cx == g_cx && s_cz == g_cz) {
        out_xz[0] = gx; out_xz[1] = gz;
        return 1;
    }

    JCE_PROFILE_ZONE_N("NavMesh::FindPath");

    size_t cells = (size_t)nm->gx * (size_t)nm->gz;
    if (!astar_scratch_ensure(cells)) { JCE_PROFILE_ZONE_END; return 0; }
    /* New query generation = O(1) reset of gscore/parent/closed.  On token wrap,
     * clear the stamps once so a stale mark can't alias the fresh token. */
    if (++s_as_token == 0) {
        memset(s_as_seen,   0, s_as_cap * sizeof(uint32_t));
        memset(s_as_closed, 0, s_as_cap * sizeof(uint32_t));
        s_as_token = 1;
    }
    const uint32_t tok = s_as_token;
    float     *gscore = s_as_gscore;
    int       *parent = s_as_parent;
    AStarOpen *open   = s_as_open;
    int open_count = 0;

    int s_idx = s_cz * nm->gx + s_cx;
    int g_idx = g_cz * nm->gx + g_cx;
    gscore[s_idx] = 0.0f; parent[s_idx] = -1; s_as_seen[s_idx] = tok;
    AStarOpen start = { s_cx, s_cz, heuristic(s_cx, s_cz, g_cx, g_cz) };
    heap_push(open, &open_count, start);

    static const int dx[8] = { 1, -1, 0,  0, 1, 1, -1, -1 };
    static const int dz[8] = { 0,  0, 1, -1, 1, -1, 1, -1 };
    bool found = false;
    while (open_count > 0) {
        AStarOpen cur = heap_pop(open, &open_count);
        int idx = cur.z * nm->gx + cur.x;
        if (s_as_closed[idx] == tok) continue;        /* already expanded */
        s_as_closed[idx] = tok;
        if (idx == g_idx) { found = true; break; }
        for (int k = 0; k < 8; ++k) {
            int nx = cur.x + dx[k], nz = cur.z + dz[k];
            if (nx < 0 || nz < 0 || nx >= nm->gx || nz >= nm->gz) continue;
            int nidx = nz * nm->gx + nx;
            if (!nm->walkable[nidx] || s_as_closed[nidx] == tok) continue;
            float step = (k < 4) ? 1.0f : 1.41421356f;
            float tentative = gscore[idx] + step;     /* idx is closed => seen => valid */
            /* Unseen cells read as 1e30 (the old explicit init); seen ones use the
             * stored gscore — identical A* relaxation, no per-query memset. */
            float ngs = (s_as_seen[nidx] == tok) ? gscore[nidx] : 1e30f;
            if (tentative < ngs) {
                gscore[nidx] = tentative;
                parent[nidx] = idx;
                s_as_seen[nidx] = tok;
                AStarOpen nxt = { nx, nz, tentative + heuristic(nx, nz, g_cx, g_cz) };
                heap_push(open, &open_count, nxt);
            }
        }
    }

    int written = 0;
    if (found) {
        /* Reconstruct (in reverse) into the persistent trail, then emit forwards.
         * Every cell on the chain was stamped during the search, so parent[] is
         * valid along it; the start's parent is -1 and terminates the walk. */
        int *trail = s_as_trail;
        int  trail_n = 0;
        int  idx = g_idx;
        while (idx >= 0 && trail_n < (int)cells) {
            trail[trail_n++] = idx;
            idx = parent[idx];
        }
        for (int i = trail_n - 1; i >= 0 && written < max_points; --i) {
            int cidx = trail[i];
            int cx = cidx % nm->gx;
            int cz = cidx / nm->gx;
            float wx = nm->origin_x + (cx + 0.5f) * nm->cell;
            float wz = nm->origin_z + (cz + 0.5f) * nm->cell;
            out_xz[written * 2 + 0] = wx;
            out_xz[written * 2 + 1] = wz;
            ++written;
        }
        /* Replace last point with exact goal. */
        if (written > 0) {
            out_xz[(written - 1) * 2 + 0] = gx;
            out_xz[(written - 1) * 2 + 1] = gz;
        }
    }

    JCE_PROFILE_ZONE_END;
    return written;
}
