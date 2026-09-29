/*
 * jce_shader_inspect -- what is actually inside a compiled shader.
 *
 * Unity has "Compile and show code", Unreal has the material stats bar,
 * Godot shows the generated shader.  This tree had none of the three: a
 * .bin went from shaderc into bgfx and nobody could ask it anything, so
 * "why is this material black" and "how expensive is this shader" were both
 * answered by reading the .sc source and guessing.
 *
 * It reports three things, and is careful about which of them it can
 * actually know:
 *
 *   REFLECTION, on every backend.  The uniform table, the vertex attribute
 *   list and the code size come out of the blob itself
 *   (jce_shader_reflect.h), so they are available for D3D, SPIR-V, Metal,
 *   GLSL and ESSL alike.
 *
 *   THE CODE, when the code is source.  GLSL / ESSL blobs carry the final
 *   post-optimiser source; --code prints it.  For bytecode profiles there is
 *   nothing to print and it says so rather than dumping hex.
 *
 *   INSTRUCTION COUNTS, only from a D3D disassembly.  shaderc's --disasm is
 *   DirectX-only and writes <output>.disasm; hand it here with --disasm and
 *   the classes are counted from the assembly text.  Without one, the counts
 *   are ABSENT, not zero -- printing "0 instructions" for a Vulkan shader
 *   would be a lie with a number on it.
 *
 * The D3D disassembler's own trailing "Approximately N instruction slots
 * used" line is NOT used: it reads 0 at every optimisation level this
 * toolchain produces, measured.  The count here comes from the instruction
 * lines.
 */
#include <jce/renderer/jce_shader_reflect.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */

static void *read_all(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    void *buf = malloc((size_t)n + 1u);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    ((char *)buf)[got] = '\0';
    *out_size = got;
    return buf;
}

/* -- reporting ------------------------------------------------------ */

/* The counts and whether any were produced.  jce_shader_disasm_stats()
 * lives in the engine so the editor panel and this tool cannot drift; the
 * `parsed` flag is this program's own, because "no listing was supplied" is
 * a fact about the invocation, not about the listing. */
typedef struct {
    JceShaderDisasmStats s;
    int                  parsed;
} DisasmStats;

static void print_text(const char *path, const JceShaderReflection *r,
                       const DisasmStats *st, const unsigned char *blob,
                       int want_code)
{
    printf("%s\n", path);
    printf("  stage           %s (format v%u)\n",
           jce_shader_stage_magic(r->stage), (unsigned)r->format_version);
    printf("  varying hash    in %08x  out %08x\n",
           (unsigned)r->hash_in, (unsigned)r->hash_out);
    printf("  code            %u bytes, %s\n",
           (unsigned)r->code_size, r->code_is_text ? "source text" : "bytecode");

    unsigned samplers = 0, values = 0;
    for (uint32_t i = 0; i < r->uniform_count; i++) {
        if (r->uniforms[i].is_sampler
         || r->uniforms[i].kind == (uint8_t)JCE_SHADER_UNIFORM_SAMPLER)
            samplers++;
        else
            values++;
    }
    printf("  uniforms        %u declared (%u sampler, %u value), %u bytes",
           (unsigned)r->uniform_total, samplers, values,
           (unsigned)jce_shader_reflect_uniform_bytes(r));
    if (r->attr_present)
        printf(", bgfx uploads %u\n", (unsigned)r->uniform_block_size);
    else
        printf("\n");
    if (r->uniform_count < r->uniform_total)
        printf("  NOTE            only the first %u uniforms are listed\n",
               (unsigned)r->uniform_count);

    for (uint32_t i = 0; i < r->uniform_count; i++) {
        const JceShaderUniformInfo *u = &r->uniforms[i];
        printf("    %-32s %-8s%s reg %u+%u",
               u->name, jce_shader_uniform_kind_name(u->kind),
               u->is_fragment ? " frag" : "     ",
               (unsigned)u->reg_index, (unsigned)u->reg_count);
        if (u->num > 1) printf("  [%u]", (unsigned)u->num);
        printf("\n");
    }

    if (r->attr_present) {
        printf("  attributes      %u", (unsigned)r->attr_count);
        for (uint8_t i = 0; i < r->attr_count
                         && i < JCE_SHADER_REFLECT_MAX_ATTRS; i++)
            printf(" %s", jce_shader_attrib_name(r->attrs[i]));
        printf("\n");
    } else {
        printf("  attributes      (this blob carries no attribute list)\n");
    }

    if (st->parsed) {
        printf("  instructions    %u  (%u arithmetic, %u texture, %u flow;"
               " %u declarations)\n",
               (unsigned)st->s.total, (unsigned)st->s.arithmetic,
               (unsigned)st->s.texture, (unsigned)st->s.flow,
               (unsigned)st->s.declarations);
    } else {
        printf("  instructions    unknown -- pass --disasm <file>."
               " shaderc's --disasm is DirectX only.\n");
    }

    if (want_code) {
        if (r->code_is_text) {
            printf("\n---- code ----\n");
            fwrite(blob + r->code_offset, 1, r->code_size, stdout);
            printf("\n");
        } else {
            printf("\n---- code ----\n(bytecode, %u bytes -- nothing to"
                   " print. Compile for a GLSL/ESSL profile to read the"
                   " source, or pass --disasm for D3D assembly.)\n",
                   (unsigned)r->code_size);
        }
    }
}

