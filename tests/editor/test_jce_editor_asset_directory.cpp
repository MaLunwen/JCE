/* Actual scan/cache owner and extracted panel/navigation/lifecycle bodies.
 * Async and host filesystem endpoints are controlled CPU ports; threaded scans
 * run the real work callback on a std::thread. ImGui is real and headless.
 * This does not run the engine executor, SDL filesystem or native Editor. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "jce_test_imgui_context.h"
#include <imgui_internal.h>
#include "panels/jce_panel_assets_caption.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include JCE_TEST_ASSET_DIRECTORY_OWNER
#include JCE_TEST_ASSET_DIRECTORY_STATE

struct JceAsyncExecutor {};
struct JceAsyncTask {
    JceAsyncTaskDesc desc{};
    JceAsyncState state = JCE_ASYNC_STATE_QUEUED;
    unsigned references = 2; // caller plus executor completion queue
    bool cancelled = false, cleaned = false;
    void *result = nullptr;
    JceAsyncResultDestroyFn destroy = nullptr;
};
struct JceAsyncContext { JceAsyncTask *task; };

namespace port {
struct Directory {
    jce_asset_directory::Names names;
    bool success = true;
    int fail_after = -1;
};
std::map<std::string, Directory> directories;
std::vector<JceAsyncTask *> tasks;
std::thread::id owner_thread;
JceAsyncExecutor executor;
bool refuse = false, available = true;
int submissions = 0, rejected = 0, cleanup_calls = 0, result_destroys = 0;
int fs_main = 0, fs_worker = 0, metadata = 0, asset_index = 0, errors = 0;
int peak_pending = 0, locations = 0;
double tree_time = 0;
std::vector<std::string> shutdown_order;

int pending()
{
    return int(std::count_if(tasks.begin(), tasks.end(), [](auto *t) {
        return t->state < JCE_ASYNC_STATE_SUCCEEDED;
    }));
}
JceAsyncTask *first(const char *name = nullptr)
{
    for (auto *task : tasks)
        if (task->state == JCE_ASYNC_STATE_QUEUED &&
            (!name || std::string(task->desc.debug_name) == name)) return task;
    return nullptr;
}
void run(JceAsyncTask *task, bool threaded = true, bool force_cancelled_work = false)
{
    REQUIRE(task != nullptr);
    auto work = [=] {
        task->state = JCE_ASYNC_STATE_RUNNING;
        JceAsyncContext context{task};
        const auto result = task->cancelled && !force_cancelled_work
            ? JCE_ASYNC_RUN_CANCELLED : task->desc.work(&context, task->desc.user_data);
        task->state = task->cancelled || result == JCE_ASYNC_RUN_CANCELLED
            ? JCE_ASYNC_STATE_CANCELLED : result == JCE_ASYNC_RUN_SUCCESS
            ? JCE_ASYNC_STATE_SUCCEEDED : JCE_ASYNC_STATE_FAILED;
    };
    if (threaded) { std::thread worker(work); worker.join(); }
    else work();
}
void completions()
{
    const auto copy = tasks;
    for (auto *task : copy) {
        if (!task->cleaned && jce_async_task_is_terminal(task)) {
            CHECK(task->desc.complete == nullptr);
            if (task->desc.cleanup) task->desc.cleanup(task->desc.user_data);
            task->desc.user_data = nullptr;
            task->cleaned = true;
            ++cleanup_calls;
            jce_async_task_release(task);
        }
    }
}
void finish_all(bool threaded = true)
{
    while (auto *task = first()) run(task, threaded);
    completions();
}
void reset()
{
    jce_asset_directory::owner().shutdown();
    finish_all();
    REQUIRE(tasks.empty());
    owner_thread = std::this_thread::get_id();
    directories.clear();
    refuse = false; available = true;
    submissions = rejected = cleanup_calls = result_destroys = 0;
    fs_main = fs_worker = metadata = asset_index = errors = 0;
    peak_pending = locations = 0; tree_time = 0;
    shutdown_order.clear();
}
}

extern "C" {
void JCE_CALL jce_async_task_desc_init(JceAsyncTaskDesc *desc)
{ *desc = {}; desc->struct_size = sizeof(*desc); }
JceAsyncExecutor *JCE_CALL jce_async_default_executor(void)
{ return port::available ? &port::executor : nullptr; }
JceAsyncTask *JCE_CALL jce_async_submit(JceAsyncExecutor *, const JceAsyncTaskDesc *desc)
{
    ++port::submissions;
    if (port::refuse) { ++port::rejected; return nullptr; }
    auto *task = new JceAsyncTask;
    task->desc = *desc;
    port::tasks.push_back(task);
    port::peak_pending = std::max(port::peak_pending, port::pending());
    return task;
}
void JCE_CALL jce_async_task_release(JceAsyncTask *task)
{
    REQUIRE(task->references > 0);
    if (--task->references) return;
    if (task->result && task->destroy) {
        task->destroy(task->result);
        ++port::result_destroys;
    }
    port::tasks.erase(std::find(port::tasks.begin(), port::tasks.end(), task));
    delete task;
}
bool JCE_CALL jce_async_task_cancel(JceAsyncTask *task)
{ task->cancelled = true; return true; }
bool JCE_CALL jce_async_task_is_terminal(const JceAsyncTask *task)
{ return task->state >= JCE_ASYNC_STATE_SUCCEEDED; }
JceAsyncState JCE_CALL jce_async_task_state(const JceAsyncTask *task)
{ return task->state; }
void *JCE_CALL jce_async_task_result(const JceAsyncTask *task)
{ return task->result; }
bool JCE_CALL jce_async_context_cancel_requested(const JceAsyncContext *ctx)
{ return ctx->task->cancelled; }
bool JCE_CALL jce_async_context_set_result(JceAsyncContext *ctx, void *result,
                                         JceAsyncResultDestroyFn destroy)
{
    if (ctx->task->result) return false;
    ctx->task->result = result; ctx->task->destroy = destroy;
    return true;
}
bool JCE_CALL jce_fs_host_list_dir(const char *dir, JceFsHostListFn callback, void *user)
{
    if (std::this_thread::get_id() == port::owner_thread) ++port::fs_main;
    else ++port::fs_worker;
    const auto found = port::directories.find(dir);
    if (found == port::directories.end()) return false;
    const auto &directory = found->second;
    int count = 0;
    for (const auto &[name, is_dir] : directory.names) {
        if (directory.fail_after >= 0 && count >= directory.fail_after) return false;
        ++count;
        if (!callback(name.c_str(), is_dir, user)) return directory.success;
    }
    return directory.success;
}
bool jce_path_join(char *out, size_t size, const char *base, const char *leaf)
{
    const std::string joined = std::string(base) + "/" + leaf;
    if (joined.size() >= size) return false;
    std::memcpy(out, joined.c_str(), joined.size() + 1); return true;
}
bool jce_path_normalize(char *out, size_t size, const char *path)
{
    if (std::strlen(path) >= size) return false;
    std::memmove(out, path, std::strlen(path) + 1); return true;
}
bool jce_path_basename(char *out, size_t size, const char *path)
{
    const char *base = std::strrchr(path, '/');
    return jce_path_normalize(out, size, base ? base + 1 : path);
}
bool jce_path_extension(char *out, size_t size, const char *path)
{
    const char *ext = std::strrchr(path, '.');
    return jce_path_normalize(out, size, ext ? ext + 1 : "");
}
bool JCE_CALL jce_fs_host_get_size(const char *, uint64_t *out)
{ ++port::metadata; *out = 17; return true; }
}

/* Typed external ports used by the complete actual caller bodies. The
 * filesystem/path functions above retain their actual public declarations. */
