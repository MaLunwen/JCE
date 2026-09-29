/*
 * jce_pbr_material.h  PBR (Physically Based Rendering) material system.
 *
 * Metallic-Roughness workflow following glTF 2.0 specification.
 * Supports albedo, metallic-roughness, normal, AO, and emissive maps.
 *
 * Layer: Graphics (Layer 3) — public API.
 */

#ifndef JCE_PBR_MATERIAL_PUBLIC_H
#define JCE_PBR_MATERIAL_PUBLIC_H


#include <jce/os/core/jce_defs.h>
#include <jce/renderer/jce_texture_types.h>
#include <jce/renderer/jce_gfx_types.h>
#include <jce/renderer/jce_shader_variants.h>   /* JceBlendMode */

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceFileSystem JceFileSystem;

typedef struct JceRenderer JceRenderer;

/* ================================================================== */
/* Alpha mode                                                          */
/* ================================================================== */

typedef enum {
    JCE_ALPHA_OPAQUE = 0,   /* Fully opaque, alpha ignored              */
    JCE_ALPHA_MASK,          /* Alpha test with cutoff threshold          */
    JCE_ALPHA_BLEND          /* Standard alpha blending                   */
} JceAlphaMode;

/* ================================================================== */
/* PBR Material                                                        */
/* ================================================================== */

/* Stencil compare, in the order Unity lists them.  0 is OFF rather than a
 * comparison, because a zeroed material must not suddenly start testing. */
typedef enum {
    JCE_STENCIL_OFF          = 0,
    JCE_STENCIL_NEVER        = 1,
    JCE_STENCIL_LESS         = 2,
    JCE_STENCIL_LEQUAL       = 3,
    JCE_STENCIL_EQUAL        = 4,
    JCE_STENCIL_GEQUAL       = 5,
    JCE_STENCIL_GREATER      = 6,
    JCE_STENCIL_NOTEQUAL     = 7,
    JCE_STENCIL_ALWAYS       = 8,
} JceStencilFunc;

/* What to do to the stencil buffer.  KEEP is 0 so a zeroed block leaves the
 * buffer alone, which is the only safe default for a value other draws read. */
typedef enum {
    JCE_STENCIL_OP_KEEP      = 0,
    JCE_STENCIL_OP_ZERO      = 1,
    JCE_STENCIL_OP_REPLACE   = 2,
    JCE_STENCIL_OP_INCR      = 3,
    JCE_STENCIL_OP_INCR_WRAP = 4,
    JCE_STENCIL_OP_DECR      = 5,
    JCE_STENCIL_OP_DECR_WRAP = 6,
    JCE_STENCIL_OP_INVERT    = 7,
} JceStencilOp;

