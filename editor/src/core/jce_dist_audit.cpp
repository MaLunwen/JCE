/* jce_dist_audit.cpp  Dist artifact parser and fail-closed audit report. */

#include "jce_dist_audit.h"

extern "C" {
#include <jce/os/core/jce_filesystem.h>
#include <jce/resource/jce_archive.h>
}

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Keep this OS-owned DLL list identical to tools/build/standalone.py.
 * API-set prefixes below retain that owner's exact acceptance semantics. */
constexpr const char *system_dlls[] = {
    "advapi32", "bcrypt", "cabinet", "cfgmgr32", "comctl32", "comdlg32", "crypt32",
    "cryptbase", "d3d11", "d3d12", "d3dcompiler_47", "dbghelp", "dnsapi", "dwmapi",
    "dwrite", "dxgi", "gdi32", "gdiplus", "hid", "imm32", "iphlpapi", "kernel32",
    "kernelbase", "mpr", "msimg32", "msvcrt", "ncrypt", "netapi32", "normaliz", "ntdll",
    "ole32", "oleaut32", "opengl32", "powrprof", "psapi", "rpcrt4", "secur32",
    "setupapi", "shell32", "shlwapi", "user32", "userenv", "usp10", "uxtheme",
    "version", "winhttp", "wininet", "winmm", "winspool", "wintrust", "wldap32",
    "ws2_32", "wsock32", "wtsapi32"
};

bool system_import(const std::string &name)
{
    if (name.compare(0, 16, "api-ms-win-core-") == 0 ||
        name.compare(0, 11, "ext-ms-win-") == 0)
        return true;
    if (name.size() < 5u || name.compare(name.size() - 4u, 4, ".dll") != 0)
        return false;
    const std::string stem = name.substr(0, name.size() - 4u);
    return std::find(std::begin(system_dlls), std::end(system_dlls), stem) !=
           std::end(system_dlls);
}

/* A mapped range cannot run into another section, an overlay or the embedded
 * PAK. All additions use file-bounded subtraction or 64-bit RVA arithmetic. */
