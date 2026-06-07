/* jce_pak.c
 *
 * Host tool that packs resource files into a JCE PAK archive.
 *
 *   1. Recursively enumerates RESOURCE_DIR
 *   2. ZSTD-compresses each file
 *   3. Indexes paths with XXH3_64bits
 *   4. Writes a binary .pak (see jce_pak_format.h)
 *   5. Generates embedded_assets.h  (extern declarations)
 *   6. Generates _assets_manifest.cmake (per-asset sizes)
 *   7. Optionally generates a COFF .obj wrapping the .pak blob
 *
 * Build:
 *   Links zstd (static) and xxhash.
 *
 * CLI:
 *   jce_pak --resource-dir <dir>
 *           --pak-file     <out.pak>
 *           --obj-file     <out.obj>        (optional, COFF output)
 *           --header-file  <out.h>
 *           --manifest-file <out.cmake>
 *           --exclude-segment <name>        (optional, repeatable)
 *           --obj-format   coff|none        (default: none)
 *           --obj-arch     x64|arm64|x86|arm (default: x64)
 */

/* Ensure POSIX functions (strdup, etc.) are declared on glibc/Linux. */
#ifndef _WIN32
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <dirent.h>
#  include <sys/stat.h>
#  define _strdup strdup
#endif

#include "resource/jce_pak_format.h"
#include "resource/jce_archive_format.h"

#include <jce/resource/jce_archive.h>
#include <jce/resource/jce_archive_cook.h>
#include <jce/os/core/jce_alloc.h>

#include <xxhash.h>
#include <zstd.h>

/* Logging stub: the archive writer/reader/dict/mmap TUs reference
 * jce_log_write(); this host tool has no async log subsystem, so route
 * warnings/errors to stderr and drop the rest. */
#include <stdarg.h>
void jce_log_write(int level, const char *tag, const char *file, int line,
                   const char *fmt, ...) {
    (void)file; (void)line;
    if (level < 4) return; /* below WARN: ignore (levels are ascending) */
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%s] ", tag ? tag : "?");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* ================================================================== */
/* Dynamic byte buffer                                                 */
/* ================================================================== */

typedef struct {
    uint8_t *data;
    size_t   size;
    size_t   cap;
} ByteBuf;

static void bb_init(ByteBuf *b) {
    b->data = NULL;
    b->size = 0;
    b->cap  = 0;
}

static void bb_reserve(ByteBuf *b, size_t need) {
    if (b->cap >= need) return;
    size_t nc = b->cap ? b->cap : 256;
    while (nc < need) nc *= 2;
    b->data = (uint8_t *)realloc(b->data, nc);
    if (!b->data) { fprintf(stderr, "[jce_pak] out of memory\n"); exit(1); }
    b->cap = nc;
}

static void bb_push(ByteBuf *b, uint8_t v) {
    bb_reserve(b, b->size + 1);
    b->data[b->size++] = v;
}

static void bb_append(ByteBuf *b, const void *src, size_t n) {
    bb_reserve(b, b->size + n);
    memcpy(b->data + b->size, src, n);
    b->size += n;
}

static void bb_free(ByteBuf *b) {
    free(b->data);
    b->data = NULL;
    b->size = b->cap = 0;
}

/* ================================================================== */
/* Dynamic string list                                                 */
/* ================================================================== */

typedef struct {
    char **items;
    size_t count;
    size_t cap;
} StrList;

static void sl_init(StrList *l) {
    l->items = NULL;
    l->count = 0;
    l->cap   = 0;
}

static void sl_push(StrList *l, const char *s) {
    if (l->count == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 64;
        l->items = (char **)realloc(l->items, l->cap * sizeof(char *));
        if (!l->items) { fprintf(stderr, "[jce_pak] out of memory\n"); exit(1); }
    }
    l->items[l->count++] = _strdup(s);
}

static void sl_free(StrList *l) {
    for (size_t i = 0; i < l->count; i++) free(l->items[i]);
    free(l->items);
    l->items = NULL;
    l->count = l->cap = 0;
}

static int sl_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* ================================================================== */
/* Little-endian write helpers                                         */
/* ================================================================== */

static void buf_le16(ByteBuf *b, uint16_t v) {
    bb_push(b, (uint8_t)(v));
    bb_push(b, (uint8_t)(v >> 8));
}

static void buf_le32(ByteBuf *b, uint32_t v) {
    bb_push(b, (uint8_t)(v));
    bb_push(b, (uint8_t)(v >> 8));
    bb_push(b, (uint8_t)(v >> 16));
    bb_push(b, (uint8_t)(v >> 24));
}

/* ================================================================== */
/* File I/O helpers                                                    */
/* ================================================================== */

static uint8_t *read_file_bin(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[jce_pak] ERROR: cannot open %s\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); fprintf(stderr, "[jce_pak] out of memory\n"); exit(1); }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        free(buf);
        fprintf(stderr, "[jce_pak] ERROR: short read on %s\n", path);
        exit(1);
    }
    fclose(f);
    *out_size = (size_t)sz;
    return buf;
}

/* Write atomically: tmp + compare + rename. */
static void write_file_atomic(const char *path, const void *data, size_t size) {
    /* Build tmp path. */
    size_t plen = strlen(path);
    char *tmp = (char *)malloc(plen + 5);
    if (!tmp) { fprintf(stderr, "[jce_pak] out of memory\n"); exit(1); }
    sprintf(tmp, "%s.tmp", path);

    FILE *f = fopen(tmp, "wb");
    if (!f) {
        fprintf(stderr, "[jce_pak] ERROR: cannot write %s\n", tmp);
        free(tmp);
        exit(1);
    }
    fwrite(data, 1, size, f);
    fclose(f);

    /* Compare with existing; skip overwrite if identical (preserves mtime). */
    FILE *existing = fopen(path, "rb");
    if (existing) {
        fseek(existing, 0, SEEK_END);
        long esz = ftell(existing);
        if ((size_t)esz == size) {
            fseek(existing, 0, SEEK_SET);
            uint8_t *ebuf = (uint8_t *)malloc(size);
            if (ebuf) {
                if (fread(ebuf, 1, size, existing) == size &&
                    memcmp(ebuf, data, size) == 0) {
                    fclose(existing);
                    free(ebuf);
                    remove(tmp);
                    free(tmp);
                    return;
                }
                free(ebuf);
                fclose(existing);
            } else {
                fclose(existing);
            }
        } else {
            fclose(existing);
        }
    }

    remove(path);
    rename(tmp, path);
    free(tmp);
}

/* ================================================================== */
/* Path helpers                                                        */
/* ================================================================== */

/* Normalise separators: backslash -> forward slash, in-place. */
static void normalise_sep(char *s) {
    for (; *s; ++s)
        if (*s == '\\') *s = '/';
}

/* Compute relative path: strip prefix from full path.
 * Returns malloc'd string with forward slashes. */
static char *make_relative(const char *full, const char *base) {
    size_t blen = strlen(base);
    /* Skip base prefix. */
    const char *rel = full;
    if (strncmp(full, base, blen) == 0) {
        rel = full + blen;
        while (*rel == '/' || *rel == '\\') rel++;
    }
    char *out = _strdup(rel);
    if (!out) { fprintf(stderr, "[jce_pak] out of memory\n"); exit(1); }
    normalise_sep(out);
    return out;
}