AssetBrowserState s_assets{};
std::string normalized_path_string(const std::string &p) { return p; }
std::string format_modified_time(const char *) { ++port::metadata; return "date"; }
bool is_filesystem_root(const char *p) { return std::string(p) == "/"; }
void jce_asset_path_index_rebuild_async(const char *) { ++port::asset_index; }
void jce_editor_pstate_set_str(const char *, const char *) {}
enum { JCE_CONSOLE_INFO, JCE_CONSOLE_ERROR };
void jce_editor_console_log_level(int level, const char *, ...)
{ if (level == JCE_CONSOLE_ERROR) ++port::errors; }
const char *jce_editor_i18n(const char *key) { return key; }
void jce_host_reveal_path(const char *) {}
void jce_host_open_in_text_editor(const char *) {}
void jce_host_open_terminal(const char *) {}
void draw_asset_locations_section() { ++port::locations; }
bool set_asset_browser_simulated_root(const std::string &);
bool restore_asset_browser_project_root() { return false; }
double asset_tree_now() { return port::tree_time; }
bool s_visible[JCE_PANEL_COUNT]{};
struct { int count; } s_console;
struct JceEditorConfig { uint32_t panels_visible_mask, panels_visible_mask_hi; };
constexpr uint32_t JCE_EDITOR_PANELS_MASK_UNSET = UINT32_MAX;
bool jce_editor_config_load(JceEditorConfig *) { return false; }
int jce_editor_config_get_ui_int_or(JceEditorConfig *, const char *, int d) { return d; }
void settings_restore_locale_and_theme() {}
void jce_editor_panel_console_init() {}
void jce_editor_lan_discovery_shutdown() { port::shutdown_order.emplace_back("lan"); }
void jce_editor_panel_bundle_browser_shutdown() { port::shutdown_order.emplace_back("bundle"); }
void jce_editor_panel_test_runner_shutdown() { port::shutdown_order.emplace_back("tests"); }
void jce_hierarchy_storage_shutdown() { port::shutdown_order.emplace_back("hierarchy"); }
void jce_file_viewer_shutdown() { port::shutdown_order.emplace_back("viewer"); }
void jce_editor_panel_console_shutdown() { port::shutdown_order.emplace_back("console"); }
#include JCE_TEST_ASSET_DIRECTORY_WIRING
bool set_asset_browser_simulated_root(const std::string &path)
{ s_assets.project_root = path; reset_asset_browser_view_to_root(path); return true; }
void *asset_directory_peer_owner();

