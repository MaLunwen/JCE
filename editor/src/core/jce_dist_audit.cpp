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
                  const std::vector<std::string> &leaks)
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
    json += ", \"subsystem\": " + std::to_string(r.pe_subsystem) + "},\n";
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
        }
        r.leaked_path_count = (uint32_t)leaks.size();
        if (!leaks.empty()) set_error(r, "asset virtual paths leaked outside PAK");
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
    if (!write_report(input, r, leaks) && r.passed) {
        set_error(r, "cannot write dist audit report");
        r.passed = false;
    }
    *out_result = r;
    return r.passed;
}