/* Check if a normalised relative path contains hidden segments. */
static int is_hidden(const char *rel) {
    if (rel[0] == '.') return 1;
    if (strstr(rel, "/.")) return 1;
    return 0;
}

/* Check if rel path contains a segment exactly matching segment_name.
 * Example: "raw_assets/foo.png" contains segment "raw_assets". */
static int path_has_segment(const char *rel, const char *segment_name) {
    size_t seg_len = strlen(segment_name);
    const char *p = rel;

    while (*p) {
        const char *q = p;
        while (*q && *q != '/') q++;
        if ((size_t)(q - p) == seg_len && strncmp(p, segment_name, seg_len) == 0)
            return 1;

        if (*q == '\0') break;
        p = q + 1;
    }
    return 0;
}

/* ================================================================== */
/* Recursive directory enumeration                                     */
/* ================================================================== */

#ifdef _WIN32

static void enumerate_files(const char *dir, StrList *out) {
    char pattern[MAX_PATH + 3];
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        if (fd.cFileName[0] == '.') continue;

        char child[MAX_PATH];
        snprintf(child, sizeof(child), "%s\\%s", dir, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            enumerate_files(child, out);
        } else {
            sl_push(out, child);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

#else /* POSIX */

static void enumerate_files(const char *dir, StrList *out) {
    DIR *d = opendir(dir);
    if (!d) return;

    const struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        size_t need = strlen(dir) + 1 + strlen(ent->d_name) + 1;
        char *child = (char *)malloc(need);
        if (!child) { closedir(d); fprintf(stderr, "[jce_pak] out of memory\n"); exit(1); }
        snprintf(child, need, "%s/%s", dir, ent->d_name);

        struct stat st;
        if (stat(child, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                enumerate_files(child, out);
            } else if (S_ISREG(st.st_mode)) {
                sl_push(out, child);
            }
        }
        free(child);
    }
    closedir(d);
}

#endif

/* ================================================================== */
/* Per-asset record                                                    */
/* ================================================================== */

typedef struct {
    char *rel_path;
    uint64_t path_hash;
    uint64_t original_size;
    uint8_t *compressed;
    size_t compressed_size;
    uint32_t name_offset;
    uint32_t name_length;
    uint64_t data_offset;
    uint32_t flags;        /* JPAK_FLAG_*                            */
    uint64_t content_hash; /* XXH3_64bits of original bytes          */
} AssetEntry;

/* ================================================================== */
/* COFF .obj generator                                                 */
/* ================================================================== */

#define COFF_MACHINE_AMD64 0x8664
#define COFF_MACHINE_I386  0x014C
#define COFF_MACHINE_ARM64 0xAA64
#define COFF_MACHINE_ARMNT 0x01C4

static uint16_t coff_machine_from_arch(const char *arch) {
    if (strcmp(arch, "x64") == 0 || strcmp(arch, "x86_64") == 0 || strcmp(arch, "amd64") == 0)
        return COFF_MACHINE_AMD64;
    if (strcmp(arch, "x86") == 0 || strcmp(arch, "i386") == 0)
        return COFF_MACHINE_I386;
    if (strcmp(arch, "arm64") == 0 || strcmp(arch, "aarch64") == 0)
        return COFF_MACHINE_ARM64;
    if (strcmp(arch, "arm") == 0 || strcmp(arch, "armv7") == 0)
        return COFF_MACHINE_ARMNT;
    fprintf(stderr, "[jce_pak] unknown --obj-arch: %s\n", arch);
    exit(1);
}

static int coff_pointer_size(uint16_t machine) {
    switch (machine) {
        case COFF_MACHINE_I386:
        case COFF_MACHINE_ARMNT: return 4;
        default:                 return 8;
    }
}

static void generate_coff_obj(const uint8_t *pak_data, size_t pak_size,
                              uint16_t machine, const char *sym_prefix,
                              ByteBuf *obj)
{
    const uint64_t blob_size = pak_size;
    const int ptr_size = coff_pointer_size(machine);

    const uint64_t size_offset =
        (blob_size + (uint64_t)ptr_size - 1u) & ~((uint64_t)ptr_size - 1u);
    const uint64_t rdata_size = size_offset + (uint64_t)ptr_size;
    const uint64_t rdata_aligned = (rdata_size + 3u) & ~(uint64_t)3;

    /* 32-bit x86 (i386) MSVC decorates cdecl C symbols with a leading '_';
     * x64, ARM64 and 32-bit ARM (ARMNT) do not.  Match that decoration so the
     * blob the C code references (e.g. assets_pak_data -> _assets_pak_data on
     * x86) resolves at link time. */
    const char *us = (machine == COFF_MACHINE_I386) ? "_" : "";

    char sym_data_buf[96];
    char sym_size_buf[112];
    snprintf(sym_data_buf, sizeof(sym_data_buf), "%s%s", us, sym_prefix);
    snprintf(sym_size_buf, sizeof(sym_size_buf), "%s%s_size", us, sym_prefix);
    const char *sym_data = sym_data_buf;
    const char *sym_size_name = sym_size_buf;

    const uint32_t strtab_off_data = 4;
    const uint32_t strtab_off_size =
        strtab_off_data + (uint32_t)strlen(sym_data) + 1;
    const uint32_t strtab_total =
        strtab_off_size + (uint32_t)strlen(sym_size_name) + 1;

    const uint32_t coff_header_size = 20;
    const uint32_t section_hdr_size = 40;
    const uint32_t section_data_off = coff_header_size + section_hdr_size;
    const uint32_t symtab_off = section_data_off + (uint32_t)rdata_aligned;
    const uint32_t num_symbols = 2;

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
    buf_le32(obj, 0x40500040u);

    /* Section Data */
    bb_append(obj, pak_data, pak_size);
    while (obj->size < (size_t)(section_data_off + size_offset))
        bb_push(obj, 0);
    for (int i = 0; i < ptr_size; ++i)
        bb_push(obj, (uint8_t)(blob_size >> (i * 8)));
    while (obj->size < (size_t)symtab_off)
        bb_push(obj, 0);

    /* Symbol Table */
    buf_le32(obj, 0);
    buf_le32(obj, strtab_off_data);
    buf_le32(obj, 0);
    buf_le16(obj, 1);
    buf_le16(obj, 0);
    bb_push(obj, 2);
    bb_push(obj, 0);

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
    bb_append(obj, sym_size_name, strlen(sym_size_name) + 1);
}

/* ================================================================== */
/* C-array generator                                                   */
/* ================================================================== */

static void generate_c_array(const uint8_t *pak_data, size_t pak_size,
                             const char *sym_prefix, ByteBuf *out)
{
    char header[256];
    snprintf(header, sizeof(header),
        "/* Auto-generated by jce_pak -- DO NOT EDIT */\n"
        "#include <stddef.h>\n\n"
        "const unsigned char %s[] = {\n", sym_prefix);
    bb_append(out, header, strlen(header));

    for (size_t i = 0; i < pak_size; ++i) {
        char hex[16];
        if (i % 16 == 0) bb_append(out, "    ", 4);
        int n = snprintf(hex, sizeof(hex), "0x%02X", pak_data[i]);
        bb_append(out, hex, (size_t)n);
        if (i + 1 < pak_size) bb_push(out, ',');
        if (i % 16 == 15 || i + 1 == pak_size) bb_push(out, '\n');
    }

    char footer[160];
    snprintf(footer, sizeof(footer),
        "};\n\n"
        "const size_t %s_size = sizeof(%s);\n",
        sym_prefix, sym_prefix);
    bb_append(out, footer, strlen(footer));
}

/* ================================================================== */
/* Header / manifest generators                                        */
/* ================================================================== */

static void generate_header(const char *sym_prefix, ByteBuf *out) {
    char h[768];
    snprintf(h, sizeof(h),
        "/* Auto-generated by jce_pak -- DO NOT EDIT */\n"
        "#pragma once\n"
        "#include <stddef.h>\n\n"
        "#ifdef __cplusplus\n"
        "extern \"C\" {\n"
        "#endif\n\n"
        "/* Raw PAK blob linked into the executable (.obj / .incbin). */\n"
        "extern const unsigned char %s[];\n"
        "extern const size_t        %s_size;\n\n"
        "#ifdef __cplusplus\n"
        "}\n"
        "#endif\n",
        sym_prefix, sym_prefix);
    bb_append(out, h, strlen(h));
}

static void generate_manifest(const AssetEntry *entries, size_t count,
                              uint64_t pak_total, ByteBuf *out)
{
    char line[512];
    int n;

    bb_append(out, "# Auto-generated by jce_pak -- DO NOT EDIT\n", 43);

    /* ASSET_PATHS */
    bb_append(out, "set(ASSET_PATHS \"", 17);
    for (size_t i = 0; i < count; ++i) {
        if (i) bb_push(out, ';');
        bb_append(out, entries[i].rel_path, strlen(entries[i].rel_path));
    }
    bb_append(out, "\")\n", 3);

    /* ASSET_SIZES */
    bb_append(out, "set(ASSET_SIZES \"", 17);
    for (size_t i = 0; i < count; ++i) {
        if (i) bb_push(out, ';');
        n = snprintf(line, sizeof(line), "%llu", (unsigned long long)entries[i].original_size);
        bb_append(out, line, (size_t)n);
    }
    bb_append(out, "\")\n", 3);

    /* ASSET_COMPRESSED */
    bb_append(out, "set(ASSET_COMPRESSED \"", 22);
    for (size_t i = 0; i < count; ++i) {
        if (i) bb_push(out, ';');
        n = snprintf(line, sizeof(line), "%llu", (unsigned long long)entries[i].compressed_size);
        bb_append(out, line, (size_t)n);
    }
    bb_append(out, "\")\n", 3);

    /* ASSET_FLAGS (PAK v2: 1 = STORED, 0 = compressed) */
    bb_append(out, "set(ASSET_FLAGS \"", 17);
    for (size_t i = 0; i < count; ++i) {
        if (i)
            bb_push(out, ';');
        n = snprintf(line, sizeof(line), "%u", (unsigned)entries[i].flags);
        bb_append(out, line, (size_t)n);
    }
    bb_append(out, "\")\n", 3);

    /* Totals */
    uint64_t raw_total = 0, comp_total = 0, stored_raw = 0;
    size_t stored_count = 0;
    for (size_t i = 0; i < count; ++i) {
        raw_total  += entries[i].original_size;
        comp_total += entries[i].compressed_size;
        if (entries[i].flags & JPAK_FLAG_STORED) {
            stored_count++;
            stored_raw += entries[i].original_size;
        }
    }

    n = snprintf(line, sizeof(line),
                 "set(ASSET_RAW_TOTAL %llu)\n"
                 "set(ASSET_COMP_TOTAL %llu)\n"
                 "set(ASSET_PAK_TOTAL %llu)\n"
                 "set(ASSET_FILE_COUNT %llu)\n"
                 "set(ASSET_STORED_COUNT %llu)\n"
                 "set(ASSET_STORED_RAW_TOTAL %llu)\n",
                 (unsigned long long)raw_total, (unsigned long long)comp_total,
                 (unsigned long long)pak_total, (unsigned long long)count,
                 (unsigned long long)stored_count, (unsigned long long)stored_raw);
    bb_append(out, line, (size_t)n);
}

/* ================================================================== */
/* Archive inspection / bill-of-materials (--inspect)                  */
/* ================================================================== */

static const char *jarc_comp_name(unsigned c) {
    switch (c) {
        case JARC_COMP_NONE:      return "NONE";
        case JARC_COMP_ZSTD:      return "ZSTD";
        case JARC_COMP_ZSTD_DICT: return "ZSTD_DICT";
        case JARC_COMP_LZ4:       return "LZ4";
        default:                  return "?";
    }
}

/* Append `s` to `b` as a JSON string literal (with surrounding quotes). */
static void json_str(ByteBuf *b, const char *s) {
    bb_push(b, '"');
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
            case '"':  bb_append(b, "\\\"", 2); break;
            case '\\': bb_append(b, "\\\\", 2); break;
            case '\n': bb_append(b, "\\n", 2);  break;
            case '\r': bb_append(b, "\\r", 2);  break;
            case '\t': bb_append(b, "\\t", 2);  break;
            default:
                if (c < 0x20) {
                    char u[8];
                    int n = snprintf(u, sizeof(u), "\\u%04x", c);
                    if (n > 0) bb_append(b, u, (size_t)n);
                } else {
                    bb_push(b, (uint8_t)c);
                }
        }
    }
    bb_push(b, '"');
}