typedef struct JcePbrMaterial {
    /* Texture maps (JCE_TEXTURE_INVALID = not present, use factor).
     *   stage 0: s_albedo
     *   stage 1: s_metalRough  (G = roughness, B = metallic)
     *   stage 2: s_normalMap
     *   stage 3: s_aoMap       (R channel)
     *   stage 4: s_emissive
     */
    JceTexture   albedo_map;
    JceTexture   metallic_roughness_map;
    JceTexture   normal_map;
    JceTexture   ao_map;
    JceTexture   emissive_map;

    /* Factors (multiplied with texture values, or used directly when
     * the corresponding texture is not present). */
    float        base_color_factor[4];   /* RGBA, linear color space     */
    float        metallic_factor;        /* 0.0 .. 1.0                   */
    float        roughness_factor;       /* 0.0 .. 1.0                   */
    float        emissive_factor[3];     /* RGB                          */
    float        normal_scale;           /* normal map strength, default 1.0 */
    float        ao_strength;            /* AO intensity, 0.0 .. 1.0     */

    /* Alpha handling. */
    JceAlphaMode alpha_mode;
    float        alpha_cutoff;           /* MASK mode threshold, default 0.5 */

    /* Render state. */
    bool         double_sided;
    /* Per-renderer "Receive Shadows" off (Unity-style): when true the shader
       skips ALL shadow sampling (CSM + local atlas) for this draw. */
    bool         receive_shadows_off;

    /* Optional custom shader program.  The handle itself is runtime-only
     * (never serialized), but jce_pbr_material_load_json will populate it
     * automatically when the .mat.json declares "customProgramVs" /
     * "customProgramFs" (compiled bgfx .bin blobs produced by the editor's
     * Shader Graph "Compile & Bind").  UINT16_MAX = unset (caller picks the
     * default PBR program; see jce_pbr_material_effective_program). */
    uint16_t     custom_program;

    /* Transparent draw-order override (Unity's Material.renderQueue offset,
     * Godot's render_priority).  Higher draws LATER, so on top; 0 is neutral
     * and every material authored before this existed loads as 0.
     *
     * Only the transparent pass reads it -- opaque order is a performance
     * decision (front-to-back, to kill overdraw), not an authoring one, and
     * letting an author reorder it would cost frame time to fix a problem
     * opaque geometry does not have. */
    int16_t      render_priority;

    /* UV TILING and OFFSET, applied to every texture this material samples.
     *   uv = v_texcoord0 * uv_tiling + uv_offset
     * Unity's Tiling/Offset, Godot's uv1_scale/uv1_offset, UE's TexCoord
     * node scale.  Tiling defaults to (1,1) and offset to (0,0), so a
     * material authored before this key existed samples exactly as before.
     *
     * DEFAULTING MATTERS MORE THAN USUAL HERE: a zeroed tiling collapses
     * every texture to one texel, so this pair must never arrive from a
     * memset.  jce_pbr_material_default() sets it, and that is the only
     * constructor -- the .mat.json loader starts from it (jce_pbr_material.c
     * calls it before parsing), so an old file inherits (1,1)/(0,0) rather
     * than zeros.
     *
     * ONE transform for all five maps, not one per map.  Per-map UVs are a
     * real feature in bigger engines and a real cost here: five more vec4
     * uniforms on every draw to express something almost nothing authors.
     * Tiling a wall texture is the case that exists, and until this landed
     * the only way to do it was to bake the repeat into the mesh's UVs.
     *
     * The missing-texture CHECKER deliberately ignores this: it visualises
     * the mesh's OWN uv layout to diagnose unwrapping, and transforming it
     * would hide exactly what it is there to show. */
    float        uv_tiling[2];           /* default 1,1 */
    float        uv_offset[2];           /* default 0,0 */

    /* HOW a BLEND-mode surface composites: alpha, additive or multiply.
     * Ignored unless alpha_mode is JCE_ALPHA_BLEND.  Defaults to
     * JCE_BLEND_ALPHA, which is the equation this engine emitted
     * unconditionally before the field was read, so every existing material
     * composites exactly as it did. */
    JceBlendMode blend_mode;

    /* STENCIL, the full block Unity's materials carry: reference value, read
     * and write masks, compare function and the three operations.  Portals,
     * outlines and UI masking are all this one mechanism, and none of them
     * was expressible before -- the renderer set no stencil state at all.
     *
     * stencil_func 0 means OFF and is what every material and every older
     * .mat.json already holds, so a zeroed block disables the test entirely
     * and nothing that exists today changes.  The values are JceStencilFunc /
     * JceStencilOp below rather than raw bgfx bits, because a serialised
     * number that means a bgfx constant is a file that breaks when bgfx
     * renumbers.
     *
     * SIX FIELDS, NOT UNITY'S SEVEN: there is no write mask, because bgfx's
     * stencil word has none -- it carries REF, the READ mask, the test and
     * the three ops, and nothing else.  Authoring a write mask that the
     * renderer cannot express would be one more field somebody finds unread
     * a year later, which is the whole defect class this block was added
     * under.
     *
     * APPENDED (ABI). */
    uint8_t      stencil_func;       /* JceStencilFunc; 0 = off        */
    uint8_t      stencil_ref;        /* 0..255                          */
    uint8_t      stencil_read_mask;  /* 0 = 0xFF                        */
    uint8_t      stencil_fail_op;    /* JceStencilOp, stencil test fail */
    uint8_t      stencil_zfail_op;   /* JceStencilOp, depth test fail   */
    uint8_t      stencil_pass_op;    /* JceStencilOp, both pass         */

    /* EXTENDED LOBES -- glTF KHR_materials_clearcoat and _sheen.
     *
     * CLEARCOAT is a second, thin specular layer over the base: car paint,
     * lacquer, a wet surface.  Its own roughness, because the coat is
     * smoother than what is under it -- that difference IS the effect.
     * SHEEN is the retroreflective rim a fibrous surface has: velvet, satin,
     * brushed cloth.  Its own colour, because a fabric's sheen is rarely the
     * colour of its body.
     *
     * Both default to 0 = OFF, so every material and every .mat.json that
     * exists shades byte-identically.  Named after the glTF extensions on
     * purpose: a serialised number meaning "clearcoat" should mean what every
     * other tool means by it.
     *
     * (This paragraph used to end "NOT anisotropy or subsurface", on the
     * grounds that anisotropy needs a tangent frame the shading path does not
     * carry.  It does carry one -- fs_pbr_main.sh builds a TBN for normal
     * mapping three lines above the specular lobe -- so anisotropy ships
     * below.  The subsurface half of that sentence stands, and what ships
     * beside it is named translucency rather than pretending otherwise.)
     * APPENDED (ABI). */
    float        clearcoat;            /* 0 = off .. 1 = full coat        */
    float        clearcoat_roughness;  /* 0 = mirror .. 1 = matte coat    */
    float        sheen_color[3];       /* linear; 0,0,0 = off             */
    float        sheen_roughness;      /* width of the rim lobe           */

    /* THE OTHER TWO LOBES.
     *
     * ANISOTROPY -- glTF KHR_materials_anisotropy.  A specular highlight
     * stretched along a direction: brushed metal, hair, vinyl, a machined
     * face.  The direction is the mesh's own tangent, rotated in the tangent
     * plane by `anisotropy_rotation`, so it follows the UV layout the way
     * every other tool's does.  The claim that this needed a tangent frame
     * the shading path does not carry was wrong: fs_pbr_main.sh builds a TBN
     * from v_tangent and v_bitangent three lines above the specular lobe.
     *
     * TRANSLUCENCY -- wrap-around diffuse plus a back-lit transmission term,
     * driven by one thickness scalar.  NOT called subsurface, on purpose: a
     * real subsurface model is a diffusion profile and a thickness map, and
     * on a stated integrated-GPU baseline that is the wrong subsystem to
     * carry.  This is what Godot 4's SSS does at its cheap end and what Unity
     * calls Translucent -- leaves, ears, wax, curtains -- and naming it after
     * what it is keeps a future diffusion profile free to arrive as its own
     * thing rather than as a redefinition of this number.
     *
     * Both default to 0 = OFF.  APPENDED (ABI). */
    float        anisotropy;           /* 0 = isotropic .. 1 = fully stretched */
    float        anisotropy_rotation;  /* turns [0,1), about the surface normal */
    float        translucency;         /* 0 = opaque .. 1 = fully translucent  */
    float        translucency_thickness;/* 0 = paper-thin .. 1 = thick          */
    float        translucency_color[3];/* linear; the tint light picks up going through */
} JcePbrMaterial;

