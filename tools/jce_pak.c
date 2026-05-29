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

#include <xxhash.h>
#include <zstd.h>

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

static void buf_le64(ByteBuf *b, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        bb_push(b, (uint8_t)(v >> (i * 8)));
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

/* Files whose contents are already entropy-coded; running zstd on them
 * yields ~1.00× ratio while still costing CPU at decode time.  We mark
 * those entries with JPAK_FLAG_STORED and copy raw bytes verbatim. */
static int jce_pak_is_already_compressed(const char *rel)
{
    static const char *exts[] = {
        ".png", ".jpg",      ".jpeg",     ".webp", ".ktx",  ".ktx2", ".basis", ".dds",  ".ogg",
        ".mp3", ".opus",     ".flac",     ".aac",  ".wav",  ".m4a",  ".mp4",   ".webm", ".mkv",
        ".mov", ".avi",      ".ivf",      ".zip",  ".7z",   ".gz",   ".zst",   ".xz",   ".bz2",
        ".pak", ".jceasset", ".jpeg2000", ".jp2",  ".heic", ".heif", ".avif",  NULL};
    size_t n = strlen(rel);
    for (size_t i = 0; exts[i]; ++i) {
        size_t e = strlen(exts[i]);
        if (n >= e) {
            int eq = 1;
            for (size_t k = 0; k < e; ++k) {
                char c = rel[n - e + k];
                if (c >= 'A' && c <= 'Z')
                    c = (char)(c - 'A' + 'a');
                if (c != exts[i][k]) {
                    eq = 0;
                    break;
                }
            }
            if (eq)
                return 1;
        }
    }
    return 0;
}

static int entry_cmp_hash(const void *a, const void *b)
{
    uint64_t ha = ((const AssetEntry *)a)->path_hash;
    uint64_t hb = ((const AssetEntry *)b)->path_hash;
    if (ha < hb) return -1;
    if (ha > hb) return  1;
    return 0;
}

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

    char sym_data_buf[96];
    char sym_size_buf[112];
    snprintf(sym_data_buf, sizeof(sym_data_buf), "%s", sym_prefix);
    snprintf(sym_size_buf, sizeof(sym_size_buf), "%s_size", sym_prefix);
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

// cppcheck-suppress constParameter   ; argv comes from main() with non-const char**
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
        } else {
            fprintf(stderr, "[jce_pak] unknown argument: %s\n", arg);
            usage();
            exit(1);
        }
    }

    if (a.resource_dir_count == 0 || !a.pak_file[0] ||
        !a.header_file[0]  || !a.manifest_file[0]) {
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

    /* -- 2. Compress & hash ------------------------------------ */
    size_t num_entries = 0;
    size_t entries_cap = files.count ? files.count : 1;
    AssetEntry *entries = (AssetEntry *)calloc(entries_cap, sizeof(AssetEntry));
    if (!entries) { fprintf(stderr, "[jce_pak] out of memory\n"); return 1; }

    /* Reusable ZSTD compression context — avoids repeated internal
       allocation/deallocation when compressing many files. */
    ZSTD_CCtx *cctx = ZSTD_createCCtx();
    if (!cctx) { fprintf(stderr, "[jce_pak] ZSTD_createCCtx failed\n"); return 1; }

    size_t skipped_hidden = 0;
    size_t skipped_excluded = 0;
    size_t stored_count = 0;
    uint64_t stored_bytes = 0;

    for (size_t fi = 0; fi < files.count; fi++) {
        char *rel = make_relative(files.items[fi], file_bases.items[fi]);
        if (is_hidden(rel)) { free(rel); skipped_hidden++; continue; }
        if (is_excluded_rel_path(rel, &args)) { free(rel); skipped_excluded++; continue; }

        AssetEntry *e = &entries[num_entries];
        e->rel_path = rel;
        e->path_hash = XXH3_64bits(rel, strlen(rel));

        size_t raw_size = 0;
        uint8_t *raw = read_file_bin(files.items[fi], &raw_size);
        e->original_size = raw_size;
        e->content_hash = XXH3_64bits(raw, raw_size);
        e->flags = 0;

        /* STORED bypass: already-compressed media or tiny files where the
         * ZSTD frame header would inflate the payload.  Loader memcpy's
         * compressed_size == original_size bytes verbatim. */
        int store_raw =
            !args.no_store_opt && (jce_pak_is_already_compressed(rel) || raw_size < 256);

        if (store_raw) {
            e->compressed = (uint8_t *)malloc(raw_size ? raw_size : 1);
            if (!e->compressed) {
                fprintf(stderr, "[jce_pak] out of memory\n");
                return 1;
            }
            if (raw_size)
                memcpy(e->compressed, raw, raw_size);
            e->compressed_size = raw_size;
            e->flags |= JPAK_FLAG_STORED;
            stored_count++;
            stored_bytes += raw_size;
            free(raw);
        } else {
            size_t bound = ZSTD_compressBound(raw_size);
            e->compressed = (uint8_t *)malloc(bound);
            if (!e->compressed) {
                fprintf(stderr, "[jce_pak] out of memory\n");
                return 1;
            }

            size_t comp_sz =
                ZSTD_compressCCtx(cctx, e->compressed, bound, raw, raw_size, args.zstd_level);
            free(raw);

            if (ZSTD_isError(comp_sz)) {
                fprintf(stderr, "[jce_pak] ZSTD error compressing %s: %s\n", rel,
                        ZSTD_getErrorName(comp_sz));
                return 1;
            }

            /* Fallback: if zstd somehow produced a larger output (very small
             * inputs or already-entropy-coded), promote to STORED to avoid
             * negative ratios. */
            if (comp_sz >= e->original_size && e->original_size > 0) {
                /* Re-read to store raw — cheaper than a second buffer. */
                uint8_t *raw2 = read_file_bin(files.items[fi], &raw_size);
                memcpy(e->compressed, raw2, raw_size);
                free(raw2);
                e->compressed_size = raw_size;
                e->flags |= JPAK_FLAG_STORED;
                stored_count++;
                stored_bytes += raw_size;
            } else {
                e->compressed_size = comp_sz;
            }
        }
        num_entries++;
    }

    ZSTD_freeCCtx(cctx);

    if (stored_count > 0) {
        printf("[jce_pak] stored (uncompressed): %zu entries, %.2f MB\n", stored_count,
               (double)stored_bytes / (1024.0 * 1024.0));
    }

    sl_free(&files);
    sl_free(&file_bases);

    /* Sort TOC by path_hash for binary search at runtime. */
    qsort(entries, num_entries, sizeof(AssetEntry), entry_cmp_hash);

    /* -- 3. Build .pak blob ------------------------------------ */

    /* 3a. Compute names section. */
    uint32_t names_cursor = 0;
    for (size_t i = 0; i < num_entries; i++) {
        entries[i].name_offset = names_cursor;
        entries[i].name_length = (uint32_t)strlen(entries[i].rel_path);
        names_cursor += entries[i].name_length;
    }
    const uint32_t names_size = names_cursor;

    /* 3b. Compute data offsets. */
    uint64_t data_cursor = 0;
    for (size_t i = 0; i < num_entries; i++) {
        entries[i].data_offset = data_cursor;
        data_cursor += entries[i].compressed_size;
    }

    /* 3c. Compute section offsets in the file. */
    const uint64_t toc_offset = JPAK_HEADER_SIZE;
    const uint64_t toc_size   = (uint64_t)num_entries * JPAK_TOC_ENTRY_SIZE;
    const uint64_t names_off  = toc_offset + toc_size;
    const uint64_t data_off   = names_off + names_size;
    const uint64_t pak_total  = data_off + data_cursor;

    /* 3d. Serialise. */
    ByteBuf pak;
    bb_init(&pak);
    bb_reserve(&pak, (size_t)pak_total);

    /* Header. */
    bb_push(&pak, JPAK_MAGIC_0);
    bb_push(&pak, JPAK_MAGIC_1);
    bb_push(&pak, JPAK_MAGIC_2);
    bb_push(&pak, JPAK_MAGIC_3);
    buf_le32(&pak, JPAK_VERSION);
    buf_le32(&pak, (uint32_t)num_entries);
    buf_le32(&pak, 0);
    buf_le64(&pak, toc_offset);
    buf_le64(&pak, data_off);

    /* TOC entries. */
    for (size_t i = 0; i < num_entries; i++) {
        const AssetEntry *e = &entries[i];
        buf_le64(&pak, e->path_hash);
        buf_le32(&pak, (uint32_t)(names_off + e->name_offset));
        buf_le32(&pak, e->name_length);
        buf_le64(&pak, e->data_offset);
        buf_le64(&pak, e->compressed_size);
        buf_le64(&pak, e->original_size);
        buf_le32(&pak, e->flags);
        buf_le32(&pak, 0); /* _pad */
        buf_le64(&pak, e->content_hash);
    }

    /* Names section. */
    for (size_t i = 0; i < num_entries; i++)
        bb_append(&pak, entries[i].rel_path, entries[i].name_length);

    /* Data section. */
    for (size_t i = 0; i < num_entries; i++)
        bb_append(&pak, entries[i].compressed, entries[i].compressed_size);

    /* -- 4. Write outputs -------------------------------------- */

    write_file_atomic(args.pak_file, pak.data, pak.size);

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

    /* COFF .obj */
    if (strcmp(args.obj_format, "coff") == 0 && args.obj_file[0]) {
        uint16_t machine = coff_machine_from_arch(args.obj_arch);
        ByteBuf obj;
        bb_init(&obj);
        generate_coff_obj(pak.data, pak.size, machine, args.sym_prefix, &obj);
        write_file_atomic(args.obj_file, obj.data, obj.size);
        printf("[jce_pak] COFF .obj written: %s (%zu bytes)\n",
               args.obj_file, obj.size);
        bb_free(&obj);
    }

    /* C-array (Emscripten / platforms without .incbin). */
    if (strcmp(args.obj_format, "c-array") == 0 && args.c_file[0]) {
        ByteBuf carr;
        bb_init(&carr);
        generate_c_array(pak.data, pak.size, args.sym_prefix, &carr);
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
    bb_free(&pak);
    for (size_t i = 0; i < num_entries; i++) {
        free(entries[i].rel_path);
        free(entries[i].compressed);
    }
    free(entries);

    return 0;
}