namespace {
struct Fixture {
    JceTestImGuiContext imgui;
    Fixture()
    {
        port::reset(); s_assets = {};
        jce_editor_panels_init();
    }
    ~Fixture()
    {
        jce_asset_directory::owner().shutdown();
        port::finish_all();
        CHECK(port::tasks.empty());
    }
    void frame(double now, bool tree = false)
    {
        imgui.begin();
        ImGui::GetCurrentContext()->Time = now;
        port::tree_time = now;
        ImGui::SetNextWindowSize(ImVec2(700, 500), ImGuiCond_Always);
        ImGui::Begin("asset-directory-control", nullptr, ImGuiWindowFlags_NoSavedSettings);
        asset_directory_frame();
        if (tree) draw_asset_directory_tree(300, 400);
        ImGui::End(); imgui.end();
    }
};
FileEntry entry(std::string name, bool dir = false)
{ FileEntry e{}; e.name = std::move(name); e.is_dir = dir; return e; }
const jce_asset_directory::Listing &tree_frame(jce_asset_directory::Refresh &owner,
                                             const std::string &path, double now)
{
    owner.begin_tree(now);
    const auto &listing = owner.listing(path, now);
    owner.end_tree(now);
    return listing;
}
}

TEST_CASE("directory scan preserves byte names and result after request cleanup")
{
    Fixture fixture;
    CHECK(asset_directory_peer_owner() == &jce_asset_directory::owner());
    port::directories["A"].names = {{"z.glb",false},{"a",true},{"A",true},
        {"\xe6\xa0\x91.glb",false},{"z",true}};
    jce_asset_directory::Flight flight;
    REQUIRE(flight.start("A", 7, false));
    CHECK(port::fs_main == 0); CHECK(port::fs_worker == 0);
    jce_asset_directory::Snapshot result;
    CHECK_FALSE(flight.take(result));
    port::run(port::first());
    CHECK(port::fs_worker == 1);
    port::completions(); // request has already been destroyed; result is live
    CHECK(port::cleanup_calls == 1); CHECK(port::result_destroys == 0);
    REQUIRE(flight.take(result));
    const jce_asset_directory::Names expected = {{"A",true},{"a",true},{"z",true},
        {"z.glb",false},{"\xe6\xa0\x91.glb",false}};
    CHECK(result.names == expected);
    CHECK(result.request.generation == 7);
    CHECK(port::result_destroys == 1);

    REQUIRE(flight.start("A", 8, false));
    port::run(port::first());
    REQUIRE(flight.take(result)); // terminal result before request cleanup
    CHECK(port::cleanup_calls == 1);
    port::completions();
    CHECK(port::cleanup_calls == 2); CHECK(port::result_destroys == 2);
}