static void json_escape(const char *s)
{
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if      (ch == '"')  fputs("\\\"", stdout);
        else if (ch == '\\') fputs("\\\\", stdout);
        else if (ch == '\n') fputs("\\n", stdout);
        else if (ch == '\r') fputs("\\r", stdout);
        else if (ch == '\t') fputs("\\t", stdout);
        else if (ch < 0x20)  printf("\\u%04x", ch);
        else                 fputc((int)ch, stdout);
    }
}

static void print_json(const char *path, const JceShaderReflection *r,
                       const DisasmStats *st, const unsigned char *blob,
                       int want_code)
{
    printf("{\n  \"file\": \"");
    json_escape(path);
    printf("\",\n");
    printf("  \"stage\": \"%s\",\n", jce_shader_stage_magic(r->stage));
    printf("  \"format_version\": %u,\n", (unsigned)r->format_version);
    printf("  \"hash_in\": %u,\n  \"hash_out\": %u,\n",
           (unsigned)r->hash_in, (unsigned)r->hash_out);
    printf("  \"code_size\": %u,\n", (unsigned)r->code_size);
    printf("  \"code_is_text\": %s,\n", r->code_is_text ? "true" : "false");
    printf("  \"uniform_total\": %u,\n", (unsigned)r->uniform_total);
    printf("  \"uniform_bytes\": %u,\n",
           (unsigned)jce_shader_reflect_uniform_bytes(r));
    /* null, not 0: a blob with no trailer does not SAY zero bytes. */
    if (r->attr_present)
        printf("  \"uniform_block_size\": %u,\n",
               (unsigned)r->uniform_block_size);
    else
        printf("  \"uniform_block_size\": null,\n");
    printf("  \"uniforms\": [\n");
    for (uint32_t i = 0; i < r->uniform_count; i++) {
        const JceShaderUniformInfo *u = &r->uniforms[i];
        printf("    {\"name\": \"");
        json_escape(u->name);
        printf("\", \"kind\": \"%s\", \"fragment\": %s, \"sampler\": %s,"
               " \"reg_index\": %u, \"reg_count\": %u, \"num\": %u}%s\n",
               jce_shader_uniform_kind_name(u->kind),
               u->is_fragment ? "true" : "false",
               u->is_sampler  ? "true" : "false",
               (unsigned)u->reg_index, (unsigned)u->reg_count,
               (unsigned)u->num,
               (i + 1 < r->uniform_count) ? "," : "");
    }
    printf("  ],\n");
    printf("  \"attributes_present\": %s,\n",
           r->attr_present ? "true" : "false");
    printf("  \"attributes\": [");
    for (uint8_t i = 0; i < r->attr_count
                     && i < JCE_SHADER_REFLECT_MAX_ATTRS; i++)
        printf("%s\"%s\"", i ? ", " : "", jce_shader_attrib_name(r->attrs[i]));
    printf("],\n");
    if (st->parsed) {
        printf("  \"instructions\": {\"total\": %u, \"arithmetic\": %u,"
               " \"texture\": %u, \"flow\": %u, \"declarations\": %u},\n",
               (unsigned)st->s.total, (unsigned)st->s.arithmetic,
               (unsigned)st->s.texture, (unsigned)st->s.flow,
               (unsigned)st->s.declarations);
    } else {
        printf("  \"instructions\": null,\n");
    }
    printf("  \"code\": ");
    if (want_code && r->code_is_text) {
        printf("\"");
        /* The code section is NUL-free by construction (code_is_text
         * refuses an embedded NUL), so it is safe to walk as a string. */
        const char *code = (const char *)blob + r->code_offset;
        for (uint32_t i = 0; i < r->code_size; i++) {
            char one[2];
            one[0] = code[i];
            one[1] = '\0';
            json_escape(one);
        }
        printf("\"\n");
    } else {
        printf("null\n");
    }
    printf("}\n");
}