/* ================================================================== */
/* API                                                                 */
/* ================================================================== */

/* Create a default PBR material (white, metallic=0, roughness=1, opaque). */
JCE_API JcePbrMaterial jce_pbr_material_default(void);

/* Bind the PBR material state (textures, uniforms) for the next draw call.
 * Selects the appropriate PBR shader program.
 * Must be called after jce_renderer_begin_frame_3d. */
/* Per-draw UV TRANSFORM override, armed and disarmed around a draw the way
 * jce_model_set_material_override() is.  NULL disarms.
 *
 * WHY IT IS NOT JUST THE MATERIAL'S FIELD.  A glTF model draws with the
 * materials the ASSET carries, not with anything the MeshRenderer says --
 * the renderer's material only reaches the draw when it has an authored
 * albedo or .mat.json, and a model dropped into a scene has neither.  So a
 * tiling authored on the renderer reached nothing at all on the single most
 * common shape in this tree: measured on a scene with 1789 renderers and
 * zero authored materials, tiling 1 -> 4 moved 4046 px against a 4339 px
 * noise floor.
 *
 * Overriding the whole material instead would flatten a multi-material model
 * onto one set of factors, which is a much larger change than "tile the
 * textures".  A transform is the smaller, truer thing to override.
 *
 * The pointers are copied, not retained. */
JCE_API void jce_pbr_material_set_uv_override(const float tiling[2],
                                              const float offset[2]);

/* THE KEYWORD BITS THIS MATERIAL ASKS FOR.
 *
 * A draw path calls this rather than deciding for itself which axes matter,
 * because "for itself" is how six of seven paths ended up agreeing and the
 * seventh silently not.  Feed the result to
 * jce_renderer_get_program_variant together with the frame's own keys.
 *
 * Today it answers one axis: a material with receive_shadows_off gets
 * JCE_SHADER_KEY_NOSHADOW, whose program never compiled the cascade block --
 * 8329 instructions down to 1549, and 1061 texture fetches down to 22, the
 * 1039 it drops being the PCF taps.  The PIXELS ARE THE SAME: the base
 * shader already read that flag at runtime and multiplied by one.  That is
 * the property the test asserts, because it is the one that can fail
 * silently. */