TEST_CASE("pre-work cancellation performs no filesystem call and refusal has no fallback")
{
    Fixture fixture;
    port::directories["A"].names = {{"x",false}};
    jce_asset_directory::Flight flight;
    REQUIRE(flight.start("A", 1, false));
    auto *task = port::first();
    flight.cancel();
    port::run(task, true, true); // race: executor entered work after cancellation
    jce_asset_directory::Snapshot result;
    CHECK_FALSE(flight.take(result)); port::completions();
    CHECK(port::fs_main == 0); CHECK(port::fs_worker == 0);
    port::refuse = true;
    CHECK_FALSE(flight.start("A", 2, false));
    CHECK(port::cleanup_calls == 1); // rejected submit does not own request
    CHECK(port::fs_main == 0); CHECK(port::fs_worker == 0);
    CHECK(port::tasks.empty());
    port::refuse = false; port::available = false;
    const int attempts = port::submissions;
    CHECK_FALSE(flight.start("A", 3, false));
    CHECK(port::submissions == attempts); CHECK(port::fs_main == 0);
    port::available = true;
    REQUIRE(flight.start("A", 4, false));
    port::run(port::first()); REQUIRE(flight.take(result)); port::completions();
    CHECK(port::fs_worker == 1);
}

TEST_CASE("tree delayed completion failure partial failure and empty success stay distinct")
{
    Fixture fixture;
    auto &owner = jce_asset_directory::owner();
    port::directories["A"].names = {{"b",true},{"file",false},{"a",true}};
    port::directories["A/a"].names = {{"nested",true}};
    port::directories["A/b"].names = {};
    CHECK(tree_frame(owner, "A", 0).subdirs.empty());
    for (int i = 1; i < 20; ++i) CHECK(tree_frame(owner, "A", i * .01).subdirs.empty());
    CHECK(port::submissions == 1); CHECK(port::fs_main == 0);
    port::finish_all();
    const auto &complete = tree_frame(owner, "A", .2);
    const std::vector<std::string> expected = {"A/a", "A/b"};
    CHECK(complete.subdirs == expected);
    CHECK(complete.has_kids == std::vector<bool>{true, false});
    port::directories["A"].fail_after = 1;
    tree_frame(owner, "A", 1.3); port::finish_all();
    CHECK(tree_frame(owner, "A", 1.4).subdirs == expected);
    port::directories["A"] = {{}, false};
    tree_frame(owner, "A", 2.5); port::finish_all();
    CHECK(tree_frame(owner, "A", 2.6).subdirs == expected);
    port::directories["A"] = {{}, true};
    tree_frame(owner, "A", 3.7); port::finish_all();
    CHECK(tree_frame(owner, "A", 3.8).subdirs.empty());
    CHECK(port::fs_main == 0);
}

TEST_CASE("unknown child probes retain prior expanders and new unknown remains expandable")
{
    Fixture fixture;
    auto &owner = jce_asset_directory::owner();
    port::directories["A"].names = {{"branch",true},{"leaf",true}};
    port::directories["A/branch"].names = {{"next",true}};
    port::directories["A/leaf"].names = {};
    tree_frame(owner,"A",0); port::finish_all(); tree_frame(owner,"A",.1);
    port::directories["A"].names.push_back({"new",true});
    port::directories["A/branch"].success = false;
    port::directories["A/leaf"].success = false;
    tree_frame(owner,"A",1.2); port::finish_all();
    const auto &unknown = tree_frame(owner,"A",1.3);
    CHECK(unknown.has_kids == std::vector<bool>{true, false, true});
    port::directories["A/new"] = {{},true};
    tree_frame(owner,"A",2.4); port::finish_all();
    CHECK(tree_frame(owner,"A",2.5).has_kids == std::vector<bool>{true,false,false});
}

