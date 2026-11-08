/* Actual snapshot owner, public profile policy and real filesystem publication.
 * No ImGui, application window, allocator-completeness or hardware-budget claim. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "jce_test_owned_directory.h"
#include "core/jce_editor_config.h"
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_path.h>
#include <cstring>
#include <string>
static unsigned atomic_calls, ordinary_calls;
static bool fail_write;
static std::string wrong_root;
static bool atomic_port(const char *path,const void *data,uint64_t size)
{ ++atomic_calls; return !fail_write && jce_fs_host_write_all_atomic(path,data,size); }
static bool ordinary_port(const char *path,const void *data,uint64_t size)
{ ++ordinary_calls; return !fail_write && jce_fs_host_write_all(path,data,size); }
[[maybe_unused]] static bool old_profile_path(const char *name,char *out,size_t capacity)
{ return jce_path_join(out,capacity,wrong_root.c_str(),name); }
#define jce_fs_host_write_all_atomic atomic_port
#define jce_fs_host_write_all ordinary_port
#ifndef JCE_EDITOR_MEMORY_SNAPSHOT_HEADER
#define JCE_EDITOR_MEMORY_SNAPSHOT_HEADER "panels/jce_panel_memory_snapshot.h"
#endif
#include JCE_EDITOR_MEMORY_SNAPSHOT_HEADER
#undef jce_fs_host_write_all_atomic
#undef jce_fs_host_write_all
static std::string read(const char *path)
{
    uint64_t size=0; void *bytes=jce_fs_host_read_all(path,&size);
    if (!bytes) return {};
    std::string text(static_cast<const char *>(bytes),static_cast<size_t>(size));
    jce_fs_buffer_free(bytes); return text;
}
TEST_CASE("the memory target measures process RSS rather than voluntary tags")
{
    JceEditorMemorySnapshot snapshot{}; snapshot.process_valid=true;
    snapshot.proc.process_rss_bytes=797ull*1024ull*1024ull;
    snapshot.total_cur=0; CHECK(jce_editor_memory_budget_rss(snapshot)==snapshot.proc.process_rss_bytes);
    CHECK(jce_editor_memory_budget_rss(snapshot)>JCE_EDITOR_MEMORY_TARGET_BYTES);
    snapshot.total_cur=9ull*JCE_EDITOR_MEMORY_TARGET_BYTES;
    snapshot.process_valid=false; CHECK(jce_editor_memory_budget_rss(snapshot)==0);
}
TEST_CASE("unavailable allocator data is JSON null with explicit scope")
{
    JceEditorMemorySnapshot snapshot{}; snapshot.process_valid=true;
    snapshot.proc.process_rss_bytes=797ull*1024ull*1024ull;
    snapshot.proc.process_committed_bytes=546ull*1024ull*1024ull;
    char output[4096]; size_t length=0;
    REQUIRE(jce_editor_memory_snapshot_encode(snapshot,output,sizeof output,length));
    JceJson *root=jce_json_parse_strict(output,length); REQUIRE(root);
    JceJson *process=jce_json_get(root,"process");
    CHECK(jce_json_get_bool(process,"rss_available",false));
    CHECK_FALSE(jce_json_get_bool(process,"mimalloc_reserved_available",true));
    CHECK(jce_json_is_null(jce_json_get(process,"mimalloc_reserved_bytes")));
    CHECK(jce_json_is_null(jce_json_get(process,"mimalloc_committed_bytes")));
    JceJson *tagged=jce_json_get(root,"tagged");
    CHECK(std::strcmp(jce_json_get_string(tagged,"scope",""),"voluntary-tagged")==0);
    CHECK_FALSE(jce_json_get_bool(tagged,"complete_process_coverage",true));
    CHECK(jce_json_get_bool(root,"complete",false)); jce_json_free(root);
}
TEST_CASE("complete snapshot preserves tag escaping and exact unsigned counters")
{
    JceEditorMemorySnapshot snapshot{}; snapshot.row_count=JCE_MEM_TAG_COUNT_MAX;
    snapshot.total_cur=UINT64_MAX;
    const char name[]="quoted\"tag\\with\ncontrols";
    for (auto &row:snapshot.rows) { row.name=name; row.s.current_bytes=UINT64_MAX; }
    char output[32768]; size_t length=0;
    REQUIRE(jce_editor_memory_snapshot_encode(snapshot,output,sizeof output,length));
    CHECK(std::strstr(output,"18446744073709551615")!=nullptr);
    JceJson *root=jce_json_parse_strict(output,length); REQUIRE(root);
    JceJson *tags=jce_json_get(root,"tags"); CHECK(jce_json_array_size(tags)==JCE_MEM_TAG_COUNT_MAX);
    CHECK(std::strcmp(jce_json_get_string(jce_json_array_at(tags,63),"name",""),name)==0);
    jce_json_free(root);
}
TEST_CASE("snapshot publication follows the actual profile and preserves CWD")
{
    JceTestOwnedDirectory owned{"_ut_memory_snapshot"};
    const std::string profile=owned.path("profile"); wrong_root=owned.path("outside-profile");
    JceTestEnvironment isolated{"JCE_EDITOR_PROFILE_DIR",profile.c_str()};
    char before[1024],after[1024],published[1024];
    REQUIRE(jce_fs_host_get_current_dir(before,sizeof before));
    JceEditorMemorySnapshot snapshot{};
    snapshot.process_valid=jce_mem_profile_get_process_stats(&snapshot.proc);
    atomic_calls=ordinary_calls=0; fail_write=false;
    REQUIRE(jce_editor_memory_snapshot_publish(snapshot,published,sizeof published)==JceEditorMemorySnapshotResult::Written);
    CHECK(std::string(published).rfind(profile+"/reports/memory/",0)==0);
    CHECK(atomic_calls==1); CHECK(ordinary_calls==0);
    const auto text=read(published); REQUIRE_FALSE(text.empty());
    JceJson *root=jce_json_parse_strict(text.data(),text.size()); REQUIRE(root);
    CHECK(jce_json_get_bool(root,"complete",false));
    CHECK(jce_json_get_number(jce_json_get(root,"process"),"rss_bytes",-1)==
        (snapshot.proc.process_rss_bytes?static_cast<double>(snapshot.proc.process_rss_bytes):-1));
    jce_json_free(root);
    REQUIRE(jce_fs_host_get_current_dir(after,sizeof after)); CHECK(std::strcmp(before,after)==0);
}
TEST_CASE("invalid explicit profile cannot fall back to CWD")
{
    JceTestOwnedDirectory owned{"_ut_memory_snapshot_profile"}; wrong_root=owned.path("outside-profile");
    JceTestEnvironment isolated{"JCE_EDITOR_PROFILE_DIR","relative/profile"};
    JceEditorMemorySnapshot snapshot{}; char published[1024];
    atomic_calls=ordinary_calls=0;
    CHECK(jce_editor_memory_snapshot_publish(snapshot,published,sizeof published)==JceEditorMemorySnapshotResult::ProfileUnavailable);
    CHECK(atomic_calls==0); CHECK(ordinary_calls==0);
}
TEST_CASE("encoding overflow cannot publish a truncated successful snapshot")
{
    JceTestOwnedDirectory owned{"_ut_memory_snapshot_bounds"}; wrong_root=owned.path("outside-profile");
    JceTestEnvironment isolated{"JCE_EDITOR_PROFILE_DIR",owned.root.c_str()};
    const std::string name(40000,'x'); JceEditorMemorySnapshot snapshot{};
    snapshot.row_count=1; snapshot.rows[0].name=name.c_str();
    char published[1024]; atomic_calls=ordinary_calls=0; fail_write=false;
    CHECK(jce_editor_memory_snapshot_publish(snapshot,published,sizeof published)==JceEditorMemorySnapshotResult::Incomplete);
    CHECK(atomic_calls==0); CHECK(ordinary_calls==0);
    CHECK_FALSE(jce_fs_host_exists_file(published));
}
TEST_CASE("failed publication retains the previous complete observation")
{
    JceTestOwnedDirectory owned{"_ut_memory_snapshot_failure"}; wrong_root=owned.path("outside-profile");
    JceTestEnvironment isolated{"JCE_EDITOR_PROFILE_DIR",owned.root.c_str()};
    JceEditorMemorySnapshot snapshot{}; char published[1024],failed[1024];
    fail_write=false;
    REQUIRE(jce_editor_memory_snapshot_publish(snapshot,published,sizeof published)==JceEditorMemorySnapshotResult::Written);
    const auto previous=read(published); REQUIRE_FALSE(previous.empty());
    fail_write=true;
    CHECK(jce_editor_memory_snapshot_publish(snapshot,failed,sizeof failed)==JceEditorMemorySnapshotResult::WriteFailed);
    fail_write=false;
    CHECK(std::strcmp(published,failed)!=0); CHECK(read(published)==previous);
    CHECK_FALSE(jce_fs_host_exists_file(failed));
}
