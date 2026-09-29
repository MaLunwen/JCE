/*
 * jce_static_batch.h  Merge static meshes that share a material into one mesh.
 *
 * Unity's Static Batching, UE's Merge Actors, Godot 4's MeshInstance3D merge.
 * The point is DRAW CALLS: a scene of forty tree trunks that all use one bark
 * material is forty draws, and it does not have to be.  On the hardware this
 * engine targets -- single core, integrated GPU -- draw-call count is the
 * budget that runs out first, well before triangles do.
 *
 * WHAT THIS IS NOT.  It is not instancing: instancing needs BYTE-IDENTICAL
 * geometry (jce_render_queue.c batches on vbh + ibh + index_count), so forty
 * DIFFERENT trunk meshes never batch however well their material matches.  And
 * it is not the HLOD proxy bake (jce_hlod_bake.h), which merges a whole cell
 * and then throws 85% of the triangles away and collapses every material into
 * one tint.  This is LOSSLESS: every triangle, every UV, one material per
 * group, at full detail, meant to REPLACE the originals rather than stand in
 * for them at a distance.
 *
 * WHERE THE WORK IS SPLIT, and why.  Deciding WHICH meshes may merge needs the
 * scene: what is static, what is enabled, what shares a material.  Turning a
 * decided group into an asset needs the model importer and the .glb writer.
 * This header is the second half only -- the caller hands over instances it has
 * already judged mergeable, exactly as jce_hlod_bake.h takes geometry the
 * caller has already gathered.  So the editor and the cook share this code and
 * each keeps its own knowledge of its own project layout, instead of this file
 * growing a notion of where a project keeps its models.
 *
 * THE RESULT IS IN WORLD SPACE, so the entity that renders a merged group sits
 * at the origin with an identity transform -- the same convention the HLOD
 * proxy uses, for the same reason: baking the transforms in is what removes the
 * per-object work.
 */

#ifndef JCE_STATIC_BATCH_H
#define JCE_STATIC_BATCH_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* One placed mesh offered for merging. */
typedef struct {
    /* A model file on the HOST filesystem (.glb/.gltf/.obj/...), loaded through
     * the ordinary importer.  A host path and not a VFS one because this runs
     * at BUILD time, where the project is a directory and not an archive. */
    const char *mesh_host_path;
    /* Column-major world matrix.  Baked into the merged vertices. */
    float       world[16];
    /* Instances sharing this key may merge.  The CALLER decides what it means
     * -- material path, or a hash of the whole material state -- because the
     * caller is the one that knows what its renderer treats as one material.
     * Getting it too COARSE merges objects that then render with one of their
     * two materials; too FINE merges nothing.  It is a caller decision because
     * only the caller can be wrong about it in a way this file could not
     * detect. */
    uint64_t    material_key;
    /* baseColorFactor written into the merged .glb's material.  A merged group
     * normally keeps its real material through the renderer component that
     * points at it, so this matters only to whatever reads the .glb alone. */
    float       base_color[4];
} JceStaticBatchInstance;

typedef struct {
    /* Groups smaller than this are left alone.  Merging a single mesh costs a
     * duplicated asset and saves nothing.  0 -> 2. */
    uint32_t min_group;
    /* A group is SPLIT once it would exceed this many vertices, so one merged
     * mesh never becomes an un-cullable continent.  0 -> 65536, which is also
     * the point below which 16-bit indices remain possible for a consumer that
     * wants them. */
    uint32_t max_vertices;
} JceStaticBatchDesc;

typedef struct {
    uint32_t instances_in;
    uint32_t instances_merged;   /* folded into a written group        */
    uint32_t groups_written;     /* .glb files produced                */
    uint32_t vertices, triangles;/* totals across written groups       */
    uint32_t skipped_unloadable; /* mesh file missing or empty         */
    uint32_t skipped_singleton;  /* material used by < min_group meshes*/
    /* What it is FOR, so a caller reports the number that matters instead of
     * re-deriving it: draws for these instances before and after. */
    uint32_t draws_before;       /* == instances_merged + skipped_singleton */
    uint32_t draws_after;        /* == groups_written   + skipped_singleton */
} JceStaticBatchStats;

