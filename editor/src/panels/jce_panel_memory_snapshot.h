/* Memory observation and complete snapshot publication; no ImGui or CWD owner. */
#pragma once
#include "core/jce_editor_config.h"
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_mem_profile.h>
#include <jce/os/core/jce_path.h>
#include <jce/os/core/jce_timer.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
constexpr uint64_t JCE_EDITOR_MEMORY_TARGET_BYTES=512ull*1024ull*1024ull;
struct JceEditorMemoryTagRow { int tag=0; const char *name=nullptr; JceMemTagStats s{}; };
struct JceEditorMemorySnapshot {
    JceProcessMemStats proc{};
    bool process_valid=false;
    JceEditorMemoryTagRow rows[JCE_MEM_TAG_COUNT_MAX]{};
    int row_count=0;
    uint64_t total_cur=0, total_peak=0, tag_peak_sum=0;
};
inline uint64_t jce_editor_memory_budget_rss(const JceEditorMemorySnapshot &snapshot)
{
    return snapshot.process_valid?snapshot.proc.process_rss_bytes:0;
}
namespace jce_memory_snapshot_detail {
struct Buffer {
    char *bytes; size_t capacity, used=0;
    bool append(const char *format,...)
    {
        if (!bytes || used>=capacity) return false;
        va_list args; va_start(args,format);
        int n=std::vsnprintf(bytes+used,capacity-used,format,args); va_end(args);
        if (n<0 || static_cast<size_t>(n)>=capacity-used) return false;
        used+=static_cast<size_t>(n); return true;
    }
    bool quoted(const char *name)
    {
        if (!name || !append("\"")) return false;
        for (const unsigned char *p=reinterpret_cast<const unsigned char *>(name);*p;++p) {
            if (*p=='"' || *p=='\\') { if (!append("\\%c",*p)) return false; }
            else if (*p<32) { if (!append("\\u%04x",*p)) return false; }
            else if (!append("%c",*p)) return false;
        }
        return append("\"");
    }
    bool counter(const char *name,uint64_t value,bool available)
    {
        return available?append("\"%s\":%llu",name,static_cast<unsigned long long>(value)):
            append("\"%s\":null",name);
    }
};
}
inline bool jce_editor_memory_snapshot_encode(const JceEditorMemorySnapshot &snapshot,
    char *output,size_t capacity,size_t &length)
{
    length=0;
    if (snapshot.row_count<0 || snapshot.row_count>JCE_MEM_TAG_COUNT_MAX) return false;
    jce_memory_snapshot_detail::Buffer b{output,capacity};
    const bool rss=snapshot.process_valid && snapshot.proc.process_rss_bytes>0;
    const bool commit=snapshot.process_valid && snapshot.proc.process_committed_bytes>0;
    const bool reserve=snapshot.process_valid && snapshot.proc.mimalloc_reserved_bytes>0;
    const bool allocated=snapshot.process_valid && snapshot.proc.mimalloc_committed_bytes>0;
    if (!b.append("{\n\"schema\":\"jce.editor.memory_snapshot.v2\",\"complete\":true,\n"
        "\"process\":{\"scope\":\"current_process\",\"rss_available\":%s,\"committed_available\":%s,"
        "\"mimalloc_reserved_available\":%s,\"mimalloc_committed_available\":%s,",
        rss?"true":"false",commit?"true":"false",reserve?"true":"false",allocated?"true":"false") ||
        !b.counter("rss_bytes",snapshot.proc.process_rss_bytes,rss) || !b.append(",") ||
        !b.counter("committed_bytes",snapshot.proc.process_committed_bytes,commit) || !b.append(",") ||
        !b.counter("mimalloc_reserved_bytes",snapshot.proc.mimalloc_reserved_bytes,reserve) || !b.append(",") ||
        !b.counter("mimalloc_committed_bytes",snapshot.proc.mimalloc_committed_bytes,allocated) ||
        !b.append("},\n\"tagged\":{\"scope\":\"voluntary-tagged\",\"complete_process_coverage\":false,"
        "\"current_bytes\":%llu,\"simultaneous_peak_bytes\":%llu,\"tag_peak_sum_bytes\":%llu},\n"
        "\"budget\":{\"scope\":\"process_rss_only\",\"target_bytes\":%llu,\"rss_available\":%s},\n\"tags\":[\n",
        static_cast<unsigned long long>(snapshot.total_cur),static_cast<unsigned long long>(snapshot.total_peak),
        static_cast<unsigned long long>(snapshot.tag_peak_sum),
        static_cast<unsigned long long>(JCE_EDITOR_MEMORY_TARGET_BYTES),rss?"true":"false")) return false;
    for (int i=0;i<snapshot.row_count;++i) {
        const JceEditorMemoryTagRow &row=snapshot.rows[i];
        if (!b.append("{\"name\":") || !b.quoted(row.name) ||
            !b.append(",\"current\":%llu,\"peak\":%llu,\"live\":%u,\"total_alloc\":%llu,\"total_count\":%u}%s\n",
                static_cast<unsigned long long>(row.s.current_bytes),static_cast<unsigned long long>(row.s.peak_bytes),
                row.s.live_alloc_count,static_cast<unsigned long long>(row.s.total_bytes_allocated),
                row.s.total_alloc_count,i+1<snapshot.row_count?",":"")) return false;
    }
    if (!b.append("]\n}\n")) return false;
    length=b.used; return true;
}
enum class JceEditorMemorySnapshotResult { Written, ProfileUnavailable, Incomplete, WriteFailed };
inline JceEditorMemorySnapshotResult jce_editor_memory_snapshot_publish(
    const JceEditorMemorySnapshot &snapshot,char *published,size_t published_capacity)
{
    if (!published || published_capacity==0) return JceEditorMemorySnapshotResult::ProfileUnavailable;
    published[0]=0;
    char name[160], desired[1024];
    std::snprintf(name,sizeof name,"reports/memory/mem_snapshot_%llu_%llu.json",
        static_cast<unsigned long long>(jce_time_now_epoch_milliseconds()),
        static_cast<unsigned long long>(jce_time_perf_counter()));
    if (!jce_editor_dotjce_path(name,desired,sizeof desired) || !jce_path_is_absolute(desired) ||
        !jce_fs_host_make_unique_path(desired,published,static_cast<uint32_t>(published_capacity)))
        return JceEditorMemorySnapshotResult::ProfileUnavailable;
    char body[32768]; size_t length=0;
    if (!jce_editor_memory_snapshot_encode(snapshot,body,sizeof body,length))
        return JceEditorMemorySnapshotResult::Incomplete;
    if (!jce_fs_host_write_all_atomic(published,body,static_cast<uint64_t>(length)))
        return JceEditorMemorySnapshotResult::WriteFailed;
    return JceEditorMemorySnapshotResult::Written;
}