static void bb_printf(ByteBuf *b, const char *fmt, ...) {
    char    stackbuf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(stackbuf, sizeof(stackbuf), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n < sizeof(stackbuf)) {
        bb_append(b, stackbuf, (size_t)n);
        return;
    }
    /* Rare: formatted chunk exceeds the stack buffer — render on the heap so
     * we never silently truncate output. */
    char *heap = (char *)malloc((size_t)n + 1);
    if (!heap) {
        bb_append(b, stackbuf, sizeof(stackbuf) - 1);
        return;
    }
    va_start(ap, fmt);
    vsnprintf(heap, (size_t)n + 1, fmt, ap);
    va_end(ap);
    bb_append(b, heap, (size_t)n);
    free(heap);
}

static void fourcc_str(uint32_t tag, char out[5]) {
    out[0] = (char)(tag & 0xFF);
    out[1] = (char)((tag >> 8) & 0xFF);
    out[2] = (char)((tag >> 16) & 0xFF);
    out[3] = (char)((tag >> 24) & 0xFF);
    out[4] = 0;
}

/* Duplicate-content detection record (spec §6.3 wasted-space audit). Two
 * entries are treated as duplicates when they share both content_crc and
 * original_size, which makes a stray XXH32 collision astronomically unlikely. */
typedef struct {
    uint32_t crc;
    uint32_t orig;
    uint32_t stored;
    uint32_t idx;
    uint64_t offset;
} JpakDupRec;

static int jpak_dup_cmp(const void *pa, const void *pb) {
    const JpakDupRec *a = (const JpakDupRec *)pa;
    const JpakDupRec *b = (const JpakDupRec *)pb;
    if (a->crc != b->crc) return a->crc < b->crc ? -1 : 1;
    if (a->orig != b->orig) return a->orig < b->orig ? -1 : 1;
    return 0;
}

/* Open `path`, print a human-readable bill-of-materials to stdout (unless
 * quiet) and, when json_out is set, emit a machine-readable manifest
 * (schema jce.pakbom.v1) including every entry's hashes/sizes/compression.
 * When `verify` is set, every unencrypted entry is decompressed and checked
 * against its content_crc (a full integrity audit).  Duplicate content is
 * always detected (entries sharing crc + original_size).
 * Returns 0 on success, 1 on failure. */
