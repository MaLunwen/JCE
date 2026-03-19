/* jce_pak.cpp
 *
 * Host tool that packs resource files into a JCE PAK archive.
 *
 *   1. Recursively enumerates RESOURCE_DIR
 *   2. ZSTD-compresses each file
 *   3. Indexes paths with XXH3_64bits
 *   4. Writes a binary .pak (see pak_format.h)
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
 *           --obj-format   coff|none        (default: none)
 *           --obj-arch     x64|arm64|x86|arm (default: x64)
 */

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <xxhash.h>
#include <zstd.h>

/* pak_format.h lives under main/native/resource/  we include it
 * via a path relative to the tools/ directory.  CMake adds the
 * correct -I flag so this resolves at build time. */
#include "resource/pak_format.h"

namespace fs = std::filesystem;

/* ================================================================== */
/* Helpers                                                             */
/* ================================================================== */

static void write_le16(std::vector<uint8_t> &buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>(v));
    buf.push_back(static_cast<uint8_t>(v >> 8));
}

static void write_le32(std::vector<uint8_t> &buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>(v));
    buf.push_back(static_cast<uint8_t>(v >> 8));
    buf.push_back(static_cast<uint8_t>(v >> 16));
    buf.push_back(static_cast<uint8_t>(v >> 24));
}

static void write_le64(std::vector<uint8_t> &buf, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        buf.push_back(static_cast<uint8_t>(v >> (i * 8)));
}

/* Read an entire file into a byte vector. */
static std::vector<uint8_t> read_file(const fs::path &path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        std::cerr << "[jce_pak] ERROR: cannot open " << path << "\n";
        std::exit(1);
    }
    auto sz = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> data(static_cast<size_t>(sz));
    f.read(reinterpret_cast<char *>(data.data()), sz);
    return data;
}

/* Write a byte vector to a file atomically (write tmp + rename). */
static void write_file(const fs::path &path,
                       const void *data, size_t size) {
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            std::cerr << "[jce_pak] ERROR: cannot write " << tmp << "\n";
            std::exit(1);
        }
        f.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
    }
    /* Overwrite destination only when content differs (preserves
     * mtime for incremental builds). */
    std::error_code ec;
    if (fs::exists(path, ec)) {
        auto existing = read_file(path);
        if (existing.size() == size &&
            std::memcmp(existing.data(), data, size) == 0) {
            fs::remove(tmp, ec);
            return;
        }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        /* rename may fail across drives; fall back to copy. */
        fs::copy_file(tmp, path, fs::copy_options::overwrite_existing, ec);
        fs::remove(tmp, ec);
    }
}

static void write_file(const fs::path &path, const std::string &s) {
    write_file(path, s.data(), s.size());
}

static void write_file(const fs::path &path,
                       const std::vector<uint8_t> &v) {
    write_file(path, v.data(), v.size());
}