bool pe_imports(const uint8_t *data, size_t size, uint16_t *machine,
                std::vector<std::string> &imports, std::string &error)
{
    auto invalid = [&](const char *reason) {
        error = reason;
        return false;
    };
    if (size < 0x40u || data[0] != 'M' || data[1] != 'Z')
        return invalid("not a PE executable");
    const size_t pe = read_le32(data + 0x3cu);
    if (pe > size || size - pe < 24u || std::memcmp(data + pe, "PE\0\0", 4) != 0)
        return invalid("invalid or truncated PE signature");
    *machine = read_le16(data + pe + 4u);
    const size_t section_count = read_le16(data + pe + 6u);
    const size_t optional_size = read_le16(data + pe + 20u);
    const size_t optional = pe + 24u;
    if (optional_size < 2u || optional_size > size - optional)
        return invalid("truncated PE optional header");
    const uint16_t magic = read_le16(data + optional);
    if (!((*machine == 0x8664u || *machine == 0xaa64u) && magic == 0x20bu) &&
        !(*machine == 0x014cu && magic == 0x10bu))
        return invalid("unsupported PE architecture");
    const size_t directory_base = magic == 0x20bu ? 112u : 96u;
    if (optional_size < directory_base)
        return invalid("truncated PE data directories");
    const uint32_t directory_count = read_le32(data + optional + directory_base - 4u);
    if (directory_count > (optional_size - directory_base) / 8u)
        return invalid("PE data directories exceed optional header");
    const size_t section_table = optional + optional_size;
    if (section_count > (size - section_table) / 40u)
        return invalid("truncated PE section table");
    const uint64_t image_base = magic == 0x20bu
        ? (uint64_t)read_le32(data + optional + 24u) |
          (uint64_t)read_le32(data + optional + 28u) << 32u
        : read_le32(data + optional + 28u);
    auto map_rva = [&](uint64_t rva, size_t *offset, size_t *available) {
        for (size_t i = 0; i < section_count; ++i) {
            const uint8_t *section = data + section_table + i * 40u;
            const uint64_t virtual_size = read_le32(section + 8u);
            const uint64_t address = read_le32(section + 12u);
            const uint64_t raw_size = read_le32(section + 16u);
            const uint64_t raw = read_le32(section + 20u);
            const uint64_t extent = std::max(virtual_size, raw_size);
            if (rva < address || rva - address >= extent || rva - address >= raw_size)
                continue;
            const uint64_t delta = rva - address;
            if (raw > size || raw_size > size - raw)
                return false;
            *offset = (size_t)(raw + delta);
            *available = (size_t)(raw_size - delta);
            return true;
        }
        return false;
    };
    for (unsigned directory : {1u, 13u}) {
        if (directory_count <= directory) continue;
        const uint8_t *entry = data + optional + directory_base + directory * 8u;
        const uint32_t rva = read_le32(entry);
        const size_t declared_size = read_le32(entry + 4u);
        if (rva == 0 && declared_size == 0) continue;
        size_t pos = 0, available = 0;
        const size_t width = directory == 1u ? 20u : 32u;
        if (rva == 0 || !map_rva(rva, &pos, &available) ||
            declared_size < width || declared_size > available)
            return invalid("PE import directory outside mapped file section");
        const size_t end = pos + declared_size;
        bool terminated = false;
        while (width <= end - pos) {
            bool empty = true;
            for (size_t field = 0; field < width; field += 4u)
                empty = empty && read_le32(data + pos + field) == 0;
            if (empty) {
                terminated = true;
                break;
            }
            uint64_t name_rva = read_le32(data + pos + (directory == 1u ? 12u : 4u));
            if (directory == 13u && !(read_le32(data + pos) & 1u)) {
                if (name_rva < image_base)
                    return invalid("PE delayed import VA precedes image base");
                name_rva -= image_base;
            }
            size_t name_offset = 0, name_available = 0;
            if (!map_rva(name_rva, &name_offset, &name_available))
                return invalid("PE import name outside mapped file section");
            const size_t name_limit = std::min(name_available, (size_t)260u);
            size_t length = 0;
            std::string name;
            while (length < name_limit && data[name_offset + length] != 0) {
                const uint8_t value = data[name_offset + length++];
                if (value > 0x7fu)
                    return invalid("PE import name is not ASCII");
                name.push_back((char)(value >= 'A' && value <= 'Z' ? value + 'a' - 'A' : value));
            }
            if (length == name_limit || name.empty())
                return invalid("empty or unterminated PE import name");
            if (std::find(imports.begin(), imports.end(), name) == imports.end())
                imports.push_back(name);
            pos += width;
        }
        if (!terminated)
            return invalid("unterminated PE import table");
    }
    std::sort(imports.begin(), imports.end());
    return true;
}

size_t find_bytes_from(const uint8_t *data, size_t size,
                       const uint8_t *needle, size_t needle_size,
                       size_t start)
{
    if (!data || !needle || needle_size == 0 || start > size ||
        needle_size > size - start) {
        return SIZE_MAX;
    }

    size_t skip[256];
    std::fill(std::begin(skip), std::end(skip), needle_size);
    for (size_t i = 0; i + 1u < needle_size; ++i)
        skip[needle[i]] = needle_size - i - 1u;

    size_t pos = start;
    const size_t last = needle_size - 1u;
    while (pos <= size - needle_size) {
        size_t i = needle_size;
        while (i > 0 && data[pos + i - 1u] == needle[i - 1u]) --i;
        if (i == 0) return pos;
        pos += skip[data[pos + last]];
    }
    return SIZE_MAX;
}

std::string json_escape(const std::string &s)
{
    std::string out;
    out.reserve(s.size() + 8u);
    for (unsigned char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20u) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c);
                    out += buf;
                } else {
                    out.push_back((char)c);
                }
                break;
        }
    }
    return out;
}

void set_error(JceDistAuditResult &r, const std::string &message)
{
    if (r.error[0] == '\0')
        std::snprintf(r.error, sizeof(r.error), "%s", message.c_str());
}

struct WalkCount {
    uint32_t files = 0;
};

bool count_file(const char *, bool is_dir, void *user)
{
    WalkCount *count = (WalkCount *)user;
    if (!is_dir && count) ++count->files;
    return true;
}

