/*
 * jce_shader_reflect.c -- see the header for why this exists and what it
 * deliberately does not do.
 *
 * The layout below was read off real blobs rather than off memory: a
 * fragment bin and a vertex bin were compiled for both s_5_0 and GLSL 120
 * and every field boundary was checked against the bytes.  Two things a
 * from-memory implementation would have got wrong, and that the byte-level
 * check caught:
 *
 *   * The per-uniform record is TEN bytes after the name, not six: bgfx
 *     appends texInfo and texFormat, and reading six would have walked the
 *     table off by four bytes per entry from the second uniform onward --
 *     which produces plausible garbage names, not an error.
 *
 *   * The attribute/size trailer is NOT always present.  A GLSL fragment
 *     blob ends immediately after the code's terminating NUL, while the
 *     D3D one carries three more bytes.  Treating the trailer as mandatory
 *     would have made every GLSL fragment shader read as truncated.
 */
#include <jce/renderer/jce_shader_reflect.h>

#include <string.h>

/* -- little-endian readers that refuse to run off the end -------------- */

typedef struct {
    const uint8_t *p;
    size_t         n;
    size_t         at;
    bool           bad;
} Cursor;

static uint8_t rd_u8(Cursor *c)
{
    if (c->bad || c->at + 1 > c->n) { c->bad = true; return 0; }
    return c->p[c->at++];
}

static uint16_t rd_u16(Cursor *c)
{
    if (c->bad || c->at + 2 > c->n) { c->bad = true; return 0; }
    uint16_t v = (uint16_t)(c->p[c->at] | ((uint16_t)c->p[c->at + 1] << 8));
    c->at += 2;
    return v;
}

static uint32_t rd_u32(Cursor *c)
{
    if (c->bad || c->at + 4 > c->n) { c->bad = true; return 0; }
    uint32_t v = (uint32_t)c->p[c->at]
               | ((uint32_t)c->p[c->at + 1] << 8)
               | ((uint32_t)c->p[c->at + 2] << 16)
               | ((uint32_t)c->p[c->at + 3] << 24);
    c->at += 4;
    return v;
}

/* Is this code section source text?  Nothing in the blob names its profile,
 * so the honest test is on the bytes: source is overwhelmingly printable and
 * carries no embedded NUL, bytecode is neither.  The threshold is high on
 * purpose -- a DXBC blob is roughly half printable by accident, and a
 * misclassification here would put binary into a text panel. */
static bool code_looks_like_text(const uint8_t *p, uint32_t n)
{
    if (n == 0) return false;
    uint32_t printable = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t b = p[i];
        if (b == 0) return false;
        if ((b >= 32 && b < 127) || b == 9 || b == 10 || b == 13)
            printable++;
    }
    return (uint64_t)printable * 100u >= (uint64_t)n * 97u;
}

bool jce_shader_reflect(const void *blob, size_t size, JceShaderReflection *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!blob || size < 15u) return false;   /* magic+hashes+count is 14 */

    Cursor c;
    c.p = (const uint8_t *)blob;
    c.n = size;
    c.at = 0;
    c.bad = false;

    const uint8_t *m = c.p;
    if (m[0] == 'V' && m[1] == 'S' && m[2] == 'H')
        out->stage = JCE_SHADER_STAGE_VERTEX;
    else if (m[0] == 'F' && m[1] == 'S' && m[2] == 'H')
        out->stage = JCE_SHADER_STAGE_FRAGMENT;
    else if (m[0] == 'C' && m[1] == 'S' && m[2] == 'H')
        out->stage = JCE_SHADER_STAGE_COMPUTE;
    else { memset(out, 0, sizeof(*out)); return false; }
    c.at = 3;
    out->format_version = rd_u8(&c);

    out->hash_in  = rd_u32(&c);
    out->hash_out = rd_u32(&c);

    out->uniform_total = rd_u16(&c);
    for (uint32_t i = 0; i < out->uniform_total; i++) {
        uint8_t nlen = rd_u8(&c);
        if (c.bad || c.at + nlen > c.n) {
            memset(out, 0, sizeof(*out));
            return false;
        }

        JceShaderUniformInfo tmp;
        memset(&tmp, 0, sizeof(tmp));
        uint8_t keep = nlen;
        if (keep > (uint8_t)(JCE_SHADER_REFLECT_NAME_MAX - 1))
            keep = (uint8_t)(JCE_SHADER_REFLECT_NAME_MAX - 1);
        memcpy(tmp.name, c.p + c.at, keep);
        c.at += nlen;

        uint8_t type  = rd_u8(&c);
        tmp.num       = rd_u8(&c);
        tmp.reg_index = rd_u16(&c);
        tmp.reg_count = rd_u16(&c);
        (void)rd_u16(&c);   /* texInfo   -- format-carrying, not reported */
        (void)rd_u16(&c);   /* texFormat -- ditto                        */
        if (c.bad) { memset(out, 0, sizeof(*out)); return false; }

        /* 0x10 = fragment, 0x20 = sampler; the kind is the low nibble. */
        tmp.is_fragment = (type & 0x10u) != 0u;
        tmp.is_sampler  = (type & 0x20u) != 0u;
        uint8_t kind    = (uint8_t)(type & 0x0Fu);
        tmp.kind = (kind <= (uint8_t)JCE_SHADER_UNIFORM_MAT4)
                     ? kind : (uint8_t)JCE_SHADER_UNIFORM_OTHER;

        if (out->uniform_count < JCE_SHADER_REFLECT_MAX_UNIFORMS)
            out->uniforms[out->uniform_count++] = tmp;
    }

    out->code_size = rd_u32(&c);
    if (c.bad || c.at + out->code_size > c.n) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    out->code_offset  = (uint32_t)c.at;
    out->code_is_text = code_looks_like_text(c.p + c.at, out->code_size);
    c.at += out->code_size;
    (void)rd_u8(&c);   /* the code's terminating NUL */
    if (c.bad) { memset(out, 0, sizeof(*out)); return false; }

    /* Optional trailer.  Absent in some profiles' blobs -- see the file
     * header.  A partial trailer is treated as absent rather than as an
     * error: the code section already parsed cleanly, and refusing the whole
     * blob over three trailing bytes would lose the useful answer. */
    if (c.at < c.n) {
        size_t  save   = c.at;
        uint8_t n_attr = rd_u8(&c);
        if (!c.bad && c.at + (size_t)n_attr * 2u + 2u <= c.n) {
            out->attr_present = true;
            out->attr_count   = n_attr;
            for (uint8_t i = 0; i < n_attr; i++) {
                uint16_t a = rd_u16(&c);
                if (i < JCE_SHADER_REFLECT_MAX_ATTRS) out->attrs[i] = a;
            }
            out->uniform_block_size = rd_u16(&c);
        } else {
            c.at  = save;
            c.bad = false;
        }
    }

    return true;
}

