#include "cli/jce_tool_main.h"
#include "cli/jce_tool_io.h"
/* jce_bin2obj.c
 *
 * Host tool that wraps an arbitrary binary file into either:
 *   - a COFF .obj exporting `<sym>` and `<sym>_size`  (MSVC link)
 *   - a GAS .S  file using `.incbin` and exporting the same symbols
 *
 * Used by shared SDK embedding helpers to bake PAK/bundle archives into the
 * application exe so a built project is genuinely a single file.
 *
 * The existing COFF writer also supports 32-bit and ARM targets. GAS
 * output is preprocessed by the target compiler for native symbol prefixes,
 * read-only sections and size_t width.
 *
 * Usage:
 *   jce_bin2obj --input  <file>
 *               --symbol <ident>
 *               --output <out>
 *               --format coff|asm|c
 *               [--arch  x64|x86|arm64|arm]    (default: x64; coff only)
 */


#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- dynamic byte buffer ---------------- */

typedef struct {
	uint8_t *data;
	size_t   size;
	size_t   cap;
} ByteBuf;

static void bb_reserve(ByteBuf *b, size_t need)
{
	if (b->cap >= need) return;
	size_t cap = b->cap ? b->cap : 4096;
	while (cap < need) cap *= 2;
	uint8_t *n = (uint8_t *)realloc(b->data, cap);
	if (!n) { fprintf(stderr, "[jce_bin2obj] OOM\n"); exit(2); }
	b->data = n; b->cap = cap;
}
static void bb_push(ByteBuf *b, uint8_t v)
{
	bb_reserve(b, b->size + 1);
	b->data[b->size++] = v;
}
static void bb_append(ByteBuf *b, const void *src, size_t n)
{
	bb_reserve(b, b->size + n);
	memcpy(b->data + b->size, src, n);
	b->size += n;
}
static void buf_le16(ByteBuf *b, uint16_t v) { bb_push(b, (uint8_t)v); bb_push(b, (uint8_t)(v >> 8)); }
static void buf_le32(ByteBuf *b, uint32_t v)
{
	bb_push(b, (uint8_t)v);
	bb_push(b, (uint8_t)(v >> 8));
	bb_push(b, (uint8_t)(v >> 16));
	bb_push(b, (uint8_t)(v >> 24));
}

/* ---------------- COFF generator ---------------- */

static int coff_pointer_size(uint16_t machine)
{
	switch (machine) {
		case 0x8664: /* AMD64 */
		case 0xAA64: /* ARM64 */
			return 8;
		default:
			return 4;
	}
}

static uint16_t arch_to_machine(const char *arch)
{
	if (strcmp(arch, "x64")   == 0) return 0x8664;
	if (strcmp(arch, "x86")   == 0) return 0x014c;
	if (strcmp(arch, "arm64") == 0) return 0xAA64;
	if (strcmp(arch, "arm")   == 0) return 0x01c4;
	return 0;
}