bool write_report(const JceDistAuditInput &input,
                  const JceDistAuditResult &r,
                  const std::vector<std::string> &leaks,
                  const std::vector<std::string> &observed_addresses,
                  const std::vector<std::string> &imports)
{
    if (!input.report_path || !input.report_path[0]) return true;
    std::string json;
    json.reserve(2048u + leaks.size() * 96u);
    json += "{\n  \"schema\": \"jce.dist-audit.v1\",\n";
    json += "  \"passed\": "; json += r.passed ? "true,\n" : "false,\n";
    json += "  \"executable\": \"";
    json += json_escape(input.executable_path ? input.executable_path : "");
    json += "\",\n";
    json += "  \"pe\": {\"valid\": ";
    json += r.pe_valid ? "true" : "false";
    json += ", \"subsystem\": " + std::to_string(r.pe_subsystem);
    json += ", \"machine\": " + std::to_string(r.pe_machine);
    json += ", \"expected_machine\": " + std::to_string(input.expected_pe_machine);
    json += ", \"imports_verified\": ";
    json += r.pe_imports_verified ? "true" : "false";
    json += ", \"imports\": [";
    for (size_t i = 0; i < imports.size(); ++i) {
        if (i) json += ", ";
        json += "\"" + json_escape(imports[i]) + "\"";
    }
    json += "]},\n";
    json += "  \"embedded_pak\": {\"exactly_once\": ";
    json += r.pak_embedded_exactly_once ? "true" : "false";
    json += ", \"offset\": ";
    json += r.pak_offset == SIZE_MAX ? "null" : std::to_string(r.pak_offset);
    json += "},\n";
    json += "  \"archive\": {\"entries\": " +
            std::to_string(r.archive_entries) +
            ", \"encrypted\": " + std::to_string(r.encrypted_entries) +
            ", \"authenticated\": " +
            std::to_string(r.authenticated_entries) +
            ", \"verified\": " + std::to_string(r.verified_entries) +
            "},\n";
    json += "  \"bundle_graph\": {\"snapshot\": \"";
    json += json_escape(input.graph_snapshot_path
                            ? input.graph_snapshot_path : "");
    json += "\", \"expected\": " + std::to_string(r.graph_expected) +
            ", \"verified\": " + std::to_string(r.graph_verified) +
            ", \"mismatches\": " +
            std::to_string(r.graph_mismatch_count) + "},\n";
    json += "  \"package_file_count\": " +
            std::to_string(r.package_file_count) + ",\n";
    json += "  \"leaked_paths\": [";
    for (size_t i = 0; i < leaks.size(); ++i) {
        json += i ? ",\n    \"" : "\n    \"";
        json += json_escape(leaks[i]);
        json += "\"";
    }
    json += leaks.empty() ? "],\n" : "\n  ],\n";
    json += "  \"address_literals_outside_project_pak\": [";
    for (size_t i = 0; i < observed_addresses.size(); ++i) {
        json += i ? ",\n    \"" : "\n    \"";
        json += json_escape(observed_addresses[i]);
        json += "\"";
    }
    json += observed_addresses.empty() ? "],\n" : "\n  ],\n";
    json += "  \"address_literal_scope\": \"selected non-confidential address bytes outside the project PAK; does not prove plaintext payload leakage or runtime use\",\n";
    json += "  \"error\": ";
    if (r.error[0]) json += "\"" + json_escape(r.error) + "\"\n";
    else json += "null\n";
    json += "}\n";
    return jce_fs_host_write_all(input.report_path, json.data(), json.size());
}

} // namespace

bool jce_dist_audit_pe_subsystem(const uint8_t *data, size_t size,
                                 uint16_t *out_subsystem)
{
    if (!data || !out_subsystem || size < 0x40u || data[0] != 'M' ||
        data[1] != 'Z') {
        return false;
    }
    const uint32_t pe = read_le32(data + 0x3cu);
    if ((uint64_t)pe + 24u + 70u > size || data[pe] != 'P' ||
        data[pe + 1u] != 'E' || data[pe + 2u] != 0 ||
        data[pe + 3u] != 0) {
        return false;
    }
    const uint16_t optional_size = read_le16(data + pe + 20u);
    const size_t optional = (size_t)pe + 24u;
    if (optional_size < 70u || optional + optional_size > size) return false;
    const uint16_t magic = read_le16(data + optional);
    if (magic != 0x10bu && magic != 0x20bu) return false;
    *out_subsystem = read_le16(data + optional + 68u);
    return true;
}

size_t jce_dist_audit_find_outside_range(
    const uint8_t *data, size_t size, const uint8_t *needle,
    size_t needle_size, size_t excluded_offset, size_t excluded_size)
{
    if (excluded_offset > size || excluded_size > size - excluded_offset)
        return find_bytes_from(data, size, needle, needle_size, 0);

    size_t found = find_bytes_from(data, excluded_offset, needle,
                                   needle_size, 0);
    if (found != SIZE_MAX) return found;
    const size_t after = excluded_offset + excluded_size;
    return find_bytes_from(data, size, needle, needle_size, after);
}