TEST_CASE("current membership sorts exact names and kind and rejects A B A stale generations")
{
    Fixture fixture;
    auto &owner = jce_asset_directory::owner();
    std::vector<FileEntry> entries = {entry("dir",true),entry("a"),entry("z")};
    port::directories["A"].names = {{"z",false},{"dir",true},{"a",false}};
    owner.poll_current("A"); port::finish_all();
    CHECK_FALSE(owner.current_changed("A",entries));
    port::directories["A"].names = {{"dir",false},{"a",false},{"z",false}};
    owner.poll_current("A"); port::finish_all();
    CHECK(owner.current_changed("A",entries)); // same name, different kind
    owner.poll_current("A"); port::finish_all(); // terminal old A result
    owner.invalidate_current(); // A -> B
    owner.invalidate_current(); // B -> A before old completion is consumed
    CHECK_FALSE(owner.current_changed("A",entries));
    owner.poll_current("A"); port::finish_all();
    CHECK_FALSE(owner.current_changed("B",entries)); // independent path check
    port::directories["A"].success = false;
    owner.poll_current("A"); port::finish_all();
    CHECK_FALSE(owner.current_changed("A",entries));
    port::directories["A"] = {{},true};
    owner.poll_current("A"); port::finish_all();
    CHECK(owner.current_changed("A",entries));
}

TEST_CASE("active lifecycle cancellation keeps two flight slots and tree references survive invalidation")
{
    Fixture fixture;
    auto &owner = jce_asset_directory::owner();
    port::directories["A"].names = {{"child",true}};
    port::directories["A/child"] = {{},true};
    tree_frame(owner,"A",0); port::finish_all();
    const auto &listing = tree_frame(owner,"A",.1);
    REQUIRE(listing.subdirs.size() == 1);
    const std::string &borrowed = listing.subdirs.front();
    owner.invalidate();
    CHECK(borrowed == "A/child"); // no vector replacement inside recursive UI
    tree_frame(owner,"A",.2);
    owner.poll_current("A");
    for (int i = 0; i < 30; ++i) {
        if (i % 2) owner.invalidate_current();
        else owner.invalidate(); // cancel both categories, retain both slots
        owner.poll_current(i % 2 ? "A" : "B");
        tree_frame(owner,"A",.21 + i * .01);
    }
    CHECK(port::peak_pending == 2); CHECK(port::pending() == 2);
    port::finish_all();
    CHECK_FALSE(owner.current_changed("B",std::vector<FileEntry>{}));
    owner.invalidate(true);
    CHECK(borrowed == "A/child");
    tree_frame(owner,"B",1.0); // root clear only at safe boundary
    port::directories["B"].names = {{"other",true}};
    port::directories["B/other"] = {{},true};
    port::finish_all();
    CHECK(tree_frame(owner,"B",1.1).subdirs == std::vector<std::string>{"B/other"});
}

TEST_CASE("tree request queue is fair coalesced skips collapsed nodes and backs off refusal")
{
    Fixture fixture;
    auto &owner = jce_asset_directory::owner();
    for (const auto *p : {"A","B","C"}) port::directories[p] = {{},true};
    owner.begin_tree(0);
    owner.listing("A",0); owner.listing("B",0); owner.listing("C",0);
    owner.end_tree(0);
    REQUIRE(port::first());
    CHECK(static_cast<jce_asset_directory::Request *>(port::first()->desc.user_data)->path == "A");
    port::finish_all();
    owner.begin_tree(.1); owner.listing("A",.1); owner.listing("C",.1); owner.end_tree(.1);
    REQUIRE(port::first());
    CHECK(static_cast<jce_asset_directory::Request *>(port::first()->desc.user_data)->path == "C");
    port::finish_all();
    owner.begin_tree(.2); owner.listing("B",.2); owner.end_tree(.2);
    REQUIRE(port::first());
    CHECK(static_cast<jce_asset_directory::Request *>(port::first()->desc.user_data)->path == "B");
    port::finish_all(); owner.begin_tree(.3);
    port::refuse = true;
    tree_frame(owner,"A",1.2);
    const int attempts = port::submissions;
    for (int i = 0; i < 50; ++i) tree_frame(owner,"A",1.21 + i * .01);
    CHECK(port::submissions == attempts); CHECK(port::rejected == 1);
    tree_frame(owner,"A",2.3); CHECK(port::rejected == 2);
    CHECK(port::fs_main == 0);
}