static void generate_coff_obj(const uint8_t *blob, size_t blob_size,
                              uint16_t machine, const char *sym_prefix,
                              ByteBuf *obj)
{
	const int ptr_size = coff_pointer_size(machine);
	const uint64_t size_offset =
		((uint64_t)blob_size + (uint64_t)ptr_size - 1u) & ~((uint64_t)ptr_size - 1u);
	const uint64_t rdata_size = size_offset + (uint64_t)ptr_size;
	const uint64_t rdata_aligned = (rdata_size + 3u) & ~(uint64_t)3;

	/* MSVC i386 C symbols use a leading underscore; other COFF targets do not. */
	const char *prefix = machine == 0x014c ? "_" : "";
	char sym_data[96], sym_size[112];
	snprintf(sym_data, sizeof(sym_data), "%s%s", prefix, sym_prefix);
	snprintf(sym_size, sizeof(sym_size), "%s%s_size", prefix, sym_prefix);

	const uint32_t strtab_off_data = 4;
	const uint32_t strtab_off_size = strtab_off_data + (uint32_t)strlen(sym_data) + 1;
	const uint32_t strtab_total    = strtab_off_size + (uint32_t)strlen(sym_size) + 1;

	const uint32_t coff_header_size = 20;
	const uint32_t section_hdr_size = 40;
	const uint32_t section_data_off = coff_header_size + section_hdr_size;
	const uint32_t symtab_off       = section_data_off + (uint32_t)rdata_aligned;
	const uint32_t num_symbols      = 2;

	bb_reserve(obj, (size_t)symtab_off + 36 + strtab_total);

	/* COFF File Header */
	buf_le16(obj, machine);
	buf_le16(obj, 1);
	buf_le32(obj, 0);
	buf_le32(obj, symtab_off);
	buf_le32(obj, num_symbols);
	buf_le16(obj, 0);
	buf_le16(obj, 0);

	/* Section Header: .rdata */
	const char sec_name[8] = {'.','r','d','a','t','a',0,0};
	bb_append(obj, sec_name, 8);
	buf_le32(obj, 0);
	buf_le32(obj, 0);
	buf_le32(obj, (uint32_t)rdata_size);
	buf_le32(obj, section_data_off);
	buf_le32(obj, 0);
	buf_le32(obj, 0);
	buf_le16(obj, 0);
	buf_le16(obj, 0);
	buf_le32(obj, 0x40500040u);  /* IMAGE_SCN_CNT_INITIALIZED_DATA | MEM_READ | ALIGN_16 */

	/* Section Data */
	bb_append(obj, blob, blob_size);
	while (obj->size < (size_t)(section_data_off + size_offset))
		bb_push(obj, 0);
	for (int i = 0; i < ptr_size; ++i)
		bb_push(obj, (uint8_t)((uint64_t)blob_size >> (i * 8)));
	while (obj->size < (size_t)symtab_off)
		bb_push(obj, 0);

	/* Symbol Table: <sym>  (external, in .rdata, offset 0) */
	buf_le32(obj, 0);
	buf_le32(obj, strtab_off_data);
	buf_le32(obj, 0);
	buf_le16(obj, 1);
	buf_le16(obj, 0);
	bb_push(obj, 2);
	bb_push(obj, 0);

	/* Symbol Table: <sym>_size  (external, in .rdata, offset size_offset) */
	buf_le32(obj, 0);
	buf_le32(obj, strtab_off_size);
	buf_le32(obj, (uint32_t)size_offset);
	buf_le16(obj, 1);
	buf_le16(obj, 0);
	bb_push(obj, 2);
	bb_push(obj, 0);

	/* String Table */
	buf_le32(obj, strtab_total);
	bb_append(obj, sym_data, strlen(sym_data) + 1);
	bb_append(obj, sym_size, strlen(sym_size) + 1);
}

/* ---------------- GAS .S generator (for clang/gcc) ---------------- */

static void generate_asm_incbin(const char *input_path, const char *sym,
                                size_t blob_size, ByteBuf *out)
{
	char line[1024];
	const char *header =
		"/* Auto-generated by jce_bin2obj -- DO NOT EDIT */\n"
		"#define JCE_JOIN_IMPL(a, b) a##b\n"
		"#define JCE_JOIN(a, b) JCE_JOIN_IMPL(a, b)\n"
		"#define JCE_SYMBOL(name) JCE_JOIN(__USER_LABEL_PREFIX__, name)\n"
		"#if defined(__APPLE__)\n"
		".section __TEXT,__const\n"
		"#else\n"
		".section .rodata\n"
		"#endif\n";
	bb_append(out, header, strlen(header));
	snprintf(line, sizeof(line),
		".global JCE_SYMBOL(%s)\n.global JCE_SYMBOL(%s_size)\n"
		".p2align 4\nJCE_SYMBOL(%s):\n    .incbin \"",
		sym, sym, sym);
	bb_append(out, line, strlen(line));
	/* GAS reads a quoted path. Normalize native Windows separators and
	 * escape quotes instead of truncating an absolute asset path. */
	for (const char *p = input_path; *p; ++p) {
		if (*p == '"') bb_push(out, '\\');
		bb_push(out, (uint8_t)(*p == '\\' ? '/' : *p));
	}
	snprintf(line, sizeof(line),
		"\"\n.p2align 3\nJCE_SYMBOL(%s_size):\n"
		"#if __SIZEOF_POINTER__ == 8\n    .quad %zu\n"
		"#elif __SIZEOF_POINTER__ == 4\n    .long %zu\n"
		"#else\n#error Unsupported native pointer width\n#endif\n"
		"#if defined(__ELF__)\n.section .note.GNU-stack,\"\"\n#endif\n",
		sym, blob_size, blob_size);
	bb_append(out, line, strlen(line));
}

/* Portable C fallback for targets without a native assembler (Web). */
static void generate_c_array(const uint8_t *blob, size_t size,
                             const char *sym, ByteBuf *out)
{
    char line[256];
    bb_append(out, "#include <stddef.h>\n", 20);
    snprintf(line, sizeof(line), "const unsigned char %s[] = {\n", sym);
    bb_append(out, line, strlen(line));
    if (!size) bb_append(out, "0", 1);
    for (size_t i = 0; i < size; ++i) {
        snprintf(line, sizeof(line), "0x%02x,%s", blob[i], (i % 16 == 15) ? "\n" : "");
        bb_append(out, line, strlen(line));
    }
    snprintf(line, sizeof(line), "\n};\nconst size_t %s_size = %zu;\n", sym, size);
    bb_append(out, line, strlen(line));
}

