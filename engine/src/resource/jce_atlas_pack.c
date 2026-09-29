/*
 * jce_atlas_pack.c — skyline bottom-left packing, deterministic.
 *
 * Skyline rather than MaxRects: MaxRects packs a few percent tighter and
 * needs a free-rectangle list that grows and is pruned, which is more state
 * to get wrong for a gain that does not change how many atlases a project
 * ships.  Skyline is a run of (x, y, width) segments and a linear scan.
 *
 * Everything here is arithmetic on sizes.  No image data, no allocation, no
 * I/O — see the header for why that is the point rather than a limitation.
 */

#include <jce/resource/jce_atlas_pack.h>

#include <jce/os/core/jce_filesystem.h>

#include "os/core/jce_memory.h"

#include <stdio.h>   /* snprintf into the buffer, not to a file */
#include <string.h>

enum { ATLAS_MAX_ITEMS = 4096 };
enum { ATLAS_MAX_NODES = ATLAS_MAX_ITEMS + 2 };

typedef struct { uint32_t x, y, w; } SkylineNode;

JCE_API JceAtlasPackDesc jce_atlas_pack_desc_default(void)
{
    JceAtlasPackDesc d;
    d.max_width    = 4096u;
    d.max_height   = 4096u;
    d.padding      = 2u;
    d.power_of_two = false;
    return d;
}

/* Sort key: TALLEST first, then widest, then id.
 *
 * Height-first is what makes a skyline pack well — a short sprite placed
 * early leaves a low shelf that every later tall one has to step over.  The
 * id tiebreak is not cosmetic: without it the result depends on the caller's
 * input order, so a directory listing that enumerates differently would
 * reshuffle the atlas and rewrite every sprite's UVs on a cook that changed
 * nothing.  This tree gates reproducible builds. */
static int atlas_before(const JceAtlasItem *a, const JceAtlasItem *b)
{
    if (a->h != b->h) return a->h > b->h;
    if (a->w != b->w) return a->w > b->w;
    return a->id < b->id;
}

/* Lowest y at which [x, x+w) is clear, or UINT32_MAX if it runs off the end. */
static uint32_t skyline_fit(const SkylineNode *nodes, int n, int first,
                            uint32_t w, uint32_t limit_w)
{
    uint32_t x = nodes[first].x;
    if (x + w > limit_w) return UINT32_MAX;

    uint32_t y = 0u;
    uint32_t left = w;
    for (int i = first; i < n && left > 0u; ++i) {
        if (nodes[i].y > y) y = nodes[i].y;
        left = (nodes[i].w >= left) ? 0u : (left - nodes[i].w);
    }
    if (left > 0u) return UINT32_MAX;   /* ran past the last segment */
    return y;
}

static void skyline_add(SkylineNode *nodes, int *n, uint32_t x, uint32_t y,
                        uint32_t w)
{
    /* Insert the new segment, then trim whatever it covers.  Written as
     * shift-insert + shrink rather than a rebuild because the array is small
     * and a rebuild needs a second buffer this function must not allocate. */
    if (*n >= ATLAS_MAX_NODES) return;

    int i = 0;
    while (i < *n && nodes[i].x < x) ++i;
    memmove(&nodes[i + 1], &nodes[i], (size_t)(*n - i) * sizeof(SkylineNode));
    nodes[i].x = x; nodes[i].y = y; nodes[i].w = w;
    ++(*n);

    /* Shrink or drop the segments this one covers. */
    for (int j = i + 1; j < *n; ) {
        if (nodes[j].x < nodes[j - 1].x + nodes[j - 1].w) {
            uint32_t shrink = nodes[j - 1].x + nodes[j - 1].w - nodes[j].x;
            if (nodes[j].w <= shrink) {
                memmove(&nodes[j], &nodes[j + 1],
                        (size_t)(*n - j - 1) * sizeof(SkylineNode));
                --(*n);
                continue;
            }
            nodes[j].x += shrink;
            nodes[j].w -= shrink;
        }
        break;
    }

    /* Merge equal-height neighbours, or the segment list grows without bound
     * on a run of same-height sprites and eventually hits ATLAS_MAX_NODES on
     * an atlas that had plenty of room. */
    for (int j = 0; j < *n - 1; ) {
        if (nodes[j].y == nodes[j + 1].y) {
            nodes[j].w += nodes[j + 1].w;
            memmove(&nodes[j + 1], &nodes[j + 2],
                    (size_t)(*n - j - 2) * sizeof(SkylineNode));
            --(*n);
            continue;
        }
        ++j;
    }
}