/* Normalise a relative path to forward slashes. */
static std::string normalise(const fs::path &rel) {
    std::string s = rel.generic_string();
    /* generic_string already uses '/', but just in case: */
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

/* ================================================================== */
/* Per-asset record                                                    */
/* ================================================================== */

struct AssetEntry {
    std::string   rel_path;       /* e.g. "fonts/JCE.ttf"            */
    uint64_t      path_hash;      /* XXH3_64bits(rel_path)           */
    uint64_t      original_size;
    std::vector<uint8_t> compressed; /* ZSTD frame                   */
    /* Filled during serialisation: */
    uint32_t      name_offset;
    uint32_t      name_length;
    uint64_t      data_offset;
};

/* ================================================================== */
/* COFF .obj generator                                                 */
/* ================================================================== */

/* COFF machine-type constants. */
static constexpr uint16_t COFF_MACHINE_AMD64  = 0x8664;
static constexpr uint16_t COFF_MACHINE_I386   = 0x014C;
static constexpr uint16_t COFF_MACHINE_ARM64  = 0xAA64;
static constexpr uint16_t COFF_MACHINE_ARMNT  = 0x01C4;

/* Resolve a machine-type from the --obj-arch CLI string. */
static uint16_t coff_machine_from_arch(const std::string &arch) {
    if (arch == "x64"   || arch == "x86_64" || arch == "amd64")  return COFF_MACHINE_AMD64;
    if (arch == "x86"   || arch == "i386")                       return COFF_MACHINE_I386;
    if (arch == "arm64" || arch == "aarch64")                    return COFF_MACHINE_ARM64;
    if (arch == "arm"   || arch == "armv7")                      return COFF_MACHINE_ARMNT;
    std::cerr << "[jce_pak] unknown --obj-arch: " << arch << "\n";
    std::exit(1);
}

/* Pointer width in bytes for a given COFF machine type. */
static int coff_pointer_size(uint16_t machine) {
    switch (machine) {
        case COFF_MACHINE_I386:
        case COFF_MACHINE_ARMNT: return 4;
        default:                 return 8;
    }
}

/* Generates a minimal COFF object file containing:
 *   .rdata section:  [pak_blob] [size_t value = pak_blob_size]
 *   symbol table:    assets_pak_data, assets_pak_data_size
 *
 * The size value width matches the target pointer size (4 or 8 bytes)
 * so that `extern const size_t assets_pak_data_size;` is correct on
 * both 32-bit and 64-bit targets.
 */
static std::vector<uint8_t> generate_coff_obj(
    const std::vector<uint8_t> &pak_data,
    uint16_t machine)
{
    /* -- Constants ---------------------------------------------- */
    const uint64_t blob_size = pak_data.size();
    const int ptr_size = coff_pointer_size(machine);

    /* Pad blob so that the size value is naturally aligned (8 bytes for
     * 64-bit targets).  ARM64 LDR requires the address to be aligned to
     * the access width; placing size_t right after an arbitrarily-sized
     * blob would violate that when blob_size % ptr_size != 0. */
    const uint64_t size_offset =
        (blob_size + static_cast<uint64_t>(ptr_size) - 1u) &
        ~(static_cast<uint64_t>(ptr_size) - 1u);

    /* .rdata content = pak_blob + padding + ptr_size-byte size value. */
    const uint64_t rdata_size = size_offset + static_cast<uint64_t>(ptr_size);

    /* Align section data to 4 bytes (COFF requirement). */
    const uint64_t rdata_aligned =
        (rdata_size + 3u) & ~uint64_t(3);

    /* Symbol names.  COFF uses 8-byte inline names or a 4-byte-zero
     * prefix + 4-byte offset into the string table for longer names. */
    const char *sym_data = "assets_pak_data";
    const char *sym_size = "assets_pak_data_size";

    /* Both names > 8 chars  stored in the string table. */
    const uint32_t strtab_off_data = 4;  /* first entry after the 4-byte length */
    const uint32_t strtab_off_size =
        strtab_off_data + static_cast<uint32_t>(std::strlen(sym_data)) + 1;
    const uint32_t strtab_total =
        strtab_off_size + static_cast<uint32_t>(std::strlen(sym_size)) + 1;

    /* -- Layout ------------------------------------------------- */
    /* COFF Header:          20 bytes
     * Section Header (.rdata): 40 bytes
     * Section Data:         rdata_aligned bytes
     * Symbol Table:         2  18 bytes = 36 bytes
     * String Table:         strtab_total bytes                  */

    const uint32_t coff_header_size   = 20;
    const uint32_t section_hdr_size   = 40;
    const uint32_t section_data_off   = coff_header_size + section_hdr_size;
    const uint32_t symtab_off         =
        section_data_off + static_cast<uint32_t>(rdata_aligned);
    const uint32_t num_symbols        = 2;

    std::vector<uint8_t> obj;
    obj.reserve(static_cast<size_t>(symtab_off) + 36 + strtab_total);

    /* -- COFF File Header (20 bytes) --------------------------- */
    write_le16(obj, machine);          /* Machine                    */
    write_le16(obj, 1);                /* NumberOfSections            */
    write_le32(obj, 0);                /* TimeDateStamp               */
    write_le32(obj, symtab_off);       /* PointerToSymbolTable        */
    write_le32(obj, num_symbols);      /* NumberOfSymbols             */
    write_le16(obj, 0);                /* SizeOfOptionalHeader        */
    write_le16(obj, 0);                /* Characteristics             */

    /* -- Section Header: .rdata (40 bytes) --------------------- */
    /* Name: ".rdata\0\0" (8 bytes, padded) */
    const char sec_name[8] = {'.','r','d','a','t','a',0,0};
    obj.insert(obj.end(), sec_name, sec_name + 8);
    write_le32(obj, 0);                               /* VirtualSize      */
    write_le32(obj, 0);                               /* VirtualAddress   */
    write_le32(obj, static_cast<uint32_t>(rdata_size)); /* SizeOfRawData  */
    write_le32(obj, section_data_off);                 /* PointerToRawData */
    write_le32(obj, 0);                               /* PointerToRelocs  */
    write_le32(obj, 0);                               /* PointerToLinenos */
    write_le16(obj, 0);                               /* NumRelocations   */
    write_le16(obj, 0);                               /* NumLinenumbers   */
    /* Characteristics: INITIALIZED_DATA | ALIGN_16 | MEM_READ */
    write_le32(obj, 0x40500040u);

    /* -- Section Data: .rdata ----------------------------------- */
    obj.insert(obj.end(), pak_data.begin(), pak_data.end());

    /* Pad to align the size value to ptr_size boundary. */
    while (obj.size() < static_cast<size_t>(section_data_off + size_offset))
        obj.push_back(0);

    /* Size value: width matches target pointer size (4 or 8 bytes). */
    for (int i = 0; i < ptr_size; ++i)
        obj.push_back(static_cast<uint8_t>(blob_size >> (i * 8)));

    /* Pad to alignment. */
    while (obj.size() < static_cast<size_t>(symtab_off))
        obj.push_back(0);

    /* -- Symbol Table (2  18 bytes) --------------------------- */

    /* Symbol 0: assets_pak_data  points to offset 0 in .rdata */
    /* Name (8 bytes): zeros(4) + strtab offset(4) */
    write_le32(obj, 0);
    write_le32(obj, strtab_off_data);
    write_le32(obj, 0);                /* Value: offset 0             */
    write_le16(obj, 1);                /* SectionNumber: 1 (.rdata)   */
    write_le16(obj, 0);                /* Type                        */
    obj.push_back(2);                  /* StorageClass: EXTERNAL      */
    obj.push_back(0);                  /* NumberOfAuxSymbols           */

    /* Symbol 1: assets_pak_data_size  points to aligned offset */
    write_le32(obj, 0);
    write_le32(obj, strtab_off_size);
    write_le32(obj, static_cast<uint32_t>(size_offset)); /* Value   */
    write_le16(obj, 1);                /* SectionNumber: 1            */
    write_le16(obj, 0);                /* Type                        */
    obj.push_back(2);                  /* StorageClass: EXTERNAL      */
    obj.push_back(0);                  /* NumberOfAuxSymbols           */

    /* -- String Table ------------------------------------------- */
    write_le32(obj, strtab_total);     /* 4-byte length prefix        */
    obj.insert(obj.end(), sym_data, sym_data + std::strlen(sym_data) + 1);
    obj.insert(obj.end(), sym_size, sym_size + std::strlen(sym_size) + 1);

    return obj;
}

/* ================================================================== */
/* C-array generator (for Emscripten / platforms without .incbin)       */
/* ================================================================== */

static std::string generate_c_array(const std::vector<uint8_t> &pak_data) {
    std::string c;
    c += "/* Auto-generated by jce_pak -- DO NOT EDIT */\n";
    c += "#include <stddef.h>\n\n";
    c += "const unsigned char assets_pak_data[] = {\n";

    for (size_t i = 0; i < pak_data.size(); ++i) {
        if (i % 16 == 0) c += "    ";
        char hex[8];
        std::snprintf(hex, sizeof(hex), "0x%02X", pak_data[i]);
        c += hex;
        if (i + 1 < pak_data.size()) c += ',';
        if (i % 16 == 15 || i + 1 == pak_data.size()) c += '\n';
    }

    c += "};\n\n";
    c += "const size_t assets_pak_data_size = sizeof(assets_pak_data);\n";
    return c;
}

/* ================================================================== */
/* Header / manifest generators                                        */
/* ================================================================== */

static std::string generate_header(const std::vector<AssetEntry> &entries) {
    std::string h;
    h += "/* Auto-generated by jce_pak -- DO NOT EDIT */\n";
    h += "#pragma once\n";
    h += "#include <stddef.h>\n\n";
    h += "#ifdef __cplusplus\n";
    h += "extern \"C\" {\n";
    h += "#endif\n\n";
    h += "/* Raw PAK blob linked into the executable (.obj / .incbin). */\n";
    h += "extern const unsigned char assets_pak_data[];\n";
    h += "extern const size_t        assets_pak_data_size;\n\n";
    h += "#ifdef __cplusplus\n";
    h += "}\n";
    h += "#endif\n";
    return h;
}

static std::string generate_manifest(
    const std::vector<AssetEntry> &entries,
    uint64_t pak_total)
{
    std::string m;
    m += "# Auto-generated by jce_pak -- DO NOT EDIT\n";

    /* ASSET_PATHS */
    m += "set(ASSET_PATHS \"";
    for (size_t i = 0; i < entries.size(); ++i) {
        if (i) m += ';';
        m += entries[i].rel_path;
    }
    m += "\")\n";

    /* ASSET_SIZES (original) */
    m += "set(ASSET_SIZES \"";
    for (size_t i = 0; i < entries.size(); ++i) {
        if (i) m += ';';
        m += std::to_string(entries[i].original_size);
    }
    m += "\")\n";

    /* ASSET_COMPRESSED (compressed per-asset sizes) */
    m += "set(ASSET_COMPRESSED \"";
    for (size_t i = 0; i < entries.size(); ++i) {
        if (i) m += ';';
        m += std::to_string(entries[i].compressed.size());
    }
    m += "\")\n";

    /* Totals */
    uint64_t raw_total = 0;
    uint64_t comp_total = 0;
    for (auto &e : entries) {
        raw_total  += e.original_size;
        comp_total += e.compressed.size();
    }

    m += "set(ASSET_RAW_TOTAL "  + std::to_string(raw_total)  + ")\n";
    m += "set(ASSET_COMP_TOTAL " + std::to_string(comp_total) + ")\n";
    m += "set(ASSET_PAK_TOTAL "  + std::to_string(pak_total)  + ")\n";
    m += "set(ASSET_FILE_COUNT " + std::to_string(entries.size()) + ")\n";

    return m;
}

/* ================================================================== */
/* CLI argument parsing                                                */
/* ================================================================== */

struct Args {
    fs::path resource_dir;
    fs::path pak_file;
    fs::path obj_file;       /* empty  skip COFF generation */
    fs::path header_file;
    fs::path manifest_file;
    fs::path c_file;         /* empty  skip C-array generation */
    std::string obj_format;  /* "coff" | "c-array" | "none" */
    std::string obj_arch;    /* "x64" | "arm64" | "x86" | "arm" */
};

static void usage() {
    std::cerr <<
        "Usage: jce_pak --resource-dir <dir>\n"
        "               --pak-file      <out.pak>\n"
        "               --header-file   <out.h>\n"
        "               --manifest-file <out.cmake>\n"
        "              [--obj-file      <out.obj>]\n"
        "              [--c-file        <out.c>]     (for c-array format)\n"
        "              [--obj-format    coff|c-array|none]   (default: none)\n"
        "              [--obj-arch      x64|arm64|x86|arm]  (default: x64)\n";
}

static Args parse_args(int argc, char *argv[]) {
    Args a{};
    a.obj_format = "none";
    a.obj_arch   = "x64";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "[jce_pak] missing value for " << arg << "\n";
                std::exit(1);
            }
            return argv[++i];
        };
        if      (arg == "--resource-dir")  a.resource_dir  = next();
        else if (arg == "--pak-file")      a.pak_file      = next();
        else if (arg == "--obj-file")      a.obj_file      = next();
        else if (arg == "--header-file")   a.header_file   = next();
        else if (arg == "--manifest-file") a.manifest_file = next();
        else if (arg == "--c-file")         a.c_file        = next();
        else if (arg == "--obj-format")    a.obj_format    = next();
        else if (arg == "--obj-arch")      a.obj_arch      = next();
        else {
            std::cerr << "[jce_pak] unknown argument: " << arg << "\n";
            usage();
            std::exit(1);
        }
    }

    if (a.resource_dir.empty() || a.pak_file.empty() ||
        a.header_file.empty()  || a.manifest_file.empty()) {
        usage();
        std::exit(1);
    }
    return a;
}