static int cmd_inspect(const char *path, const char *json_out, int quiet, int verify) {
    /* Read only the 64-byte header (+ the small dictionary table) up front so
     * we never pull a potentially multi-gigabyte data region into RAM just to
     * list its contents.  Entries and debug paths are then served from an
     * mmap'd view (jce_archive_open_file), which pages in only the index. */
    FILE *hf = fopen(path, "rb");
    if (!hf) {
        fprintf(stderr, "[jce_pak] cannot open archive: %s\n", path);
        return 1;
    }
    uint8_t hdr[JARC_HEADER_SIZE];
    if (fread(hdr, 1, JARC_HEADER_SIZE, hf) != JARC_HEADER_SIZE) {
        fprintf(stderr, "[jce_pak] truncated archive (no header): %s\n", path);
        fclose(hf);
        return 1;
    }
    if (hdr[0] != JARC_MAGIC_0 || hdr[1] != JARC_MAGIC_1 ||
        hdr[2] != JARC_MAGIC_2 || hdr[3] != JARC_MAGIC_3) {
        fprintf(stderr, "[jce_pak] not a JPAK archive (bad magic): %s\n", path);
        fclose(hf);
        return 1;
    }
    uint32_t version = jarc_rd32(hdr + JARC_OFF_FORMAT_VERSION);
    if (version != JARC_FORMAT_VERSION) {
        fprintf(stderr, "[jce_pak] unsupported JPAK version %u (expected %u)\n",
                (unsigned)version, JARC_FORMAT_VERSION);
        fclose(hf);
        return 1;
    }

    uint32_t flags        = jarc_rd32(hdr + JARC_OFF_FLAGS);
    uint32_t entry_count  = jarc_rd32(hdr + JARC_OFF_ENTRY_COUNT);
    uint64_t index_offset = jarc_rd64(hdr + JARC_OFF_INDEX_OFFSET);
    uint64_t index_stored = jarc_rd64(hdr + JARC_OFF_INDEX_STORED_SIZE);
    uint64_t index_orig   = jarc_rd64(hdr + JARC_OFF_INDEX_ORIGINAL_SIZE);
    uint64_t data_hash    = jarc_rd64(hdr + JARC_OFF_DATA_CONTENT_HASH);
    uint64_t index_hash   = jarc_rd64(hdr + JARC_OFF_INDEX_CONTENT_HASH);
    uint16_t dict_count   = jarc_rd16(hdr + JARC_OFF_DICT_COUNT);
    uint8_t  def_comp     = hdr[JARC_OFF_DEFAULT_COMPRESSION];
    uint8_t  align_log2   = hdr[JARC_OFF_ALIGNMENT_LOG2];

    /* Dictionary table sits immediately after the header (spec §3). */
    uint8_t *dtbl    = NULL;
    size_t   dtbl_sz = (size_t)dict_count * JARC_DICT_ENTRY_SIZE;
    if (dtbl_sz) {
        dtbl = (uint8_t *)malloc(dtbl_sz);
        if (!dtbl || fread(dtbl, 1, dtbl_sz, hf) != dtbl_sz) {
            fprintf(stderr, "[jce_pak] truncated dictionary table: %s\n", path);
            free(dtbl);
            fclose(hf);
            return 1;
        }
    }

    uint64_t size = 0;
    if (fseek(hf, 0, SEEK_END) == 0) {
        long end = ftell(hf);
        if (end > 0) size = (uint64_t)end;
    }
    fclose(hf);

    JceArchive *ar = jce_archive_open_file(path);
    if (!ar) {
        fprintf(stderr, "[jce_pak] archive failed to open (corrupt index?): %s\n", path);
        free(dtbl);
        return 1;
    }
    int      header_ok = jce_archive_verify_header(ar);
    uint32_t n         = jce_archive_count(ar);

    /* ── Duplicate detection (always) + optional deep verify ──────────── */
    uint8_t *dup_flag = NULL;  /* 1 if entry's content is duplicated         */
    int8_t  *vstate   = NULL;  /* verify: 1 ok, 0 corrupt, -1 skipped/none   */
    uint32_t dup_groups = 0, dup_entries = 0;
    uint64_t dup_wasted = 0;     /* redundant bytes stored separately on disk */
    uint64_t dup_reclaimed = 0;  /* redundant bytes already coalesced (shared) */
    uint32_t verified_cnt = 0, corrupt_cnt = 0, skipped_cnt = 0;

    if (n) {
        dup_flag = (uint8_t *)calloc(n, 1);
        JpakDupRec *dr = (JpakDupRec *)malloc((size_t)n * sizeof(*dr));
        if (dup_flag && dr) {
            uint32_t m = 0;
            for (uint32_t i = 0; i < n; i++) {
                const JceArchiveEntry *e = jce_archive_get(ar, i);
                if (!e)
                    continue;
                dr[m].crc    = e->content_crc;
                dr[m].orig   = e->original_size;
                dr[m].stored = e->stored_size;
                dr[m].idx    = i;
                dr[m].offset = e->data_offset;
                m++;
            }
            qsort(dr, m, sizeof(*dr), jpak_dup_cmp);
            for (uint32_t i = 0; i < m;) {
                uint32_t j = i + 1;
                while (j < m && dr[j].crc == dr[i].crc && dr[j].orig == dr[i].orig)
                    j++;
                if (j - i > 1) {
                    dup_groups++;
                    for (uint32_t k = i; k < j; k++) {
                        dup_flag[dr[k].idx] = 1;
                        dup_entries++;
                        if (k > i) {
                            /* A redundant copy is "reclaimed" when the writer
                             * coalesced it (it shares an earlier copy's data
                             * offset); otherwise its bytes are truly wasted. */
                            int shared = 0;
                            for (uint32_t p = i; p < k; p++)
                                if (dr[p].offset == dr[k].offset) { shared = 1; break; }
                            if (shared) dup_reclaimed += dr[k].stored;
                            else        dup_wasted    += dr[k].stored;
                        }
                    }
                }
                i = j;
            }
        }
        free(dr);
    }

    if (verify && n) {
        vstate = (int8_t *)malloc(n);
        if (vstate) {
            memset(vstate, -1, n);
            void  *buf = NULL;
            size_t cap = 0;
            for (uint32_t i = 0; i < n; i++) {
                const JceArchiveEntry *e = jce_archive_get(ar, i);
                if (!e)
                    continue;
                if (e->entry_flags & JARC_ENTRY_ENCRYPTED) {
                    skipped_cnt++; /* no key available during inspection */
                    continue;
                }
                if (e->original_size > cap) {
                    void *nb = realloc(buf, e->original_size);
                    if (!nb) {
                        skipped_cnt++;
                        continue;
                    }
                    buf = nb;
                    cap = e->original_size;
                }
                size_t got = jce_archive_read(ar, e, buf, cap);
                int    ok  = (got == e->original_size) &&
                          jce_archive_verify_entry(e, buf, got);
                vstate[i] = (int8_t)(ok ? 1 : 0);
                if (ok)
                    verified_cnt++;
                else
                    corrupt_cnt++;
            }
            free(buf);
        }
    }

    if (!quiet) {
        printf("\nJPAK archive: %s\n", path);
        printf("  file size           : %llu bytes\n", (unsigned long long)size);
        printf("  format_version      : %u\n", (unsigned)version);
        printf("  entries             : %u\n", (unsigned)n);
        printf("  dictionaries        : %u\n", (unsigned)dict_count);
        printf("  alignment           : %u bytes (log2=%u)\n",
               1u << align_log2, (unsigned)align_log2);
        printf("  default_compression : %s\n", jarc_comp_name(def_comp));
        printf("  flags               : 0x%08x%s%s%s%s\n", (unsigned)flags,
               (flags & JARC_FLAG_INDEX_COMPRESSED) ? " INDEX_COMPRESSED" : "",
               (flags & JARC_FLAG_HAS_DEBUG_PATHS)  ? " HAS_DEBUG_PATHS"  : "",
               (flags & JARC_FLAG_ENCRYPTED)        ? " ENCRYPTED"        : "",
               (flags & JARC_FLAG_MMAP_FRIENDLY)    ? " MMAP_FRIENDLY"    : "");
        printf("  data_content_hash   : 0x%016llx\n", (unsigned long long)data_hash);
        printf("  index_content_hash  : 0x%016llx\n", (unsigned long long)index_hash);
        printf("  index region        : offset %llu, stored %llu, original %llu\n",
               (unsigned long long)index_offset, (unsigned long long)index_stored,
               (unsigned long long)index_orig);
        printf("  header integrity    : %s\n", header_ok ? "OK" : "MISMATCH");
        if (dict_count) {
            printf("\n  dictionaries:\n");
            for (uint16_t d = 0; d < dict_count; d++) {
                const uint8_t *de  = dtbl + (size_t)d * JARC_DICT_ENTRY_SIZE;
                uint32_t       dsz = jarc_rd32(de + JARC_DOFF_SIZE);
                char           t[5];
                fourcc_str(jarc_rd32(de + JARC_DOFF_TAG), t);
                printf("    [%u] tag=%-4s size=%u bytes\n", (unsigned)d, t, (unsigned)dsz);
            }
        }
        printf("\n  %-5s %-44s %10s %10s %6s %-9s %4s %-8s %s\n",
               "idx", "path / hash", "orig", "stored", "ratio",
               "comp", "dict", "crc32", "fl");
    }

    uint64_t tot_orig = 0, tot_stored = 0;
    uint32_t stored_cnt = 0, comp_cnt = 0, enc_cnt = 0;

    for (uint32_t i = 0; i < n; i++) {
        const JceArchiveEntry *e = jce_archive_get(ar, i);
        if (!e)
            continue;
        tot_orig   += e->original_size;
        tot_stored += e->stored_size;
        if (e->compression == JARC_COMP_NONE) stored_cnt++; else comp_cnt++;
        if (e->entry_flags & JARC_ENTRY_ENCRYPTED) enc_cnt++;

        if (!quiet) {
            const char *p = jce_archive_debug_path(ar, i);
            char        hashbuf[24];
            const char *label = p;
            if (!label) {
                snprintf(hashbuf, sizeof(hashbuf), "#%016llx",
                         (unsigned long long)e->path_hash);
                label = hashbuf;
            }
            char dictbuf[8];
            if (e->dict_id == JARC_DICT_ID_NONE) strcpy(dictbuf, "-");
            else snprintf(dictbuf, sizeof(dictbuf), "%u", (unsigned)e->dict_id);
            char fl[8];
            size_t fi = 0;
            if (e->entry_flags & JARC_ENTRY_PAGE_ALIGNED) fl[fi++] = 'P';
            if (e->entry_flags & JARC_ENTRY_ENCRYPTED)    fl[fi++] = 'E';
            if (dup_flag && dup_flag[i])                  fl[fi++] = 'D';
            if (vstate) fl[fi++] = (vstate[i] == 1 ? 'v' : (vstate[i] == 0 ? 'X' : 's'));
            if (fi == 0) fl[fi++] = '-';
            fl[fi] = 0;
            double ratio = e->original_size
                               ? (double)e->stored_size / (double)e->original_size : 0.0;
            printf("  %-5u %-44.44s %10u %10u %5.1f%% %-9s %4s %08x %s\n",
                   (unsigned)i, label, (unsigned)e->original_size,
                   (unsigned)e->stored_size, ratio * 100.0,
                   jarc_comp_name(e->compression), dictbuf,
                   (unsigned)e->content_crc, fl);
        }
    }

    if (!quiet) {
        double tratio = tot_orig ? (double)tot_stored / (double)tot_orig : 0.0;
        printf("\n  totals: %u entries, original %llu, stored %llu (%.1f%%), "
               "%u stored / %u compressed / %u encrypted\n",
               (unsigned)n, (unsigned long long)tot_orig,
               (unsigned long long)tot_stored, tratio * 100.0,
               (unsigned)stored_cnt, (unsigned)comp_cnt, (unsigned)enc_cnt);
        if (dup_groups)
            printf("  duplicates: %u groups, %u entries, %llu bytes reclaimed (shared), "
                   "%llu bytes wasted (separate)\n",
                   (unsigned)dup_groups, (unsigned)dup_entries,
                   (unsigned long long)dup_reclaimed,
                   (unsigned long long)dup_wasted);
        if (verify)
            printf("  verify: %u ok / %u CORRUPT / %u skipped%s\n",
                   (unsigned)verified_cnt, (unsigned)corrupt_cnt,
                   (unsigned)skipped_cnt,
                   corrupt_cnt ? "   <-- INTEGRITY FAILURE" : "");
    }

    if (json_out && json_out[0]) {
        ByteBuf b;
        bb_init(&b);
        bb_printf(&b, "{\n  \"schema\": \"jce.pakbom.v1\",\n");
        bb_printf(&b, "  \"file\": ");
        json_str(&b, path);
        bb_printf(&b, ",\n  \"file_size\": %llu,\n", (unsigned long long)size);
        bb_printf(&b, "  \"header\": {\n");
        bb_printf(&b, "    \"magic\": \"JPAK\",\n");
        bb_printf(&b, "    \"format_version\": %u,\n", (unsigned)version);
        bb_printf(&b, "    \"flags\": %u,\n", (unsigned)flags);
        bb_printf(&b, "    \"index_compressed\": %s,\n",
                  (flags & JARC_FLAG_INDEX_COMPRESSED) ? "true" : "false");
        bb_printf(&b, "    \"has_debug_paths\": %s,\n",
                  (flags & JARC_FLAG_HAS_DEBUG_PATHS) ? "true" : "false");
        bb_printf(&b, "    \"encrypted\": %s,\n",
                  (flags & JARC_FLAG_ENCRYPTED) ? "true" : "false");
        bb_printf(&b, "    \"mmap_friendly\": %s,\n",
                  (flags & JARC_FLAG_MMAP_FRIENDLY) ? "true" : "false");
        bb_printf(&b, "    \"entry_count\": %u,\n", (unsigned)entry_count);
        bb_printf(&b, "    \"dict_count\": %u,\n", (unsigned)dict_count);
        bb_printf(&b, "    \"alignment_log2\": %u,\n", (unsigned)align_log2);
        bb_printf(&b, "    \"alignment\": %u,\n", 1u << align_log2);
        bb_printf(&b, "    \"default_compression\": %u,\n", (unsigned)def_comp);
        bb_printf(&b, "    \"default_compression_name\": \"%s\",\n",
                  jarc_comp_name(def_comp));
        bb_printf(&b, "    \"index_offset\": %llu,\n", (unsigned long long)index_offset);
        bb_printf(&b, "    \"index_stored_size\": %llu,\n",
                  (unsigned long long)index_stored);
        bb_printf(&b, "    \"index_original_size\": %llu,\n",
                  (unsigned long long)index_orig);
        bb_printf(&b, "    \"data_content_hash\": \"0x%016llx\",\n",
                  (unsigned long long)data_hash);
        bb_printf(&b, "    \"index_content_hash\": \"0x%016llx\",\n",
                  (unsigned long long)index_hash);
        bb_printf(&b, "    \"header_verified\": %s\n", header_ok ? "true" : "false");
        bb_printf(&b, "  },\n");

        bb_printf(&b, "  \"dictionaries\": [");
        {
            for (uint16_t d = 0; d < dict_count; d++) {
                const uint8_t *de   = dtbl + (size_t)d * JARC_DICT_ENTRY_SIZE;
                uint64_t       doff = jarc_rd64(de + JARC_DOFF_DATA_OFFSET);
                uint32_t       dsz  = jarc_rd32(de + JARC_DOFF_SIZE);
                char           t[5];
                fourcc_str(jarc_rd32(de + JARC_DOFF_TAG), t);
                bb_printf(&b, "%s\n    {\"id\": %u, \"tag\": \"%s\", \"offset\": %llu, "
                              "\"size\": %u}",
                          d ? "," : "", (unsigned)d, t,
                          (unsigned long long)doff, (unsigned)dsz);
            }
        }
        bb_printf(&b, "%s],\n", dict_count ? "\n  " : "");

        double tratio = tot_orig ? (double)tot_stored / (double)tot_orig : 0.0;
        bb_printf(&b, "  \"totals\": {\"entries\": %u, \"original_size\": %llu, "
                      "\"stored_size\": %llu, \"ratio\": %.6f, \"stored_count\": %u, "
                      "\"compressed_count\": %u, \"encrypted_count\": %u, "
                      "\"duplicate_groups\": %u, \"duplicate_entries\": %u, "
                      "\"duplicate_wasted_bytes\": %llu, "
                      "\"duplicate_reclaimed_bytes\": %llu, \"verify_ran\": %s, "
                      "\"verified_count\": %u, \"corrupt_count\": %u, "
                      "\"skipped_count\": %u},\n",
                  (unsigned)n, (unsigned long long)tot_orig,
                  (unsigned long long)tot_stored, tratio,
                  (unsigned)stored_cnt, (unsigned)comp_cnt, (unsigned)enc_cnt,
                  (unsigned)dup_groups, (unsigned)dup_entries,
                  (unsigned long long)dup_wasted,
                  (unsigned long long)dup_reclaimed, verify ? "true" : "false",
                  (unsigned)verified_cnt, (unsigned)corrupt_cnt,
                  (unsigned)skipped_cnt);

        bb_printf(&b, "  \"entries\": [");
        for (uint32_t i = 0; i < n; i++) {
            const JceArchiveEntry *e = jce_archive_get(ar, i);
            if (!e)
                continue;
            const char *p     = jce_archive_debug_path(ar, i);
            double      ratio = e->original_size
                                    ? (double)e->stored_size / (double)e->original_size : 0.0;
            bb_printf(&b, "%s\n    {\"index\": %u, \"path\": ", i ? "," : "", (unsigned)i);
            if (p) json_str(&b, p);
            else   bb_append(&b, "null", 4);
            bb_printf(&b, ", \"path_hash\": \"0x%016llx\"",
                      (unsigned long long)e->path_hash);
            bb_printf(&b, ", \"data_offset\": %llu",
                      (unsigned long long)e->data_offset);
            bb_printf(&b, ", \"original_size\": %u", (unsigned)e->original_size);
            bb_printf(&b, ", \"stored_size\": %u", (unsigned)e->stored_size);
            bb_printf(&b, ", \"ratio\": %.6f", ratio);
            bb_printf(&b, ", \"compression\": %u", (unsigned)e->compression);
            bb_printf(&b, ", \"compression_name\": \"%s\"",
                      jarc_comp_name(e->compression));
            if (e->dict_id == JARC_DICT_ID_NONE) bb_printf(&b, ", \"dict_id\": null");
            else bb_printf(&b, ", \"dict_id\": %u", (unsigned)e->dict_id);
            bb_printf(&b, ", \"content_crc\": \"0x%08x\"", (unsigned)e->content_crc);
            bb_printf(&b, ", \"flags\": %u", (unsigned)e->entry_flags);
            bb_printf(&b, ", \"page_aligned\": %s",
                      (e->entry_flags & JARC_ENTRY_PAGE_ALIGNED) ? "true" : "false");
            bb_printf(&b, ", \"encrypted\": %s",
                      (e->entry_flags & JARC_ENTRY_ENCRYPTED) ? "true" : "false");
            bb_printf(&b, ", \"duplicate\": %s",
                      (dup_flag && dup_flag[i]) ? "true" : "false");
            if (vstate && vstate[i] >= 0)
                bb_printf(&b, ", \"verified\": %s}", vstate[i] == 1 ? "true" : "false");
            else
                bb_printf(&b, ", \"verified\": null}");
        }
        bb_printf(&b, "%s]\n}\n", n ? "\n  " : "");

        write_file_atomic(json_out, b.data, b.size);
        bb_free(&b);
        if (!quiet)
            printf("\n[jce_pak] BOM JSON written: %s\n", json_out);
    }

    jce_archive_close(ar);
    free(dtbl);
    free(dup_flag);
    free(vstate);
    return 0;
}

