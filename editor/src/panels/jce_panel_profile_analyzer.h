/* Private Analyzer CSV producer; complete encoding precedes atomic publication. */
#pragma once

#include "core/jce_editor_alloc.h"
#include "core/jce_editor_config.h"
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_path.h>

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>

constexpr int JCE_EDITOR_PROFILE_ANALYZER_CAPACITY = 256;
constexpr size_t JCE_EDITOR_PROFILE_ANALYZER_CSV_BYTES = 65536;

struct JceEditorProfileAnalyzerRecord {
    float frame_ms = 0, cpu_ms = 0, gpu_ms = 0, wait_ms = 0;
    bool cpu_available = false, gpu_available = false, wait_available = false;
    uint64_t frame_no = 0;
};

enum class JceEditorProfileAnalyzerExportResult {
    Written, Empty, Incomplete, ProfileUnavailable, AllocationFailed, WriteFailed
};

namespace jce_profile_analyzer_detail {

struct Buffer {
    char *bytes;
    size_t capacity, used = 0;

    bool append(const char *text, size_t length)
    {
        if (!bytes || used >= capacity || length >= capacity - used) return false;
        std::memcpy(bytes + used, text, length);
        used += length;
        bytes[used] = '\0';
        return true;
    }

    bool append(const char *text) { return append(text, std::strlen(text)); }

    bool number(uint64_t value)
    {
        char text[32];
        const auto encoded = std::to_chars(text, text + sizeof text, value);
        return encoded.ec == std::errc{} && append(text, encoded.ptr - text);
    }

    bool metric(float value)
    {
        if (!std::isfinite(value) || value < 0) return false;
        char text[64];
        const auto encoded = std::to_chars(text, text + sizeof text, value,
                                           std::chars_format::fixed, 3);
        return encoded.ec == std::errc{} && append(text, encoded.ptr - text);
    }

    bool optional(float value, bool available) { return !available || metric(value); }
};

} // namespace jce_profile_analyzer_detail

inline bool jce_editor_profile_analyzer_encode(const JceEditorProfileAnalyzerRecord *ring,
    int head, int filled, char *output, size_t capacity, size_t &length)
{
    length = 0;
    if (!ring || head < 0 || head >= JCE_EDITOR_PROFILE_ANALYZER_CAPACITY ||
        filled < 1 || filled > JCE_EDITOR_PROFILE_ANALYZER_CAPACITY) return false;
    jce_profile_analyzer_detail::Buffer b{output, capacity};
    if (!b.append("frame,frame_ms,backend_cpu_interval_ms,latched_gpu_ms,api_wait_ms,"
                  "cpu_available,gpu_available,wait_available\n")) return false;
    for (int i = 0; i < filled; ++i) {
        const auto &r = ring[(head - filled + i + JCE_EDITOR_PROFILE_ANALYZER_CAPACITY) %
                             JCE_EDITOR_PROFILE_ANALYZER_CAPACITY];
        if (!b.number(r.frame_no) || !b.append(",") || !b.metric(r.frame_ms) ||
            !b.append(",") || !b.optional(r.cpu_ms, r.cpu_available) ||
            !b.append(",") || !b.optional(r.gpu_ms, r.gpu_available) ||
            !b.append(",") || !b.optional(r.wait_ms, r.wait_available) ||
            !b.append(r.cpu_available ? ",1" : ",0") ||
            !b.append(r.gpu_available ? ",1" : ",0") ||
            !b.append(r.wait_available ? ",1\n" : ",0\n")) return false;
    }
    length = b.used;
    return true;
}

inline JceEditorProfileAnalyzerExportResult jce_editor_profile_analyzer_publish(
    const JceEditorProfileAnalyzerRecord *ring, int head, int filled,
    char *published, size_t published_capacity)
{
    using Result = JceEditorProfileAnalyzerExportResult;
    if (!published || published_capacity == 0) return Result::ProfileUnavailable;
    published[0] = '\0';
    if (filled == 0) return Result::Empty;
    if (!jce_editor_dotjce_path("reports/profiler/profile_export.csv", published,
                               published_capacity) || !jce_path_is_absolute(published)) {
        published[0] = '\0';
        return Result::ProfileUnavailable;
    }
    char *body = static_cast<char *>(ED_MALLOC(JCE_EDITOR_PROFILE_ANALYZER_CSV_BYTES));
    if (!body) return Result::AllocationFailed;
    size_t length = 0;
    if (!jce_editor_profile_analyzer_encode(ring, head, filled, body,
                                           JCE_EDITOR_PROFILE_ANALYZER_CSV_BYTES, length)) {
        ED_FREE(body);
        return Result::Incomplete;
    }
    const bool written = jce_fs_host_write_all_atomic(published, body,
                                                     static_cast<uint64_t>(length));
    ED_FREE(body);
    return written ? Result::Written : Result::WriteFailed;
}