/* ================================================================== */
/* main                                                                */
/* ================================================================== */

int main(int argc, char *argv[]) {
    Args args = parse_args(argc, argv);

    if (!fs::is_directory(args.resource_dir)) {
        std::cerr << "[jce_pak] resource dir does not exist: "
                  << args.resource_dir << "\n";
        return 1;
    }

    /* -- 1. Enumerate files ------------------------------------ */
    std::vector<fs::path> files;
    for (auto &de : fs::recursive_directory_iterator(args.resource_dir)) {
        if (!de.is_regular_file()) continue;
        auto rel = fs::relative(de.path(), args.resource_dir);
        std::string s = normalise(rel);
        /* Skip hidden / dot-files. */
        if (s.find("/.") != std::string::npos || s[0] == '.')
            continue;
        files.push_back(de.path());
    }
    std::sort(files.begin(), files.end());

    /* -- 2. Compress & hash ------------------------------------ */
    std::vector<AssetEntry> entries;
    entries.reserve(files.size());

    for (auto &fpath : files) {
        AssetEntry e{};
        e.rel_path = normalise(fs::relative(fpath, args.resource_dir));
        e.path_hash = XXH3_64bits(e.rel_path.data(), e.rel_path.size());

        auto raw = read_file(fpath);
        e.original_size = raw.size();

        size_t bound = ZSTD_compressBound(raw.size());
        e.compressed.resize(bound);
        size_t comp_sz = ZSTD_compress(
            e.compressed.data(), bound,
            raw.data(), raw.size(),
            3 /* default level */);
        if (ZSTD_isError(comp_sz)) {
            std::cerr << "[jce_pak] ZSTD error compressing "
                      << e.rel_path << ": "
                      << ZSTD_getErrorName(comp_sz) << "\n";
            return 1;
        }
        e.compressed.resize(comp_sz);

        entries.push_back(std::move(e));
    }

    /* Sort TOC by path_hash for binary search at runtime. */
    std::sort(entries.begin(), entries.end(),
              [](const AssetEntry &a, const AssetEntry &b) {
                  return a.path_hash < b.path_hash;
              });

    /* -- 3. Build .pak blob ------------------------------------ */

    /* 3a. Compute names section. */
    uint32_t names_offset = 0;
    for (auto &e : entries) {
        e.name_offset = names_offset;
        e.name_length = static_cast<uint32_t>(e.rel_path.size());
        names_offset += e.name_length;
    }
    const uint32_t names_size = names_offset;

    /* 3b. Compute data offsets. */
    uint64_t data_cursor = 0;
    for (auto &e : entries) {
        e.data_offset = data_cursor;
        data_cursor  += e.compressed.size();
    }

    /* 3c. Compute section offsets in the file. */
    const uint64_t toc_offset  = JPAK_HEADER_SIZE;
    const uint64_t toc_size    =
        static_cast<uint64_t>(entries.size()) * JPAK_TOC_ENTRY_SIZE;
    const uint64_t names_off   = toc_offset + toc_size;
    const uint64_t data_off    = names_off + names_size;
    const uint64_t pak_total   = data_off + data_cursor;

    /* 3d. Serialise. */
    std::vector<uint8_t> pak;
    pak.reserve(static_cast<size_t>(pak_total));

    /* Header. */
    pak.push_back(JPAK_MAGIC_0);
    pak.push_back(JPAK_MAGIC_1);
    pak.push_back(JPAK_MAGIC_2);
    pak.push_back(JPAK_MAGIC_3);
    write_le32(pak, JPAK_VERSION);
    write_le32(pak, static_cast<uint32_t>(entries.size()));
    write_le32(pak, 0); /* flags */
    write_le64(pak, toc_offset);
    write_le64(pak, data_off);

    /* TOC entries. */
    for (auto &e : entries) {
        write_le64(pak, e.path_hash);
        /* name_offset is relative to names section start; store as
         * absolute offset for simplicity in the loader. */
        write_le32(pak, static_cast<uint32_t>(names_off + e.name_offset));
        write_le32(pak, e.name_length);
        write_le64(pak, e.data_offset);
        write_le64(pak, e.compressed.size());
        write_le64(pak, e.original_size);
    }

    /* Names section. */
    for (auto &e : entries) {
        pak.insert(pak.end(), e.rel_path.begin(), e.rel_path.end());
    }

    /* Data section. */
    for (auto &e : entries) {
        pak.insert(pak.end(), e.compressed.begin(), e.compressed.end());
    }

    /* -- 4. Write outputs -------------------------------------- */

    write_file(args.pak_file, pak);
    write_file(args.header_file, generate_header(entries));
    write_file(args.manifest_file,
               generate_manifest(entries, pak_total));

    /* COFF .obj */
    if (args.obj_format == "coff" && !args.obj_file.empty()) {
        uint16_t machine = coff_machine_from_arch(args.obj_arch);
        auto obj = generate_coff_obj(pak, machine);
        write_file(args.obj_file, obj);
        std::cout << "[jce_pak] COFF .obj written: "
                  << args.obj_file << " ("
                  << obj.size() << " bytes)\n";
    }

    /* C-array (Emscripten / platforms without .incbin). */
    if (args.obj_format == "c-array" && !args.c_file.empty()) {
        auto c_src = generate_c_array(pak);
        write_file(args.c_file, c_src);
        std::cout << "[jce_pak] C-array written: "
                  << args.c_file << " ("
                  << c_src.size() << " bytes)\n";
    }

    /* -- 5. Summary -------------------------------------------- */

    uint64_t raw_total = 0, comp_total = 0;
    for (auto &e : entries) {
        raw_total  += e.original_size;
        comp_total += e.compressed.size();
    }

    std::cout << "[jce_pak] " << entries.size() << " files packed | raw "
              << raw_total << " B -> compressed " << comp_total
              << " B -> pak " << pak_total << " B\n";

    return 0;
}