/* ================================================================== */
/* CLI argument parsing                                                */
/* ================================================================== */

typedef struct {
    char resource_dirs[16][1024];
    int  resource_dir_count;
    char exclude_segments[16][128];
    int exclude_segment_count;
    char exclude_suffixes[16][32];
    int exclude_suffix_count;
    char pak_file[1024];
    char obj_file[1024];
    char header_file[1024];
    char manifest_file[1024];
    char c_file[1024];
    char obj_format[32];
    char obj_arch[32];
    char platform[32]; /* desktop, mobile, web, console (default: desktop) */
    char sym_prefix[64]; /* C/COFF symbol base; default "assets_pak_data".
                            Header emits <prefix> and <prefix>_size; the COFF
                            generator uses the same names. */
    int zstd_level;    /* 1..22; default 3                                */
    int no_store_opt;  /* 1 to disable STORED auto-detection             */
    char inspect_file[1024]; /* if set: inspect this archive, skip packing */
    char json_file[1024];    /* optional BOM JSON output path             */
    int  quiet;              /* suppress the console bill-of-materials     */
    int  verify;             /* inspect: deep CRC-verify every entry       */
} Args;

static void usage(void) {
    fprintf(stderr,
            "Usage: jce_pak --resource-dir <dir> [--resource-dir <dir2> ...]\n"
            "              [--exclude-segment <name> ...]\n"
            "              [--exclude-suffix  <suffix> ...]\n"
            "               --pak-file      <out.pak>\n"
            "               --header-file   <out.h>\n"
            "               --manifest-file <out.cmake>\n"
            "              [--obj-file      <out.obj>]\n"
            "              [--c-file        <out.c>]     (for c-array format)\n"
            "              [--obj-format    coff|c-array|none]   (default: none)\n"
            "              [--obj-arch      x64|arm64|x86|arm]  (default: x64)\n"
            "              [--platform      desktop|mobile|web|console] (default: desktop)\n"
            "              [--symbol-prefix <ident>]    C/COFF symbol base (default: assets_pak_data)\n"
            "              [--level         <1..22>]    ZSTD level (default: 3)\n"
            "              [--no-store-opt]             disable already-compressed bypass\n"
            "\n"
            "  Inspect mode (read an existing archive, emit a bill-of-materials):\n"
            "    jce_pak --inspect <archive.pak> [--json <out.json>] [--quiet] [--verify]\n"
            "  --verify decompresses every entry and checks its content CRC (deep audit).\n"
            "  In pack mode, --json <out.json> also writes a BOM of the produced .pak.\n"
            "\n"
            "  --exclude-suffix skips any file whose path ends with the suffix\n"
            "  (case-sensitive).  Used to drop platform-irrelevant shader binaries\n"
            "  (e.g. _mtl.bin on Windows, _dx11.bin on Linux).\n"
            "\n"
            "  --platform selects the target platform (reserved for future use):\n"
            "    desktop  → Windows / macOS / Linux\n"
            "    mobile   → Android / iOS\n"
            "    web      → Emscripten\n"
            "    console  → Console platforms\n");
}