static uint32_t next_pow2(uint32_t v)
{
    uint32_t p = 1u;
    while (p < v) p <<= 1;
    return p;
}

JCE_API size_t jce_atlas_pack(const JceAtlasItem      *items,
                              size_t                   count,
                              const JceAtlasPackDesc  *desc,
                              JceAtlasPlacement       *out,
                              uint32_t                *out_w,
                              uint32_t                *out_h)
{
    if (out_w) *out_w = 0u;
    if (out_h) *out_h = 0u;
    if (!items || !out || count == 0u) return 0u;
    if (count > ATLAS_MAX_ITEMS) count = ATLAS_MAX_ITEMS;

    JceAtlasPackDesc d = desc ? *desc : jce_atlas_pack_desc_default();
    if (d.max_width  == 0u) d.max_width  = 4096u;
    if (d.max_height == 0u) d.max_height = 4096u;

    /* Order to try, by index into `items` — the caller's array is const and
     * `out` is written in the caller's order, so the sort moves indices. */
    uint32_t order[ATLAS_MAX_ITEMS];
    for (size_t i = 0; i < count; ++i) order[i] = (uint32_t)i;
    for (size_t i = 1; i < count; ++i) {          /* insertion sort: n <= 4096
                                                     and it is stable, which
                                                     the determinism argument
                                                     depends on */
        uint32_t key = order[i];
        size_t   j   = i;
        while (j > 0 && atlas_before(&items[key], &items[order[j - 1]])) {
            order[j] = order[j - 1];
            --j;
        }
        order[j] = key;
    }

    for (size_t i = 0; i < count; ++i) {
        out[i].id     = items[i].id;
        out[i].x      = 0u;
        out[i].y      = 0u;
        out[i].w      = items[i].w;
        out[i].h      = items[i].h;
        out[i].placed = false;
    }

    SkylineNode nodes[ATLAS_MAX_NODES];
    int         node_count = 1;
    nodes[0].x = 0u; nodes[0].y = 0u; nodes[0].w = d.max_width;

    const uint32_t pad = d.padding;
    uint32_t used_w = 0u, used_h = 0u;
    size_t   placed = 0u;

    for (size_t k = 0; k < count; ++k) {
        const JceAtlasItem *it = &items[order[k]];
        if (it->w == 0u || it->h == 0u) continue;

        /* Padding on the right and bottom of every sprite, and the atlas edge
         * gets it too because the first sprite starts at (pad, pad). */
        const uint32_t need_w = it->w + pad;
        const uint32_t need_h = it->h + pad;
        if (need_w > d.max_width || need_h > d.max_height) continue;

        uint32_t best_y = UINT32_MAX, best_x = 0u;
        int      best_i = -1;
        for (int i = 0; i < node_count; ++i) {
            uint32_t y = skyline_fit(nodes, node_count, i, need_w, d.max_width);
            if (y == UINT32_MAX) continue;
            if (y + need_h > d.max_height) continue;
            /* Bottom-left: lowest y wins, leftmost breaks the tie.  Both
             * halves matter -- lowest alone leaves the choice to segment
             * order, which is not a property the caller can reason about. */
            if (y < best_y || (y == best_y && nodes[i].x < best_x)) {
                best_y = y;
                best_x = nodes[i].x;
                best_i = i;
            }
        }
        if (best_i < 0) continue;

        const size_t oi = order[k];
        out[oi].x      = best_x + pad;
        out[oi].y      = best_y + pad;
        out[oi].placed = true;
        ++placed;

        skyline_add(nodes, &node_count, best_x, best_y + need_h, need_w);

        if (out[oi].x + it->w + pad > used_w) used_w = out[oi].x + it->w + pad;
        if (out[oi].y + it->h + pad > used_h) used_h = out[oi].y + it->h + pad;
    }

    if (d.power_of_two) {
        used_w = next_pow2(used_w);
        used_h = next_pow2(used_h);
        if (used_w > d.max_width)  used_w = d.max_width;
        if (used_h > d.max_height) used_h = d.max_height;
    }
    if (out_w) *out_w = used_w;
    if (out_h) *out_h = used_h;
    return placed;
}

