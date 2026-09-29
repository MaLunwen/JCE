/*
 * jce_shader_reflect.h -- read a compiled bgfx shader blob back.
 *
 * A .bin produced by bgfx's shaderc is not opaque: it carries the shader's
 * whole uniform table, its vertex attribute list and its code section in a
 * documented layout, and every one of those is a question somebody asks.
 * The editor asks "what does this material's shader actually cost"; the cook
 * asks "does this blob still match the samplers the engine binds"; a person
 * staring at a black object asks "did my uniform even survive the compile".
 * None of them could ask before this, because the tree parsed the header in
 * exactly zero places -- it handed the bytes to bgfx and forgot them.
 *
 * WHY IT IS ENGINE CODE AND NOT A SCRIPT.  There must be ONE parser.  The
 * editor's Shader Inspector and the host-side jce_shader_inspect tool both
 * need it, and a second copy in Python would be a binary-format parser
 * maintained twice -- which is how a format drift becomes a silent
 * mis-report rather than an error.
 *
 * IT DOES NOT DISASSEMBLE.  For GLSL / ESSL / Metal / WGSL profiles the code
 * section IS the final source and comes back as text.  For D3D and SPIR-V it
 * is bytecode, and turning that into assembly is shaderc's `--disasm` (D3D
 * only) or an external tool -- not something to reimplement here.  The
 * `code_is_text` flag says which of the two you have, decided by measurement
 * rather than by profile name, because the profile is not in the blob.
 *
 * Layer: renderer.  No bgfx dependency -- this reads bytes.
 */
#ifndef JCE_SHADER_REFLECT_H
#define JCE_SHADER_REFLECT_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The uniform table is small in practice (fs_pbr, the largest shader in this
 * tree, declares 16 samplers plus its vec4 block).  A blob that declares more
 * is still parsed -- uniform_total reports the header's own count -- but only
 * this many are described. */
#define JCE_SHADER_REFLECT_MAX_UNIFORMS 96
#define JCE_SHADER_REFLECT_MAX_ATTRS    32
#define JCE_SHADER_REFLECT_NAME_MAX     64

typedef enum {
    JCE_SHADER_STAGE_UNKNOWN = 0,
    JCE_SHADER_STAGE_VERTEX,
    JCE_SHADER_STAGE_FRAGMENT,
    JCE_SHADER_STAGE_COMPUTE
} JceShaderStage;

/* bgfx's UniformType, low nibble of the type byte.  The two high bits are
 * flags (fragment, sampler) and are lifted out into their own fields. */
typedef enum {
    JCE_SHADER_UNIFORM_SAMPLER = 0,
    JCE_SHADER_UNIFORM_END     = 1,
    JCE_SHADER_UNIFORM_VEC4    = 2,
    JCE_SHADER_UNIFORM_MAT3    = 3,
    JCE_SHADER_UNIFORM_MAT4    = 4,
    JCE_SHADER_UNIFORM_OTHER   = 5
} JceShaderUniformKind;

typedef struct {
    char     name[JCE_SHADER_REFLECT_NAME_MAX];
    uint8_t  kind;          /* JceShaderUniformKind                        */
    bool     is_fragment;   /* declared in the fragment stage              */
    bool     is_sampler;    /* the sampler flag, distinct from kind        */
    uint8_t  num;           /* array length; 0 for a scalar declaration    */
    uint16_t reg_index;     /* first constant-buffer register              */
    uint16_t reg_count;     /* registers occupied                          */
} JceShaderUniformInfo;

typedef struct {
    JceShaderStage stage;
    uint8_t  format_version;      /* the version byte after the 3-char magic */
    uint32_t hash_in;             /* input  varying hash (0 in fragment bins) */
    uint32_t hash_out;            /* output varying hash                      */

    uint32_t uniform_total;       /* what the header claims                  */
    uint32_t uniform_count;       /* how many are described below            */
    JceShaderUniformInfo uniforms[JCE_SHADER_REFLECT_MAX_UNIFORMS];

    uint32_t code_size;
    uint32_t code_offset;         /* byte offset of the code INTO the blob   */
    /* True when the code section is source text rather than bytecode.  See
     * the file header: decided by measuring the bytes, because nothing in
     * the blob names its profile. */
    bool     code_is_text;

    /* Vertex/compute only, and ABSENT from some profiles' blobs -- a GLSL
     * fragment bin ends right after the code.  attr_present says whether the
     * trailer was there at all, so "no attributes" and "the blob does not
     * carry an attribute list" stay distinguishable. */
    bool     attr_present;
    uint8_t  attr_count;
    uint16_t attrs[JCE_SHADER_REFLECT_MAX_ATTRS];
    uint16_t uniform_block_size;  /* bytes of constant data bgfx will upload */
} JceShaderReflection;

/* Parse a bgfx shader blob.  Returns false and leaves *out zeroed when the
 * bytes are not a bgfx shader (wrong magic) or when a length field runs past
 * the end -- a truncated blob is a failure, never a partial answer.
 * `out` may not be NULL. */
JCE_API bool jce_shader_reflect(const void *blob, size_t size,
                                JceShaderReflection *out);

/* "VSH" / "FSH" / "CSH" / "?" -- the magic as written, for a report. */
JCE_API const char *jce_shader_stage_magic(JceShaderStage stage);

/* bgfx's Attrib enum -> "POSITION", "NORMAL", "TEXCOORD0", ...  Returns
 * "ATTR<n>" for a value this build does not know, never NULL. */
JCE_API const char *jce_shader_attrib_name(uint16_t attr);

/* "sampler" / "vec4" / "mat3" / "mat4" / "end" / "other". */
JCE_API const char *jce_shader_uniform_kind_name(uint8_t kind);

/* Instruction classes counted from a DirectX disassembly listing -- what
 * shaderc's --disasm writes beside the .bin, and the only per-instruction
 * cost signal this toolchain produces at all.
 *
 * NOT read from the listing's own trailing "Approximately N instruction
 * slots used": measured on fs_composite at -O0 and -O3, that line says 0
 * both times.  A number that is always zero is worse than no number.
 *
 * Here rather than in the two callers because the editor panel and the
 * host jce_shader_inspect both want it, and a classifier that drifts
 * between them would make the panel and CI disagree about the same
 * shader. */
typedef struct {
    uint32_t total;      /* instruction lines, declarations excluded    */
    uint32_t arithmetic;
    uint32_t texture;    /* sample / ld / gather / resinfo / lod        */
    uint32_t flow;       /* if / loop / ret / discard / switch / call   */
    uint32_t declarations;
} JceShaderDisasmStats;

/* Count `text` (NUL-terminated D3D assembly).  Zeroes *out and returns
 * false when either argument is NULL; an empty listing is a successful
 * count of nothing. */
JCE_API bool jce_shader_disasm_stats(const char *text,
                                     JceShaderDisasmStats *out);

/* Bytes of constant data the uniform table accounts for: the sum over
 * non-sampler uniforms of reg_count * 16.  Reported next to
 * uniform_block_size because the two disagreeing is worth seeing -- the
 * trailer is what bgfx uploads, this is what the declarations add up to. */
JCE_API uint32_t jce_shader_reflect_uniform_bytes(const JceShaderReflection *r);

#ifdef __cplusplus
}
#endif

#endif /* JCE_SHADER_REFLECT_H */