/* ------------------------------------------------------------------ */

static int usage(void)
{
    fprintf(stderr,
        "usage: jce_shader_inspect <shader.bin> [options]\n"
        "\n"
        "  --json              machine-readable output\n"
        "  --code              include the code section (source profiles only)\n"
        "  --disasm <file>     a shaderc --disasm listing for instruction counts\n"
        "  --self-test         parse a synthetic blob and a truncated one\n"
        "\n"
        "shaderc writes the disassembly to <output>.bin.disasm and only for\n"
        "DirectX profiles; without one the instruction counts are reported as\n"
        "unknown rather than as zero.\n");
    return 2;
}

/* A blob built here, byte by byte, so the parser is exercised without a
 * shaderc on the machine -- and so the TRUNCATION path is exercised at all,
 * which no real compiler output would ever do. */
static int self_test(void)
{
    unsigned char b[128];
    size_t n = 0;
    b[n++] = 'F'; b[n++] = 'S'; b[n++] = 'H'; b[n++] = 11;
    b[n++] = 0x11; b[n++] = 0x22; b[n++] = 0x33; b[n++] = 0x44;   /* hash in  */
    b[n++] = 0; b[n++] = 0; b[n++] = 0; b[n++] = 0;               /* hash out */
    b[n++] = 1; b[n++] = 0;                                       /* 1 uniform*/
    b[n++] = 5; memcpy(b + n, "u_foo", 5); n += 5;
    b[n++] = 0x12;            /* vec4 | fragment            */
    b[n++] = 1;               /* num                        */
    b[n++] = 4; b[n++] = 0;   /* reg index 4                */
    b[n++] = 2; b[n++] = 0;   /* reg count 2                */
    b[n++] = 0; b[n++] = 0;   /* texInfo                    */
    b[n++] = 0; b[n++] = 0;   /* texFormat                  */
    const char *code = "void main(){}";
    unsigned cs = (unsigned)strlen(code);
    b[n++] = (unsigned char)(cs & 0xFF); b[n++] = 0; b[n++] = 0; b[n++] = 0;
    memcpy(b + n, code, cs); n += cs;
    b[n++] = 0;               /* code NUL                   */

    JceShaderReflection r;
    int fails = 0;

    if (!jce_shader_reflect(b, n, &r)) {
        fprintf(stderr, "self-test: synthetic blob rejected\n"); fails++;
    } else {
        if (r.stage != JCE_SHADER_STAGE_FRAGMENT) {
            fprintf(stderr, "self-test: stage\n"); fails++;
        }
        if (r.uniform_total != 1 || r.uniform_count != 1
         || strcmp(r.uniforms[0].name, "u_foo") != 0) {
            fprintf(stderr, "self-test: uniform table\n"); fails++;
        }
        if (r.uniforms[0].kind != (uint8_t)JCE_SHADER_UNIFORM_VEC4
         || !r.uniforms[0].is_fragment || r.uniforms[0].reg_count != 2) {
            fprintf(stderr, "self-test: uniform decode\n"); fails++;
        }
        if (jce_shader_reflect_uniform_bytes(&r) != 32u) {
            fprintf(stderr, "self-test: uniform bytes\n"); fails++;
        }
        if (!r.code_is_text || r.code_size != cs) {
            fprintf(stderr, "self-test: code section\n"); fails++;
        }
        if (r.attr_present) {
            fprintf(stderr, "self-test: invented an attribute trailer\n");
            fails++;
        }
    }

    /* Every truncation must be REFUSED, not answered partially.  This is the
     * half a real .bin can never test, and the half a mis-sized record would
     * turn into confident nonsense. */
    for (size_t cut = 1; cut < n; cut++) {
        JceShaderReflection t;
        if (jce_shader_reflect(b, cut, &t)) {
            fprintf(stderr, "self-test: accepted a blob truncated to %u/%u\n",
                    (unsigned)cut, (unsigned)n);
            fails++;
            break;
        }
    }

    /* Wrong magic. */
    unsigned char bad[32];
    memset(bad, 0, sizeof(bad));
    memcpy(bad, "XYZ", 3);
    JceShaderReflection t2;
    if (jce_shader_reflect(bad, sizeof(bad), &t2)) {
        fprintf(stderr, "self-test: accepted a non-shader blob\n"); fails++;
    }

    if (fails == 0) printf("jce_shader_inspect: self-test OK\n");
    return fails ? 1 : 0;
}