/* Builds the whole document in memory and hands it to jce_fs_host_write_all.
 *
 * NOT fopen/fprintf, and the gate is right to insist: engine code goes through
 * jce_fs_* so that one implementation decides encoding, path handling and the
 * platform layer.  check_engine_native_io.py caught the first version of this
 * function doing it the other way. */
JCE_API bool jce_atlas_write_aseprite_json(const char              *path,
                                           const char *const       *names,
                                           const JceAtlasPlacement *pl,
                                           size_t                   count,
                                           uint32_t                 atlas_w,
                                           uint32_t                 atlas_h,
                                           const char              *image_name)
{
    if (!path || !names || !pl) return false;

    /* A frame line is under 160 chars and a name is capped by the caller's
     * own buffer; 512 per entry plus a fixed header is generous and bounded.
     * Everything below checks `left` before writing, so a name long enough to
     * beat that estimate truncates the document rather than the heap -- and a
     * truncated JSON fails to parse, which is loud. */
    const size_t cap = 1024u + count * 512u;
    char *buf = (char *)JCE_MALLOC(cap);
    if (!buf) return false;

    size_t off = 0u;
#define ATLAS_APPEND(...)                                                         do {                                                                              if (off >= cap) { JCE_FREE(buf); return false; }                                  int _n = snprintf(buf + off, cap - off, __VA_ARGS__);                         if (_n < 0 || (size_t)_n >= cap - off) { JCE_FREE(buf); return false; }           off += (size_t)_n;                                                        } while (0)

    ATLAS_APPEND("{\n  \"frames\": {\n");
    bool first = true;
    for (size_t i = 0; i < count; ++i) {
        if (!pl[i].placed || !names[i]) continue;
        if (!first) ATLAS_APPEND(",\n");
        first = false;
        ATLAS_APPEND("    \"%s\": { \"frame\": { "
                     "\"x\": %u, \"y\": %u, "
                     "\"w\": %u, \"h\": %u }, "
                     "\"duration\": 100 }",
                     names[i], pl[i].x, pl[i].y, pl[i].w, pl[i].h);
    }
    ATLAS_APPEND("\n  },\n  \"meta\": {\n");
    ATLAS_APPEND("    \"app\": \"jce_atlas_pack\",\n");
    ATLAS_APPEND("    \"image\": \"%s\",\n",
                 image_name ? image_name : "");
    ATLAS_APPEND("    \"size\": { \"w\": %u, "
                 "\"h\": %u },\n", atlas_w, atlas_h);
    /* No frameTags.  The packer has no idea which files form an animation,
     * and inventing tags from file-name prefixes guesses wrong the first time
     * somebody names a sprite "run_over_bridge". */
    ATLAS_APPEND("    \"frameTags\": []\n  }\n}\n");
#undef ATLAS_APPEND

    const bool ok = jce_fs_host_write_all(path, buf, (uint64_t)off);
    JCE_FREE(buf);
    return ok;
}
