/* jce_dist_audit.h  Post-link verification for one-file dist packages. */
#ifndef JCE_DIST_AUDIT_H
#define JCE_DIST_AUDIT_H

#include <cstddef>
#include <cstdint>

struct JceDistGraphAsset {
    const char *address = nullptr;
    uint64_t content_id = 0;
};

struct JceDistAuditInput {
    const char *executable_path = nullptr;
    const char *pak_path = nullptr;
    const char *report_path = nullptr;
    const char *package_dir = nullptr;
    const char *expected_exe_name = nullptr;
    const char *graph_snapshot_path = nullptr;
    const char *const *protected_paths = nullptr;
    size_t protected_path_count = 0;
    const uint8_t *key = nullptr; /* 32 bytes when require_secure */
    const JceDistGraphAsset *graph_assets = nullptr;
    size_t graph_asset_count = 0;
    bool require_secure = true;
    bool require_gui = false;
    bool require_single_file = false;
    bool require_graph_parity = false;
};

struct JceDistAuditResult {
    bool passed = false;
    bool pe_valid = false;
    uint16_t pe_subsystem = 0;
    bool pak_embedded_exactly_once = false;
    size_t pak_offset = SIZE_MAX;
    uint32_t archive_entries = 0;
    uint32_t encrypted_entries = 0;
    uint32_t authenticated_entries = 0;
    uint32_t verified_entries = 0;
    uint32_t graph_expected = 0;
    uint32_t graph_verified = 0;
    uint32_t graph_mismatch_count = 0;
    uint32_t leaked_path_count = 0;
    uint32_t package_file_count = 0;
    char error[512] = {0};
};

bool jce_dist_audit_pe_subsystem(const uint8_t *data, size_t size,
                                 uint16_t *out_subsystem);

/* Find `needle` outside [excluded_offset, excluded_offset+excluded_size).
 * Returns SIZE_MAX when absent. */
size_t jce_dist_audit_find_outside_range(
    const uint8_t *data, size_t size, const uint8_t *needle,
    size_t needle_size, size_t excluded_offset, size_t excluded_size);

bool jce_dist_audit_run(const JceDistAuditInput &input,
                        JceDistAuditResult *out_result);

#endif /* JCE_DIST_AUDIT_H */