int main(int argc, char **argv)
{
    const char *bin_path    = NULL;
    const char *disasm_path = NULL;
    int         want_json   = 0;
    int         want_code   = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0)        want_json = 1;
        else if (strcmp(argv[i], "--code") == 0)   want_code = 1;
        else if (strcmp(argv[i], "--self-test") == 0) return self_test();
        else if (strcmp(argv[i], "--disasm") == 0) {
            if (i + 1 >= argc) return usage();
            disasm_path = argv[++i];
        } else if (argv[i][0] == '-') {
            return usage();
        } else if (!bin_path) {
            bin_path = argv[i];
        } else {
            return usage();
        }
    }
    if (!bin_path) return usage();

    size_t         size = 0;
    unsigned char *blob = (unsigned char *)read_all(bin_path, &size);
    if (!blob) {
        fprintf(stderr, "jce_shader_inspect: cannot read %s\n", bin_path);
        return 1;
    }

    JceShaderReflection r;
    if (!jce_shader_reflect(blob, size, &r)) {
        fprintf(stderr,
            "jce_shader_inspect: %s is not a bgfx shader blob, or it is\n"
            "truncated. Expected a .bin written by shaderc -o.\n", bin_path);
        free(blob);
        return 1;
    }

    DisasmStats st;
    memset(&st, 0, sizeof(st));
    char *disasm = NULL;
    if (disasm_path) {
        size_t dsz = 0;
        disasm = (char *)read_all(disasm_path, &dsz);
        if (!disasm) {
            fprintf(stderr, "jce_shader_inspect: cannot read %s\n", disasm_path);
            free(blob);
            return 1;
        }
        jce_shader_disasm_stats(disasm, &st.s);
        st.parsed = 1;
    }

    if (want_json) print_json(bin_path, &r, &st, blob, want_code);
    else           print_text(bin_path, &r, &st, blob, want_code);

    free(disasm);
    free(blob);
    return 0;
}