bool jce_dist_audit_run(const JceDistAuditInput &input,
                        JceDistAuditResult *out_result)
{
    if (!out_result) return false;
    JceDistAuditResult r{};
    std::vector<std::string> leaks;
    std::vector<std::string> observed_addresses;
    std::vector<std::string> imports;
    uint64_t exe_size_u64 = 0;
    uint64_t pak_size_u64 = 0;
    uint8_t *exe = (uint8_t *)jce_fs_host_read_all(input.executable_path,
                                                    &exe_size_u64);
    uint8_t *pak = (uint8_t *)jce_fs_host_read_all(input.pak_path,
                                                    &pak_size_u64);
    if (!exe || !pak || exe_size_u64 > SIZE_MAX || pak_size_u64 > SIZE_MAX) {
        set_error(r, "cannot read executable or generated PAK");
        goto finish;
    }

    {
        const size_t exe_size = (size_t)exe_size_u64;
        const size_t pak_size = (size_t)pak_size_u64;
        r.pak_offset = find_bytes_from(exe, exe_size, pak, pak_size, 0);
        if (r.pak_offset != SIZE_MAX) {
            const size_t second = find_bytes_from(
                exe, exe_size, pak, pak_size, r.pak_offset + 1u);
            r.pak_embedded_exactly_once = second == SIZE_MAX;
        }
        if (!r.pak_embedded_exactly_once)
            set_error(r, "generated PAK is missing or duplicated in executable");

        r.pe_valid = jce_dist_audit_pe_subsystem(exe, exe_size,
                                                  &r.pe_subsystem);
        if (input.require_gui && (!r.pe_valid || r.pe_subsystem != 2u))
            set_error(r, "Windows dist executable is not GUI subsystem");
        if (input.expected_pe_machine != 0u) {
            std::string pe_error;
            if (!pe_imports(exe, exe_size, &r.pe_machine, imports, pe_error)) {
                r.pe_valid = false;
                set_error(r, pe_error);
            } else {
                r.pe_import_count = (uint32_t)imports.size();
                if (input.expected_pe_machine != r.pe_machine)
                    set_error(r, "Windows dist PE machine does not match requested architecture");
                r.pe_imports_verified = !imports.empty();
                for (const std::string &name : imports)
                    r.pe_imports_verified = r.pe_imports_verified && system_import(name);
                if (!r.pe_imports_verified)
                    set_error(r, imports.empty() ? "PE artifact has no imports; cannot verify delivery"
                              : "single EXE still requires external DLLs");
            }
        }

        JceArchive *archive = jce_archive_open(pak, pak_size);
        if (!archive && input.key) {
            jce_archive_set_process_key(input.key);
            archive = jce_archive_open(pak, pak_size);
        }
        if (!archive) {
            set_error(r, "generated PAK cannot be opened");
        } else {
            if (input.key) jce_archive_set_decryption_key(archive, input.key);
            const bool secure_ok = jce_archive_is_secure(archive) &&
                jce_archive_is_authenticated(archive) &&
                jce_archive_auth_status(archive) == JCE_ARCHIVE_AUTH_VALID;
            if (input.require_secure && !secure_ok)
                set_error(r, "generated PAK is not authenticated secure mode");
            if (!jce_archive_verify_header(archive))
                set_error(r, "generated PAK content hashes do not verify");

            r.archive_entries = jce_archive_count(archive);
            for (uint32_t i = 0; i < r.archive_entries; ++i) {
                const JceArchiveEntry *entry = jce_archive_get(archive, i);
                if (!entry) continue;
                if (entry->entry_flags & JCE_ARCHIVE_ENTRY_ENCRYPTED)
                    ++r.encrypted_entries;
                if (entry->entry_flags & JCE_ARCHIVE_ENTRY_AUTHENTICATED)
                    ++r.authenticated_entries;
                if (jce_archive_debug_path(archive, i))
                    set_error(r, "secure PAK contains a debug path table");
                std::vector<uint8_t> decoded(
                    entry->original_size ? entry->original_size : 1u);
                const size_t got = jce_archive_read(
                    archive, entry, decoded.data(), decoded.size());
                if (got == entry->original_size &&
                    (entry->original_size == 0 ||
                     jce_archive_verify_entry(entry, decoded.data(), got))) {
                    ++r.verified_entries;
                }
            }
            if (r.verified_entries != r.archive_entries ||
                (input.require_secure &&
                 (r.encrypted_entries != r.archive_entries ||
                  r.authenticated_entries != r.archive_entries))) {
                set_error(r, "one or more PAK entries failed secure verification");
            }

            r.graph_expected = (uint32_t)input.graph_asset_count;
            if (input.require_graph_parity && input.graph_asset_count == 0)
                set_error(r, "approved Bundle graph is empty or missing");
            for (size_t i = 0; i < input.graph_asset_count; ++i) {
                const JceDistGraphAsset &expected = input.graph_assets[i];
                const JceArchiveEntry *entry = expected.address
                    ? jce_archive_find(archive, expected.address) : nullptr;
                if (!entry || entry->original_size > (uint64_t)SIZE_MAX) {
                    ++r.graph_mismatch_count;
                    continue;
                }
                std::vector<uint8_t> decoded(
                    entry->original_size ? entry->original_size : 1u);
                const size_t got = jce_archive_read(
                    archive, entry, decoded.data(), decoded.size());
                if (got != entry->original_size ||
                    jce_archive_content_hash(decoded.data(), got) !=
                        expected.content_id) {
                    ++r.graph_mismatch_count;
                    continue;
                }
                ++r.graph_verified;
            }
            if (r.graph_mismatch_count != 0 ||
                (input.require_graph_parity &&
                 r.graph_verified != r.graph_expected)) {
                set_error(r, "Dist PAK does not match approved Bundle graph");
            }
            jce_archive_close(archive);
        }

        if (r.pak_offset != SIZE_MAX) {
            for (size_t i = 0; i < input.protected_path_count; ++i) {
                const char *path = input.protected_paths[i];
                if (!path || !path[0]) continue;
                std::vector<char> normalized(std::strlen(path) + 1u);
                size_t len = jce_archive_normalize_path(
                    path, normalized.data(), normalized.size());
                if (len == 0) continue;
                size_t found = jce_dist_audit_find_outside_range(
                    exe, exe_size, (const uint8_t *)normalized.data(), len,
                    r.pak_offset, pak_size);
                if (found != SIZE_MAX) leaks.emplace_back(normalized.data(), len);
            }
            for (size_t i = 0; i < input.graph_asset_count; ++i) {
                const char *path = input.graph_assets[i].address;
                if (!path || !path[0]) continue;
                std::vector<char> normalized(std::strlen(path) + 1u);
                const size_t len = jce_archive_normalize_path(path, normalized.data(), normalized.size());
                if (len == 0) continue;
                const std::string name(normalized.data(), len);
                if (std::find(leaks.begin(), leaks.end(), name) != leaks.end()) continue;
                if (jce_dist_audit_find_outside_range(exe, exe_size,
                        (const uint8_t *)normalized.data(), len, r.pak_offset, pak_size) != SIZE_MAX &&
                    std::find(observed_addresses.begin(), observed_addresses.end(), name) == observed_addresses.end())
                    observed_addresses.push_back(name);
            }
        }
        r.leaked_path_count = (uint32_t)leaks.size();
        r.observed_address_literal_count = (uint32_t)observed_addresses.size();
        if (!leaks.empty()) set_error(r, "confidential asset names are visible outside project PAK");
    }

    if (input.package_dir && input.package_dir[0]) {
        WalkCount count;
        if (!jce_fs_host_walk(input.package_dir, count_file, &count)) {
            set_error(r, "cannot inspect staged package directory");
        }
        r.package_file_count = count.files;
        if (input.require_single_file) {
            std::string expected = input.package_dir;
            expected += '/';
            expected += input.expected_exe_name ? input.expected_exe_name : "";
            if (count.files != 1u ||
                !jce_fs_host_exists_file(expected.c_str())) {
                set_error(r, "dist package must contain exactly one executable");
            }
        }
    }

finish:
    if (exe) jce_fs_buffer_free(exe);
    if (pak) jce_fs_buffer_free(pak);
    r.passed = r.error[0] == '\0';
    if (!write_report(input, r, leaks, observed_addresses, imports) && r.passed) {
        set_error(r, "cannot write dist audit report");
        r.passed = false;
    }
    *out_result = r;
    return r.passed;
}