const char *jce_shader_stage_magic(JceShaderStage stage)
{
    switch (stage) {
    case JCE_SHADER_STAGE_VERTEX:   return "VSH";
    case JCE_SHADER_STAGE_FRAGMENT: return "FSH";
    case JCE_SHADER_STAGE_COMPUTE:  return "CSH";
    default:                        return "?";
    }
}

const char *jce_shader_attrib_name(uint16_t attr)
{
    /* bgfx's Attrib enum, in declaration order.  Kept as a table rather than
     * derived, because the order IS the wire format and a reordering would
     * otherwise be invisible. */
    static const char *const k[] = {
        "POSITION", "NORMAL", "TANGENT", "BITANGENT",
        "COLOR0", "COLOR1", "COLOR2", "COLOR3",
        "INDICES", "WEIGHT",
        "TEXCOORD0", "TEXCOORD1", "TEXCOORD2", "TEXCOORD3",
        "TEXCOORD4", "TEXCOORD5", "TEXCOORD6", "TEXCOORD7"
    };
    if (attr < (uint16_t)(sizeof(k) / sizeof(k[0]))) return k[attr];
    return "ATTR?";
}

const char *jce_shader_uniform_kind_name(uint8_t kind)
{
    switch (kind) {
    case JCE_SHADER_UNIFORM_SAMPLER: return "sampler";
    case JCE_SHADER_UNIFORM_END:     return "end";
    case JCE_SHADER_UNIFORM_VEC4:    return "vec4";
    case JCE_SHADER_UNIFORM_MAT3:    return "mat3";
    case JCE_SHADER_UNIFORM_MAT4:    return "mat4";
    default:                         return "other";
    }
}

/* -- instruction classes from a DirectX listing ---------------------- */

static bool sr_starts_with(const char *s, const char *pfx)
{
    return strncmp(s, pfx, strlen(pfx)) == 0;
}

static bool sr_is_flow_op(const char *op)
{
    static const char *const k[] = {
        "if", "if_z", "if_nz", "else", "endif", "loop", "endloop", "break",
        "breakc", "breakc_z", "breakc_nz", "continue", "continuec", "ret",
        "retc", "discard", "discard_z", "discard_nz", "switch", "case",
        "default", "endswitch", "call", "callc", "label"
    };
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++)
        if (strcmp(op, k[i]) == 0) return true;
    return false;
}

static bool sr_is_texture_op(const char *op)
{
    /* Prefix match: the mnemonics carry suffixes (sample_l, ld_indexable,
     * gather4_po_c) and enumerating every one would go stale with the next
     * shader model. */
    return sr_starts_with(op, "sample")  || sr_starts_with(op, "ld")
        || sr_starts_with(op, "gather")  || sr_starts_with(op, "resinfo")
        || sr_starts_with(op, "lod")     || sr_starts_with(op, "bufinfo");
}

bool jce_shader_disasm_stats(const char *text, JceShaderDisasmStats *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!text) return false;

    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t      len = eol ? (size_t)(eol - p) : strlen(p);

        size_t i = 0;
        while (i < len && (p[i] == ' ' || p[i] == '\t')) i++;
        size_t s = i;
        while (i < len && p[i] != ' ' && p[i] != '\t' && p[i] != '\r') i++;

        char   op[64];
        size_t oplen = i - s;
        if (oplen > 0 && oplen < sizeof(op)) {
            memcpy(op, p + s, oplen);
            op[oplen] = 0;

            if (op[0] == '/') {
                /* a comment line */
            } else if (sr_starts_with(op, "dcl")) {
                out->declarations++;
            } else if (oplen == 6 && op[2] == '_' && op[4] == '_') {
                /* the ps_5_0 / vs_5_0 / cs_5_0 profile banner */
            } else {
                out->total++;
                if      (sr_is_texture_op(op)) out->texture++;
                else if (sr_is_flow_op(op))    out->flow++;
                else                           out->arithmetic++;
            }
        }

        if (!eol) break;
        p = eol + 1;
    }
    return true;
}

uint32_t jce_shader_reflect_uniform_bytes(const JceShaderReflection *r)
{
    if (!r) return 0u;
    uint32_t bytes = 0u;
    for (uint32_t i = 0; i < r->uniform_count; i++) {
        if (r->uniforms[i].is_sampler) continue;
        if (r->uniforms[i].kind == (uint8_t)JCE_SHADER_UNIFORM_SAMPLER) continue;
        bytes += (uint32_t)r->uniforms[i].reg_count * 16u;
    }
    return bytes;
}