static int has_suffix(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lf = strlen(suffix);
    if (lf > ls)
        return 0;
    return memcmp(s + ls - lf, suffix, lf) == 0;
}

static int is_excluded_rel_path(const char *rel, const Args *args)
{
    for (int i = 0; i < args->exclude_segment_count; i++) {
        if (args->exclude_segments[i][0] == '\0')
            continue;
        if (path_has_segment(rel, args->exclude_segments[i]))
            return 1;
    }
    for (int i = 0; i < args->exclude_suffix_count; i++) {
        if (args->exclude_suffixes[i][0] == '\0')
            continue;
        if (has_suffix(rel, args->exclude_suffixes[i]))
            return 1;
    }
    return 0;
}

static Args parse_args(int argc, char *const argv[]) {
    Args a;
    memset(&a, 0, sizeof(a));
    strcpy(a.obj_format, "none");
    strcpy(a.obj_arch, "x64");
    strcpy(a.platform, "desktop");
    strcpy(a.sym_prefix, "assets_pak_data");
    a.zstd_level = 3;
    a.no_store_opt = 0;

    /* Safety default: raw_assets is source-only and must never be packed. */
    snprintf(a.exclude_segments[a.exclude_segment_count],
             sizeof(a.exclude_segments[0]), "%s", "raw_assets");
    a.exclude_segment_count++;

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        const char *val = NULL;
        if (i + 1 < argc) val = argv[i + 1];

        if (strcmp(arg, "--resource-dir") == 0 && val) {
            if (a.resource_dir_count < 16) {
                snprintf(a.resource_dirs[a.resource_dir_count], sizeof(a.resource_dirs[0]), "%s", val);
                a.resource_dir_count++;
            }
            ++i;
        }
        else if (strcmp(arg, "--exclude-segment") == 0 && val) {
            if (a.exclude_segment_count < 16) {
                snprintf(a.exclude_segments[a.exclude_segment_count],
                         sizeof(a.exclude_segments[0]), "%s", val);
                a.exclude_segment_count++;
            }
            ++i;
        } else if (strcmp(arg, "--exclude-suffix") == 0 && val) {
            if (a.exclude_suffix_count < 16) {
                snprintf(a.exclude_suffixes[a.exclude_suffix_count], sizeof(a.exclude_suffixes[0]),
                         "%s", val);
                a.exclude_suffix_count++;
            }
            ++i;
        } else if (strcmp(arg, "--pak-file") == 0 && val) {
            snprintf(a.pak_file, sizeof(a.pak_file), "%s", val);
            ++i;
        } else if (strcmp(arg, "--obj-file") == 0 && val) {
            snprintf(a.obj_file, sizeof(a.obj_file), "%s", val);
            ++i;
        } else if (strcmp(arg, "--header-file") == 0 && val) {
            snprintf(a.header_file, sizeof(a.header_file), "%s", val);
            ++i;
        } else if (strcmp(arg, "--manifest-file") == 0 && val) {
            snprintf(a.manifest_file, sizeof(a.manifest_file), "%s", val);
            ++i;
        } else if (strcmp(arg, "--c-file") == 0 && val) {
            snprintf(a.c_file, sizeof(a.c_file), "%s", val);
            ++i;
        } else if (strcmp(arg, "--obj-format") == 0 && val) {
            snprintf(a.obj_format, sizeof(a.obj_format), "%s", val);
            ++i;
        } else if (strcmp(arg, "--obj-arch") == 0 && val) {
            snprintf(a.obj_arch, sizeof(a.obj_arch), "%s", val);
            ++i;
        } else if (strcmp(arg, "--platform") == 0 && val) {
            snprintf(a.platform, sizeof(a.platform), "%s", val);
            ++i;
        } else if (strcmp(arg, "--symbol-prefix") == 0 && val) {
            snprintf(a.sym_prefix, sizeof(a.sym_prefix), "%s", val);
            ++i;
        } else if (strcmp(arg, "--level") == 0 && val) {
            a.zstd_level = atoi(val);
            ++i;
        } else if (strcmp(arg, "--no-store-opt") == 0) {
            a.no_store_opt = 1;
        } else if (strcmp(arg, "--inspect") == 0 && val) {
            snprintf(a.inspect_file, sizeof(a.inspect_file), "%s", val);
            ++i;
        } else if (strcmp(arg, "--json") == 0 && val) {
            snprintf(a.json_file, sizeof(a.json_file), "%s", val);
            ++i;
        } else if (strcmp(arg, "--quiet") == 0) {
            a.quiet = 1;
        } else if (strcmp(arg, "--verify") == 0) {
            a.verify = 1;
        } else {
            fprintf(stderr, "[jce_pak] unknown argument: %s\n", arg);
            usage();
            exit(1);
        }
    }

    /* Inspect mode needs only --inspect; packing inputs are not required. */
    if (!a.inspect_file[0] &&
        (a.resource_dir_count == 0 || !a.pak_file[0] ||
         !a.header_file[0]  || !a.manifest_file[0])) {
        usage();
        exit(1);
    }

    /* Validate --platform value. */
    if (strcmp(a.platform, "desktop") != 0 &&
        strcmp(a.platform, "mobile")  != 0 &&
        strcmp(a.platform, "web")     != 0 &&
        strcmp(a.platform, "console") != 0) {
        fprintf(stderr, "[jce_pak] unknown --platform: %s\n", a.platform);
        fprintf(stderr, "  Valid values: desktop, mobile, web, console\n");
        exit(1);
    }

    /* Validate --level. */
    if (a.zstd_level < 1 || a.zstd_level > 22) {
        fprintf(stderr, "[jce_pak] --level must be in [1, 22] (got %d)\n", a.zstd_level);
        exit(1);
    }

    return a;
}