/* ---------------- io helpers ---------------- */

static int read_file(const char *path, uint8_t **out, size_t *out_size)
{
	JceToolFile *f = jce_tool_fopen(path, "rb");
	if (!f) { fprintf(stderr, "[jce_bin2obj] cannot open input: %s\n", path); return 0; }
	if (jce_tool_fseek(f, 0, SEEK_END) != 0) { jce_tool_fclose(f); return 0; }
	int64_t sz = jce_tool_ftell(f);
	if (sz < 0 || (uint64_t)sz >= SIZE_MAX) { jce_tool_fclose(f); return 0; }
	if (jce_tool_fseek(f, 0, SEEK_SET) != 0) { jce_tool_fclose(f); return 0; }
	uint8_t *buf = (uint8_t *)malloc((size_t)sz + 1);
	if (!buf) { jce_tool_fclose(f); return 0; }
	size_t got = jce_tool_fread(buf, 1, (size_t)sz, f);
	jce_tool_fclose(f);
	if (got != (size_t)sz) { free(buf); return 0; }
	*out = buf; *out_size = (size_t)sz;
	return 1;
}

static int write_file(const char *path, const void *data, size_t n)
{
	JceToolFile *f = jce_tool_fopen(path, "wb");
	if (!f) { fprintf(stderr, "[jce_bin2obj] cannot open output: %s\n", path); return 0; }
	size_t wrote = jce_tool_fwrite(data, 1, n, f);
	jce_tool_fclose(f);
	return wrote == n;
}

static void usage(void)
{
	fprintf(stderr,
		"Usage: jce_bin2obj --input <file> --symbol <ident> --output <out>\n"
		"                   --format coff|asm|c\n"
		"                  [--arch   x64|x86|arm64|arm]   (coff only; default x64)\n");
}

int jce_tool_main(int argc, char *argv[])
{
	const char *input  = NULL;
	const char *sym    = NULL;
	const char *output = NULL;
	const char *format = NULL;
	const char *arch   = "x64";

	for (int i = 1; i < argc; ++i) {
		const char *a = argv[i];
		const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
		if      (strcmp(a, "--input")  == 0 && v) { input  = v; ++i; }
		else if (strcmp(a, "--symbol") == 0 && v) { sym    = v; ++i; }
		else if (strcmp(a, "--output") == 0 && v) { output = v; ++i; }
		else if (strcmp(a, "--format") == 0 && v) { format = v; ++i; }
		else if (strcmp(a, "--arch")   == 0 && v) { arch   = v; ++i; }
		else { fprintf(stderr, "[jce_bin2obj] unknown arg: %s\n", a); usage(); return 1; }
	}
	if (!input || !sym || !output || !format) { usage(); return 1; }
	if (strlen(sym) > 63 || strspn(sym,
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") != strlen(sym) ||
	    (sym[0] >= '0' && sym[0] <= '9') || !sym[0]) {
		fprintf(stderr, "[jce_bin2obj] invalid C symbol: %s\n", sym);
		return 1;
	}
	if (strchr(input, '\n') || strchr(input, '\r')) {
		fprintf(stderr, "[jce_bin2obj] invalid input path\n");
		return 1;
	}

	uint8_t *blob = NULL; size_t blob_size = 0;
	if (!read_file(input, &blob, &blob_size)) return 2;

	ByteBuf out = {0};
	if (strcmp(format, "coff") == 0) {
		uint16_t machine = arch_to_machine(arch);
		if (!machine) { fprintf(stderr, "[jce_bin2obj] bad --arch: %s\n", arch); free(blob); return 1; }
		generate_coff_obj(blob, blob_size, machine, sym, &out);
	} else if (strcmp(format, "asm") == 0) {
		generate_asm_incbin(input, sym, blob_size, &out);
	} else if (strcmp(format, "c") == 0) {
		generate_c_array(blob, blob_size, sym, &out);
	} else {
		fprintf(stderr, "[jce_bin2obj] bad --format: %s\n", format);
		free(blob);
		return 1;
	}

	int ok = write_file(output, out.data, out.size);
	free(blob);
	free(out.data);
	if (!ok) return 3;
	fprintf(stdout, "[jce_bin2obj] %s -> %s (%zu bytes blob, sym=%s)\n",
	        input, output, blob_size, sym);
	return 0;
}
