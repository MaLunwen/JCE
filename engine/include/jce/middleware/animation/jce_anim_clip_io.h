/*
 * jce_anim_clip_io.h — a JceAnimClip that can be written down.
 *
 * WHY THIS EXISTS.  jce_anim_clip_create is called from exactly two places in
 * the tree — jce_gltf_loader.c:1201 and jce_model_importer.cpp:1254 — and
 * there is NO SERIALISER ANYWHERE.  A clip could therefore only ever arrive
 * by importing a model, and nothing could persist one: the editor cannot save
 * a clip, a project cannot ship a clip without the model it came in, and a
 * SkeletalAnimator names its clips by string (JceSkeletalAnimator.clip_name /
 * clip_names[8]) against whatever happens to be inside its skeleton asset.
 *
 * The Animation Clip Editor's own header says the consequence out loud:
 * legacy float/vec3 tracks are "dropped -- no engine consumer".  A written
 * reason is the first place to check whether it is still true.
 *
 * WHAT THIS IS NOT.  It is not the reserved JCEASSET_CHUNK_MODEL_ANIMS
 * (0x0604).  That chunk and its whole MODEL_* family are declared and
 * unproduced, because models deliberately pass through the cooker unchanged
 * -- jce_asset_cooker.c says so in its default case.  Building on a slot the
 * tree has decided not to use would revive a format by accident.
 *
 * THE SURFACE IS THREE FUNCTIONS AND EACH HAS A CONSUMER.  parse is called
 * by the runtime's clip-file fallback (sr_find_file_clip); serialize and its
 * free are called by the editor's clip export, which writes with the panel's
 * own file helper the way the rest of that panel does.  There is deliberately
 * no load() and no save(): a VFS loader had no caller at all -- the runtime
 * reads PAK-first through its own path resolver -- and a save() would have
 * been a second way to write the same bytes.
 *
 * FORMAT: JSON, through <jce/os/core/jce_json.h>, the same facade the editor
 * writes its sidecars with.  Chosen over a packed binary for the reason the
 * curve format was: a clip a human can open is a clip a human can diff, and
 * the volume here is keyframes per joint, not vertices.
 *
 *   {
 *     "name": "Run",
 *     "duration": 1.2,
 *     "channels": [
 *       { "joint": 3, "target": "translation", "interp": "linear",
 *         "times": [0.0, 0.6, 1.2],
 *         "values": [[x,y,z], [x,y,z], [x,y,z]] },
 *       { "joint": 3, "target": "rotation", ...
 *         "values": [[x,y,z,w], ...] }
 *     ]
 *   }
 *
 * TARGET AND INTERPOLATION ARE NAMES, NOT NUMBERS.  JceAnimTarget and
 * JceInterpolation are internal enums whose numeric values are free to change;
 * writing the integer would freeze them into every file on disk, which is the
 * mistake `kind` in .import.json already forces us to live with.  An unknown
 * name degrades to a stated default rather than to whatever the switch's
 * default branch happens to be.
 *
 * Layer: Middleware (L4), animation.
 */

#ifndef JCE_ANIM_CLIP_IO_H
#define JCE_ANIM_CLIP_IO_H

#include <jce/middleware/animation/jce_animation.h>

#include <stdbool.h>
#include <stddef.h>

JCE_EXTERN_C_BEGIN

/* Parse a clip document.  NULL when the bytes are not JSON, when there is no
 * `channels` array, or when every channel in it is unusable.
 *
 * A clip with ZERO usable channels is refused rather than returned empty: an
 * empty clip samples to the rest pose at every time, which looks exactly like
 * a character that is standing still on purpose.  The one failure this format
 * can have that nothing downstream would report is the one it must refuse. */
JCE_API JceAnimClip *JCE_CALL jce_anim_clip_parse(const char *json, size_t len);

/* Serialise to a newly allocated NUL-terminated string, or NULL.  Free it
 * with jce_anim_clip_io_free_string -- NOT with free(): the engine's
 * allocator is tracked, and a cross-allocator free is the kind of defect that
 * only shows up under a different CRT. */
JCE_API char *JCE_CALL jce_anim_clip_serialize(const JceAnimClip *clip);
JCE_API void  JCE_CALL jce_anim_clip_io_free_string(char *s);

JCE_EXTERN_C_END

#endif /* JCE_ANIM_CLIP_IO_H */