/* ================================================================== */
/* main                                                                */
/* ================================================================== */

int main(int argc, char *argv[]) {
    Args args = parse_args(argc, argv);

    /* Inspect mode: read an existing archive and emit a bill-of-materials. */
    if (args.inspect_file[0])
        return cmd_inspect(args.inspect_file, args.json_file, args.quiet, args.verify);

    printf("[jce_pak] target platform: %s\n", args.platform);

    /* Normalise resource_dir separators and strip trailing slash. */
    for (int rd = 0; rd < args.resource_dir_count; rd++) {
        normalise_sep(args.resource_dirs[rd]);
        size_t rlen = strlen(args.resource_dirs[rd]);
        while (rlen > 0 && args.resource_dirs[rd][rlen - 1] == '/')
            args.resource_dirs[rd][--rlen] = '\0';
    }

    /* -- 1. Enumerate files from all resource dirs ------------- */
    StrList files;
    sl_init(&files);
    /* Track which resource_dir each file came from for relative path computation. */
    StrList file_bases;
    sl_init(&file_bases);
    for (int rd = 0; rd < args.resource_dir_count; rd++) {
        StrList dir_files;
        sl_init(&dir_files);
        enumerate_files(args.resource_dirs[rd], &dir_files);
        for (size_t fi = 0; fi < dir_files.count; fi++) {
            sl_push(&files, dir_files.items[fi]);
            sl_push(&file_bases, args.resource_dirs[rd]);
        }
        sl_free(&dir_files);
    }
    /* Note: files are not sorted here since file_bases must stay in sync.
       The TOC is sorted by path_hash later. */

    /* -- 2. Read & collect ------------------------------------- */
    /* Slurp every file into memory and hand the set to jce_archive_cook(),
     * which classifies each resource (spec §6.2), trains shared dictionaries
     * (spec §7), applies the keep-if-helps guard (spec §6.3) and emits a
     * JPAK v1 archive.  The original bytes are kept alive in e->compressed
     * until the cook copies them. */
    size_t num_entries = 0;
    size_t entries_cap = files.count ? files.count : 1;
    AssetEntry   *entries = (AssetEntry *)calloc(entries_cap, sizeof(AssetEntry));
    JceCookInput *inputs  = (JceCookInput *)calloc(entries_cap, sizeof(JceCookInput));
    if (!entries || !inputs) { fprintf(stderr, "[jce_pak] out of memory\n"); return 1; }

    size_t skipped_hidden = 0;
    size_t skipped_excluded = 0;

    for (size_t fi = 0; fi < files.count; fi++) {
        char *rel = make_relative(files.items[fi], file_bases.items[fi]);
        if (is_hidden(rel)) { free(rel); skipped_hidden++; continue; }
        if (is_excluded_rel_path(rel, &args)) { free(rel); skipped_excluded++; continue; }

        size_t raw_size = 0;
        uint8_t *raw = read_file_bin(files.items[fi], &raw_size);

        AssetEntry *e = &entries[num_entries];
        e->rel_path        = rel;
        e->original_size   = raw_size;
        e->compressed      = raw;       /* original bytes, freed after cook */
        e->compressed_size = raw_size;  /* updated from the cooked archive  */
        e->flags           = 0;

        inputs[num_entries].vpath = rel;
        inputs[num_entries].data  = raw;
        inputs[num_entries].size  = raw_size;
        num_entries++;
    }

    sl_free(&files);
    sl_free(&file_bases);

    /* -- 3. Cook into a JPAK v1 archive ------------------------ */
    JceCookConfig cfg = {0};
    cfg.zstd_level       = args.zstd_level;
    cfg.alignment_log2   = 4;
    cfg.mmap_friendly    = false;
    cfg.emit_debug_paths = true;  /* populate JcePakAsset.path at runtime */
    cfg.compress_index   = true;
    cfg.use_dict         = true;  /* train JSON/TEXT/SHADER dictionaries  */
    cfg.dedup_content    = true;  /* coalesce byte-identical payloads      */

    void    *pak_blob   = NULL;
    size_t   pak_size   = 0;
    uint16_t dict_count = 0;
    if (!jce_archive_cook(inputs, num_entries, &cfg, &pak_blob, &pak_size, &dict_count)) {
        fprintf(stderr, "[jce_pak] archive cook failed\n");
        return 1;
    }
    free(inputs);

    const uint64_t pak_total = (uint64_t)pak_size;

    /* Reopen the cooked archive to recover each entry's on-disk size and
     * compression decision for the build manifest / report. */
    {
        JceArchive *ar = jce_archive_open(pak_blob, pak_size);
        if (ar) {
            for (size_t i = 0; i < num_entries; i++) {
                const JceArchiveEntry *ae = jce_archive_find(ar, entries[i].rel_path);
                if (ae) {
                    entries[i].compressed_size = ae->stored_size;
                    entries[i].flags = (ae->compression == JCE_ARCHIVE_COMP_NONE)
                                           ? JPAK_FLAG_STORED : 0;
                }
            }
            jce_archive_close(ar);
        }
    }

    if (dict_count)
        printf("[jce_pak] trained %u shared dictionar%s\n",
               (unsigned)dict_count, dict_count == 1 ? "y" : "ies");

    /* -- 4. Write outputs -------------------------------------- */

    write_file_atomic(args.pak_file, pak_blob, pak_size);

    {
        ByteBuf hdr;
        bb_init(&hdr);
        generate_header(args.sym_prefix, &hdr);
        write_file_atomic(args.header_file, hdr.data, hdr.size);
        bb_free(&hdr);
    }

    {
        ByteBuf mf;
        bb_init(&mf);
        generate_manifest(entries, num_entries, pak_total, &mf);
        write_file_atomic(args.manifest_file, mf.data, mf.size);
        bb_free(&mf);
    }

    /* Optional bill-of-materials JSON for the just-produced archive. */
    if (args.json_file[0])
        cmd_inspect(args.pak_file, args.json_file, 1, 0);

    /* COFF .obj */
    if (strcmp(args.obj_format, "coff") == 0 && args.obj_file[0]) {
        uint16_t machine = coff_machine_from_arch(args.obj_arch);
        ByteBuf obj;
        bb_init(&obj);
        generate_coff_obj(pak_blob, pak_size, machine, args.sym_prefix, &obj);
        write_file_atomic(args.obj_file, obj.data, obj.size);
        printf("[jce_pak] COFF .obj written: %s (%zu bytes)\n",
               args.obj_file, obj.size);
        bb_free(&obj);
    }

    /* C-array (Emscripten / platforms without .incbin). */
    if (strcmp(args.obj_format, "c-array") == 0 && args.c_file[0]) {
        ByteBuf carr;
        bb_init(&carr);
        generate_c_array(pak_blob, pak_size, args.sym_prefix, &carr);
        write_file_atomic(args.c_file, carr.data, carr.size);
        printf("[jce_pak] C-array written: %s (%zu bytes)\n",
               args.c_file, carr.size);
        bb_free(&carr);
    }

    /* -- 5. Summary -------------------------------------------- */

    uint64_t raw_total = 0, comp_total = 0;
    for (size_t i = 0; i < num_entries; i++) {
        raw_total  += entries[i].original_size;
        comp_total += entries[i].compressed_size;
    }

    printf("[jce_pak] %zu files packed | raw %llu B -> compressed %llu B -> pak %llu B\n",
           num_entries,
           (unsigned long long)raw_total,
           (unsigned long long)comp_total,
           (unsigned long long)pak_total);

    if (skipped_hidden > 0 || skipped_excluded > 0) {
        printf("[jce_pak] skipped: hidden=%zu excluded=%zu\n",
               skipped_hidden, skipped_excluded);
    }

    /* Cleanup. */
    jce_free(pak_blob);
    for (size_t i = 0; i < num_entries; i++) {
        free(entries[i].rel_path);
        free(entries[i].compressed);
    }
    free(entries);

    return 0;
}