/* Group `instances` by material_key, merge each group into world space, and
 * write one .glb per group as "<out_dir_host>/<name_prefix>_<group>.glb".
 *
 * `out_group_of_instance` (optional, `instance_count` entries) receives the
 * group index each instance landed in, or -1 for an instance that was NOT
 * merged -- unloadable, or alone in its material.  That is what lets a caller
 * rewrite its scene: remove the instances with a group, add one entity per
 * group, leave the -1s exactly as they were.
 *
 * Returns false only on bad arguments or a write failure.  An input that
 * produces NO groups is a SUCCESS with groups_written == 0: "nothing here was
 * worth merging" is an answer, not an error, and a caller that treated it as
 * one would refuse to cook a scene that simply has no repeated materials. */
JCE_API bool jce_static_batch_bake(const JceStaticBatchInstance *instances,
                                   uint32_t                      instance_count,
                                   const JceStaticBatchDesc     *desc,
                                   const char                   *out_dir_host,
                                   const char                   *name_prefix,
                                   int32_t                      *out_group_of_instance,
                                   JceStaticBatchStats          *out_stats);

/* ================================================================== */
/* Per-member sub-ranges: what makes a merged group cullable again     */
/* ================================================================== */

/* THE COST THIS PAYS BACK.  A merged group is one draw, which is the point,
 * and it is also ONE CULLABLE OBJECT: a row of forty fence posts folded into
 * one mesh draws all forty whenever any one of them is on screen.  Unity's
 * Static Batching does not pay that -- it keeps each renderer's index
 * sub-range into the shared buffer and submits only the ranges it can see.
 *
 * So the bake writes a sidecar beside every group .glb:
 *
 *     <out_dir>/<prefix>_<group>.glb
 *     <out_dir>/<prefix>_<group>.batch.json      <- this
 *
 * one entry per member, in the order they were concatenated, with the index
 * range it occupies and the WORLD-space box its vertices actually fill.  The
 * renderer reads it and submits runs of visible members; a group that is
 * wholly visible is still exactly ONE submit, because the runs coalesce --
 * an "optimisation" that turned the common case into N draws would be a
 * regression wearing the right name. */
typedef struct {
    uint32_t first_index;
    uint32_t index_count;
    float    aabb_min[3];   /* world space */
    float    aabb_max[3];
} JceStaticBatchMember;

/* Parse a member table out of the sidecar's TEXT.
 *
 * THE PARSER IS SEPARATE FROM THE READ because the two worlds that need it
 * read bytes differently and must not each grow their own parser: the bake
 * verifies through the HOST filesystem beside the .glb it just wrote, and the
 * runtime reads through the project VFS (a pak, at ship time).  A second
 * parser is a second answer to "what does a member table mean", and the two
 * would drift the first time the format grew a field.
 *
 * Returns the member count and fills up to `max_out`.  Zero for text that is
 * not a member table -- which is the ordinary answer and not an error. */
JCE_API uint32_t jce_static_batch_members_parse(const char           *json,
                                                size_t                len,
                                                JceStaticBatchMember *out,
                                                uint32_t              max_out);

/* Load the sidecar for a group .glb through the HOST filesystem.
 * `glb_host_path` is the .glb's path; the sidecar is that path with its
 * extension replaced by ".batch.json".
 *
 * Returns the member count and fills up to `max_out`.  ZERO is the ordinary
 * answer for a mesh that is not a merged group, and it is NOT an error: every
 * ordinary model in a project goes down this path and finds nothing.
 *
 * A sidecar that exists and does not parse returns 0 and logs -- a half-read
 * member table would cull triangles that should have drawn, which reads as
 * missing geometry and not as a bad file. */
JCE_API uint32_t jce_static_batch_members_load(const char           *glb_host_path,
                                               JceStaticBatchMember *out,
                                               uint32_t              max_out);

/* Given a member table and a visibility bit per member, write the INDEX RUNS
 * to submit into `out_first` / `out_count` and return how many there are.
 *
 * ADJACENT VISIBLE MEMBERS COALESCE, which is the property that keeps a fully
 * visible group at one draw.  Members are concatenated in bake order, so
 * neighbours in the table are neighbours in the index buffer and a run is a
 * plain [first, first+count) span -- no gather, no second buffer.
 *
 * Returns 0 when nothing is visible: the caller then submits nothing at all,
 * which is the case the whole mechanism exists for. */
JCE_API uint32_t jce_static_batch_visible_runs(const JceStaticBatchMember *members,
                                               const bool                 *visible,
                                               uint32_t                    member_count,
                                               uint32_t                   *out_first,
                                               uint32_t                   *out_count,
                                               uint32_t                    max_runs);

JCE_EXTERN_C_END

#endif /* JCE_STATIC_BATCH_H */