TEST_CASE("actual panel poll tree and refresh wires remove unchanged periodic UI enumeration")
{
    Fixture fixture;
    auto &owner = jce_asset_directory::owner();
    s_assets.project_root = s_assets.current_path = "A";
    s_assets.entries = {entry("child",true),entry("old.glb")};
    port::directories["A"].names = {{"old.glb",false},{"child",true}};
    port::directories["A/child"] = {{},true};
    fixture.frame(0, true);
    CHECK(port::pending() == 2); CHECK(port::fs_main == 0);
    port::finish_all();
    fixture.frame(.1, true);
    CHECK(owner.listing("A",.1).subdirs == std::vector<std::string>{"A/child"});
    for (int i = 2; i <= 35; ++i) { fixture.frame(i * .1,true); port::finish_all(); }
    CHECK(port::submissions >= 9); CHECK(port::fs_worker > 0);
    CHECK(port::fs_main == 0); CHECK(port::metadata == 0); CHECK(port::asset_index == 0);
    CHECK(port::peak_pending == 2); CHECK(port::locations == 36);
    port::directories["A"].names = {{"child",true},{"new.glb",false}};
    for (int i = 36; i <= 42; ++i) { fixture.frame(i * .1,true); port::finish_all(); }
    REQUIRE(s_assets.entries.size() == 2);
    CHECK(s_assets.entries[1].name == "new.glb");
    CHECK(port::asset_index >= 1); CHECK(port::fs_main >= 1); // retained changed-folder materialization
    CHECK(port::metadata > 0);
    printf("DIRECTORY_CPU unchanged_frames=36 periodic_main_fs=0 controlled_worker_fs=%d changed_refresh_main_fs=%d\n",
           port::fs_worker, port::fs_main);
}

TEST_CASE("actual navigation root manual refresh and shared shutdown own cancellation")
{
    Fixture fixture;
    auto &owner = jce_asset_directory::owner();
    s_assets.project_root = s_assets.current_path = "A";
    port::directories["A"].names = {{"old",false}};
    port::directories["B"].names = {{"new",false}};
    s_assets.entries = {entry("old")};
    fixture.frame(0,true);
    REQUIRE(port::tasks.size() == 2);
    navigate_asset_directory("B",true);
    fixture.frame(.1,true);
    CHECK(s_assets.current_path == "B");
    REQUIRE(s_assets.entries.size() == 1);
    CHECK(s_assets.entries[0].name == "new");
    CHECK(port::pending() == 2); // current cancelled but retained until terminal
    request_asset_resource_refresh();
    for (auto *task : port::tasks) CHECK(task->cancelled);
    port::finish_all(); fixture.frame(.2,true);
    s_assets.project_root = "B"; reset_asset_browser_view_to_root("B");
    fixture.frame(.3,true);
    CHECK(owner.listing("B",.3).subdirs.empty());
    CHECK(s_assets.current_path == "B");
    owner.poll_current("B");
    const auto retiring = port::tasks;
    for (auto *task : retiring) task->state = JCE_ASYNC_STATE_RUNNING;
    jce_editor_panels_shutdown();
    for (auto *task : port::tasks) CHECK(task->cancelled);
    const int submitted = port::submissions;
    owner.poll_current("B"); tree_frame(owner,"B",3.0);
    CHECK(port::submissions == submitted);
    const std::vector<std::string> expected = {"lan","bundle","tests","hierarchy","viewer","console"};
    CHECK(port::shutdown_order == expected);
    // Immediate reinit cannot publish old results. Previously cancelled workers
    // can coexist transiently until the shared executor drains their references.
    jce_editor_panels_init();
    s_assets.project_root = s_assets.current_path = "B";
    s_assets.entries = {entry("new")}; s_assets.next_auto_refresh_time = 0;
    fixture.frame(4.0,true); port::finish_all(); fixture.frame(4.1,true);
    CHECK(s_assets.entries[0].name == "new");
    for (auto *task : retiring) port::run(task, true, true);
    port::completions(); fixture.frame(4.2,true);
    CHECK(s_assets.entries[0].name == "new");
    CHECK(port::errors == 0);
}

TEST_CASE("cooperative mode keeps scheduling semantics without off-thread IO claims")
{
    Fixture fixture;
    auto &owner = jce_asset_directory::owner();
    port::directories["A"].names = {{"file",false}};
    owner.poll_current("A");
    CHECK(port::fs_main == 0); // submission stays non-inline
    port::finish_all(false); // cooperative pump deliberately runs on owner thread
    CHECK(port::fs_main == 1); CHECK(port::fs_worker == 0);
    CHECK_FALSE(owner.current_changed("A",std::vector<FileEntry>{entry("file")}));
}