JCE_API uint32_t jce_pbr_material_shader_keys(const JcePbrMaterial *mat);

JCE_API void jce_pbr_material_bind(const JcePbrMaterial *mat,
                                    const JceRenderer *r, uint16_t view_id);

/* Bind one PBR draw while replacing selected texture slots exactly once.
 * A JCE_TEXTURE_INVALID override keeps the material texture/fallback.
 * Terrain and texture-array batching use this because their shaders reuse
 * s_albedo/s_emissive with draw-specific resources. */
JCE_API void jce_pbr_material_bind_texture_overrides(
    const JcePbrMaterial *mat, const JceRenderer *r, uint16_t view_id,
    JceTexture albedo_override, JceTexture emissive_override);

/* Build the bgfx render state (write masks, depth test, cull, blend) for
 * this material.  Honours alpha_mode (BLEND → src-alpha / inv-src-alpha
 * blend with depth-write disabled) and double_sided (no back-face cull).
 * Returns a value suitable for bgfx_set_state(); 0 is never returned for a
 * valid material.  Used by the renderer so both the inline and render-queue
 * draw paths apply identical transparency / culling state. */
JCE_API uint64_t jce_pbr_material_render_state(const JcePbrMaterial *mat);

/* The bgfx stencil word for this material, or 0 (BGFX_STENCIL_NONE) when the
 * material does not use the stencil buffer.  Separate from the render state
 * because bgfx keeps them separate: set_stencil is its own call, and it is
 * reset by submit exactly as set_state is -- so callers must issue it beside
 * every set_state, not once per material run. */
JCE_API uint32_t jce_pbr_material_stencil(const JcePbrMaterial *mat);

/* True when this material renders in the transparent (alpha-blended) pass
 * and therefore needs back-to-front sorting.  MASK/OPAQUE return false. */
JCE_API bool jce_pbr_material_is_transparent(const JcePbrMaterial *mat);

/* Resolve the effective shader program for this material.
 *   - returns mat->custom_program (wrapped) when set (!= UINT16_MAX)
 *   - otherwise returns `default_program` (typically program_pbr).
 * Callers that submit draws should use this to honour graph-generated
 * shaders bound at runtime by the editor's Material Graph "Compile &
 * Bind".  Returns UINT16_MAX inside the wrapper if neither source is
 * valid, in which case the caller should skip submission. */
JCE_API uint16_t jce_pbr_material_effective_program(const JcePbrMaterial *mat,
                                                     uint16_t default_program);

/* Set the global PBR view mode (0=shaded, 1=wireframe, 2=textured/unlit,
 * 3=wireframe+textured). Affects all subsequent jce_pbr_material_bind
 * calls. The scene renderer pushes this once per frame from its config. */
JCE_API void jce_pbr_material_set_view_mode(int mode);

/* Bind ONLY the view-mode component of u_normalScale.
 *
 * For shading paths that do not go through jce_pbr_material_bind at all --
 * grass and foliage each own their whole uniform set -- and therefore never
 * receive the view mode. The symptom is silent and total: those shaders keep
 * rendering their normal lit output in every debug view, so the view shows a
 * partial picture that looks complete. In hidden_cove that was 46% of the
 * viewport, all of it vegetation, which is most of what a forest scene IS.
 *
 * Call once before submitting such a batch. Only .z is meaningful to those
 * shaders; the other components are zeroed, which is correct for them because
 * they read no other field. */
JCE_API void jce_pbr_material_bind_view_mode(void);

/* Release the process-wide cache of graph-generated custom programs created
 * lazily by jce_pbr_material_load_json.  Call once during renderer teardown
 * (after the last frame, before bgfx_shutdown).  Safe to call when empty. */
JCE_API void jce_pbr_material_shutdown(void);

/* ================================================================== */
/* Material file I/O (.mat.json)                                       */
/* ================================================================== */

