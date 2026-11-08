#include "jce_dist_name_policy.h"
extern "C" {
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
#include <jce/resource/jce_archive.h>
}
#include <algorithm>
#include <cstring>
#include <memory>
#include <new>
#include <string_view>

bool jce_dist_name_policy_load(const std::string &manifest_path,
                               std::vector<std::string> *out_paths,
                               std::string *out_error)
{
    auto fail = [&](const char *reason) {
        if (out_paths) out_paths->clear();
        if (out_error) *out_error = std::string("confidential_asset_paths: ") + reason;
        return false;
    };
    if (!out_paths) return fail("missing policy output");
    out_paths->clear();
    uint64_t size = 0, total = 0;
    std::unique_ptr<void, decltype(&jce_fs_buffer_free)> bytes(
        jce_fs_host_read_capped(manifest_path.c_str(), JCE_DIST_POLICY_MANIFEST_CAP + 1u,
                                &size, &total), jce_fs_buffer_free);
    if (!bytes) return fail("cannot read project manifest");
    if (size > JCE_DIST_POLICY_MANIFEST_CAP || total != size)
        return fail("project manifest exceeds 1 MiB or is truncated");
    const auto *text = static_cast<const char *>(bytes.get());
    // The public string facade is NUL-terminated. Reject hidden suffixes before
    // decoding, including encoded NUL, rather than accepting a shortened path.
    if (std::memchr(text, '\0', (size_t)size))
        return fail("project manifest contains a NUL string");
    const std::string_view raw(text, (size_t)size);
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != '\\') continue;
        if (raw.substr(i, 6) == "\\u0000")
            return fail("project manifest contains an encoded NUL string");
        if (i + 1 < raw.size()) ++i; // Skip an escaped backslash or quote.
    }
    std::unique_ptr<JceJson, decltype(&jce_json_free)> root(
        jce_json_parse_strict(text, (size_t)size), jce_json_free);
    bytes.reset();
    if (!root || !jce_json_is_object(root.get()))
        return fail("project manifest is not a valid JSON object");
    const JceJson *policy = nullptr;
    for (auto *node = jce_json_first_child(root.get()); node;
         node = jce_json_next_sibling(node)) {
        const char *key = jce_json_member_key(node);
        if (key && std::strcmp(key, "confidential_asset_paths") == 0) {
            if (policy) return fail("duplicate policy key");
            policy = node;
        }
    }
    if (!policy) return true;
    if (!jce_json_is_array(policy)) return fail("expected an array of canonical addresses");
    const int count = jce_json_array_size(policy);
    if (count < 0 || count > JCE_DIST_CONFIDENTIAL_COUNT_CAP)
        return fail("too many confidential addresses (maximum 256)");
    try {
        size_t path_bytes = 0;
        for (int i = 0; i < count; ++i) {
            const auto *node = jce_json_array_at(policy, i);
            if (!jce_json_is_string(node)) return fail("address is not a string");
            const char *path = jce_json_string_value(node, "");
            const size_t length = std::strlen(path);
            if (!length || length > JCE_DIST_CONFIDENTIAL_PATH_CAP ||
                length + 1u > JCE_DIST_CONFIDENTIAL_BYTES_CAP - path_bytes)
                return fail("empty address or path byte budget exceeded");
            char canonical[JCE_DIST_CONFIDENTIAL_PATH_CAP + 1];
            const size_t normalized = jce_archive_normalize_path(path, canonical, sizeof(canonical));
            if (normalized != length || std::strcmp(path, canonical) ||
                std::strpbrk(path, "*?[]") || std::strchr(path, ':') || path[0] == '/' ||
                std::strstr(path, "//"))
                return fail("address must be canonical, relative and without wildcards or escape");
            path_bytes += length + 1u;
            out_paths->emplace_back(path);
        }
        std::sort(out_paths->begin(), out_paths->end());
        if (std::adjacent_find(out_paths->begin(), out_paths->end()) != out_paths->end())
            return fail("duplicate confidential address");
    } catch (const std::bad_alloc &) {
        return fail("policy allocation failed");
    }
    return true;
}