/* The .mat.json keys accepted for texture slot `slot` (0=albedo,
 * 1=metallic-roughness, 2=normal, 3=AO, 4=emissive — the same order as the
 * out_tex_paths / tex_paths arrays below), in the exact priority order
 * jce_pbr_material_load_json applies them.  Index 0 is the canonical key
 * jce_pbr_material_save_json writes; the rest are accepted aliases.  The
 * returned array is static, immutable and NULL-terminated; NULL is returned
 * for an out-of-range slot.
 *
 * This is the SINGLE authority for material texture keys.  Any other code
 * that has to find a texture reference inside a .mat.json — the editor's
 * material preview resolver, the bundle packer's dependency scan — must
 * iterate this instead of hard-coding its own alias list, or previews and
 * cooked bundles end up disagreeing with the runtime about which texture a
 * material actually uses. */
JCE_API const char *const *jce_pbr_material_texture_keys(int slot);

/* Load PBR material parameters from a .mat.json file.
 * Texture paths are returned via out_tex_paths (not bound to handles); each
 * slot accepts the key set published by jce_pbr_material_texture_keys(), read
 * from a "properties" object when present and from the root otherwise (and
 * as a fallback when a "properties" object exists but omits the key).
 *
 * When the file declares "customProgramVs" + "customProgramFs" (compiled
 * bgfx .bin blobs), both are loaded and linked into out->custom_program so
 * graph-generated shaders persisted by the editor render automatically.
 * That link is why this call is NOT thread-safe — it creates a bgfx program
 * and records it in a process-wide cache with no lock, so only the thread
 * that owns the renderer may call it.  Code on a worker thread that just
 * needs a texture reference must parse the file itself, driving the key set
 * from jce_pbr_material_texture_keys() so it stays in step with this loader.
 * Returns true on success. */
JCE_API bool jce_pbr_material_load_json(const char *path, JcePbrMaterial *out,
                                        char out_tex_paths[5][256]);

/* VFS equivalent of jce_pbr_material_load_json().  Paths remain virtual and
 * are resolved relative to the material path before falling back to the VFS
 * root, so PAK-only scenes retain the same material contract as loose files. */
JCE_API bool jce_pbr_material_load_json_vfs(const JceFileSystem *fs,
                                            const char *path,
                                            JcePbrMaterial *out,
                                            char out_tex_paths[5][256]);

/* Save PBR material parameters to a .mat.json file.
 * tex_paths[0..4] = albedo, metallic_roughness, normal, ao, emissive. */
JCE_API bool jce_pbr_material_save_json(const char *path,
                                 const JcePbrMaterial *mat,
                                 const char tex_paths[5][256]);

/* Attach (or clear) a Shader Graph custom-shader reference on an existing
 * .mat.json, preserving all other fields (read-modify-write).  Used by the
 * editor's Material Graph "Compile & Bind" to persist graph-generated
 * shaders so jce_pbr_material_load_json can re-create the program later.
 *
 *   graph_path  source .matgraph.json (reference only; may be NULL).
 *   vs_bin_path compiled vertex .bin path.
 *   fs_bin_path compiled fragment .bin path.
 *
 * Passing NULL/"" for vs_bin_path AND fs_bin_path removes the keys (detach).
 * Paths are written verbatim; callers should store project-relative paths.
 * Returns true on success. */
/* The material this one inherits from, as written in the file (relative to
 * it, not resolved), or false when the file names none / cannot be read.
 *
 * A child starts from its parent and overrides only the keys it states, so
 * editing the parent reaches every child -- UE's Material Instances, Unity's
 * Material Variants.  Chains are allowed up to a fixed depth and a cycle is
 * refused with a warning rather than a hang.
 *
 * Through the FILE rather than through JcePbrMaterial, like the graph-shader
 * reference above: the struct is a resolved render state that is copied by
 * value on every draw, and the link belongs to the document. */
JCE_API bool jce_pbr_material_get_parent(const char *mat_path,
                                         char *out, size_t out_sz);

/* Set (or, with NULL/"" , clear) the parent link, leaving every other key in
 * the file untouched.  Refuses a material as its own parent; deeper cycles are
 * caught at load, because only the loader can see the whole chain. */
JCE_API bool jce_pbr_material_set_parent(const char *mat_path,
                                         const char *parent_path);

JCE_API bool jce_pbr_material_set_graph_shader(const char *mat_path,
                                               const char *graph_path,
                                               const char *vs_bin_path,
                                               const char *fs_bin_path);

JCE_EXTERN_C_END

#endif /* JCE_PBR_MATERIAL_PUBLIC_H */
