/* Real public process children + exact private Test Runner owner.
 * No editor/renderer/application is started. Each child is this fixture. */
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest.h>
#include "core/jce_editor_test_run.h"
#include "jce_test_owned_directory.h"
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_timer.h>
#include <cstdio>
#include <cstring>

namespace {
std::string executable;
constexpr const char *literal = "value with spaces & | > $ and a\"b'c";
bool complete(JceEditorTestRun &run)
{
    const auto begin=jce_time_ticks_ms();
    while(run.process && jce_time_ticks_ms()-begin<5000) {
        jce_editor_test_run_poll(run);
        if(run.process) jce_thread_sleep_ms(1);
    }
    return run.process==nullptr;
}
std::string read(const std::string &path)
{
    uint64_t size=0,total=0;
    void *data=jce_fs_host_read_capped(path.c_str(),1024*1024+512,&size,&total);
    if(!data) return {};
    std::string result(static_cast<const char *>(data),(size_t)size);
    jce_fs_buffer_free(data); return result;
}
bool start(JceEditorTestRun &run,const std::string &path,const char *mode,uint32_t timeout=2000)
{
    const std::string args=std::string("--jce-test-run-child ")+mode;
    return jce_editor_test_run_start(run,executable.c_str(),args.c_str(),nullptr,path.c_str(),timeout);
}
}
TEST_CASE("owned directories isolate overlapping scopes and a real self-child process")
{
    JceTestOwnedDirectory first{"_ut_editor_test_runner_isolation"};
    JceTestOwnedDirectory second{"_ut_editor_test_runner_isolation"};
    const std::string expected_prefix = std::string("_ut_editor_test_runner_isolation_pid") +
        std::to_string(JceTestOwnedDirectory::current_process_id());
    CHECK(first.prefix == expected_prefix); CHECK(second.prefix == first.prefix);
    CHECK(first.root != second.root); /* Same PID keeps the public unique suffix. */
    const char retained[] = "parent owns these bytes";
    REQUIRE(jce_fs_host_write_new(first.path("retained.txt").c_str(), retained, sizeof(retained)-1));
    JceEditorTestRun run;
    REQUIRE(start(run, first.path("child.log"), "owned-directory"));
    const bool completed = complete(run);
    INFO("directory child exit=", run.exit_code, " status=", static_cast<int>(run.status),
        " error=", run.error, " output=", run.output);
    REQUIRE(completed); REQUIRE(run.status == JceEditorTestRunStatus::Passed);
    const auto field = [&run](const char *name) {
        const auto begin = run.output.find(name);
        if (begin == std::string::npos) return std::string{};
        const auto value = begin + std::strlen(name);
        const auto end = run.output.find_first_of("\r\n", value);
        return run.output.substr(value, end == std::string::npos ? end : end-value);
    };
    const auto child_root = field("owned-directory-root=");
    const auto child_prefix = field("owned-directory-prefix=");
    REQUIRE_FALSE(child_root.empty()); REQUIRE_FALSE(child_prefix.empty());
    CHECK(child_prefix != first.prefix); /* Actual separate native process namespace. */
    CHECK(child_root.rfind(first.base + "/" + child_prefix, 0) == 0);
    CHECK(child_root != first.root); CHECK(child_root != second.root);
    CHECK_FALSE(jce_fs_host_exists_dir(child_root.c_str()));
    CHECK(jce_fs_host_exists_dir(first.root.c_str()));
    CHECK(jce_fs_host_exists_dir(second.root.c_str()));
    CHECK(read(first.path("retained.txt")) == retained);
}
TEST_CASE("real child receives lossless JSON argv and complete stdout/stderr evidence")
{
    JceTestOwnedDirectory directory{"_ut_editor_test_runner"};
    JceEditorTestRun run; std::string args,error;
    JceJson *json=jce_json_parse(R"(["--jce-test-run-child","argv","value with spaces & | > $ and a\"b'c"])",0);
    REQUIRE(json!=nullptr);
    REQUIRE(jce_editor_test_run_arguments(json,args,error)); jce_json_free(json);
    const std::string path=directory.path("success.log");
    REQUIRE(jce_editor_test_run_start(run,executable.c_str(),args.c_str(),nullptr,path.c_str(),2000));
    REQUIRE(complete(run));
    CHECK(run.status==JceEditorTestRunStatus::Passed); CHECK(run.exit_code==0);
    CHECK(run.output.find(literal)!=std::string::npos);
    CHECK(run.output.find("child stderr")!=std::string::npos);
    const auto log=read(path);
    CHECK(log.find(literal)!=std::string::npos); CHECK(log.find("PASS exit=0")!=std::string::npos);
}
TEST_CASE("nonzero real child cannot become PASS merely because it finished")
{
    JceTestOwnedDirectory directory{"_ut_editor_test_runner"}; JceEditorTestRun run;
    REQUIRE(start(run,directory.path("failed.log"),"fail")); REQUIRE(complete(run));
    CHECK(run.status==JceEditorTestRunStatus::Failed); CHECK(run.exit_code==17);
    CHECK(read(run.log_path).find("FAIL exit=17")!=std::string::npos);
}
TEST_CASE("missing executable reports spawn failure with an owned terminal log")
{
    JceTestOwnedDirectory directory{"_ut_editor_test_runner"}; JceEditorTestRun run;
    CHECK_FALSE(jce_editor_test_run_start(run,directory.path("missing-exe").c_str(),"",nullptr,
        directory.path("missing.log").c_str(),2000));
    CHECK(run.process==nullptr); CHECK(run.status==JceEditorTestRunStatus::Failed);
    CHECK_FALSE(run.error.empty()); CHECK(read(run.log_path).find("FAIL exit=-1")!=std::string::npos);
}
TEST_CASE("cancel owns the real child until poll observes exit and then closes once")
{
    JceTestOwnedDirectory directory{"_ut_editor_test_runner"}; JceEditorTestRun run;
    REQUIRE(start(run,directory.path("cancel.log"),"sleep"));
    jce_editor_test_run_poll(run); REQUIRE(run.process!=nullptr);
    jce_editor_test_run_cancel(run); CHECK(run.status==JceEditorTestRunStatus::Stopping);
    REQUIRE(complete(run)); CHECK(run.status==JceEditorTestRunStatus::Cancelled);
    jce_editor_test_run_close(run); CHECK(run.status==JceEditorTestRunStatus::Cancelled);
    CHECK(read(run.log_path).find("CANCELLED")!=std::string::npos);
}
TEST_CASE("deadline stops a real running child and has a distinct timeout outcome")
{
    JceTestOwnedDirectory directory{"_ut_editor_test_runner"}; JceEditorTestRun run;
    REQUIRE(start(run,directory.path("timeout.log"),"sleep",40));
    REQUIRE(complete(run)); CHECK(run.status==JceEditorTestRunStatus::TimedOut);
    CHECK(read(run.log_path).find("TIMEOUT")!=std::string::npos);
}
TEST_CASE("late first poll cannot certify on-time completion from a real exited child")
{
    const char *mode="sleep"; int expected_exit=0;
    SUBCASE("zero exit already completed") { mode="sleep"; expected_exit=0; }
    SUBCASE("ordinary child failure already completed") { mode="sleep-fail"; expected_exit=17; }
    JceTestOwnedDirectory directory{"_ut_editor_test_runner"}; JceEditorTestRun run;
    REQUIRE(start(run,directory.path("late-poll.log"),mode,40));
    /* Actual child sleeps 2000ms; no Test Runner poll is made before this.
       This is an observed deadline, not an inferred native exit timestamp. */
    jce_thread_sleep_ms(2500);
    REQUIRE(complete(run)); CHECK(run.exit_code==expected_exit);
    CHECK(run.status==JceEditorTestRunStatus::TimedOut);
    CHECK(run.duration_s>=2.5);
    CHECK(read(run.log_path).find("TIMEOUT")!=std::string::npos);
}
TEST_CASE("bounded output overflow is explicit incomplete evidence and cannot PASS")
{
    JceTestOwnedDirectory directory{"_ut_editor_test_runner"}; JceEditorTestRun run;
    REQUIRE(start(run,directory.path("overflow.log"),"flood")); REQUIRE(complete(run));
    CHECK(run.output.size()==1024*1024); CHECK(run.output_truncated);
    CHECK(run.status==JceEditorTestRunStatus::Failed);
    CHECK(read(run.log_path).find("OUTPUT TRUNCATED")!=std::string::npos);
}
TEST_CASE("log preflight refusal preserves a real previous result and user-owned bytes")
{
    JceTestOwnedDirectory directory{"_ut_editor_test_runner"}; JceEditorTestRun run;
    REQUIRE(start(run,directory.path("prior.log"),"ok")); REQUIRE(complete(run));
    REQUIRE(run.status==JceEditorTestRunStatus::Passed);
    const auto previous_path=run.log_path, previous_output=run.output, previous_log=read(run.log_path);
    const std::string blocking=directory.path("blocked"); const char bytes[]="preserve user file";
    REQUIRE(jce_fs_host_write_all(blocking.c_str(),bytes,sizeof(bytes)-1));
    CHECK_FALSE(start(run,blocking+"/new.log","ok"));
    CHECK(run.process==nullptr); CHECK(run.status==JceEditorTestRunStatus::Passed);
    CHECK(run.log_path==previous_path); CHECK(run.output==previous_output);
    CHECK(read(blocking)==bytes);
    CHECK_FALSE(start(run,previous_path,"ok"));
    CHECK(read(previous_path)==previous_log);
    const std::string existing=directory.path("existing-user.log");
    REQUIRE(jce_fs_host_write_new(existing.c_str(),bytes,sizeof(bytes)-1));
    CHECK_FALSE(jce_fs_host_write_new(existing.c_str(),"overwrite",9));
    CHECK_FALSE(start(run,existing,"ok"));
    CHECK(run.process==nullptr); CHECK(run.status==JceEditorTestRunStatus::Passed);
    CHECK(run.log_path==previous_path); CHECK(run.output==previous_output);
    CHECK(read(existing)==bytes);
    std::string args="retained",error;
    JceJson *bad=jce_json_parse("[17]",0); REQUIRE(bad!=nullptr);
    CHECK_FALSE(jce_editor_test_run_arguments(bad,args,error)); CHECK(args=="retained");
    jce_json_free(bad);
}
TEST_CASE("actual log append failure rejects zero exit and shutdown closes a live child")
{
    JceTestOwnedDirectory directory{"_ut_editor_test_runner"}; JceEditorTestRun run;
    const std::string path=directory.path("append-failure.log");
    REQUIRE(start(run,path,"delayed-output"));
    REQUIRE(jce_fs_host_remove_file(path.c_str())); REQUIRE(jce_fs_host_create_directory(path.c_str()));
    REQUIRE(complete(run)); CHECK(run.log_failed); CHECK(run.status==JceEditorTestRunStatus::Failed);
    REQUIRE(start(run,directory.path("shutdown.log"),"sleep"));
    jce_editor_test_run_close(run); CHECK(run.process==nullptr);
    CHECK(run.status==JceEditorTestRunStatus::Cancelled);
    jce_editor_test_run_close(run); CHECK(run.status==JceEditorTestRunStatus::Cancelled);
}
int main(int argc,char **argv)
{
    if(argc>=3 && std::strcmp(argv[1],"--jce-test-run-child")==0) {
        if(std::strcmp(argv[2],"owned-directory")==0) {
            /* This mode consumes the real assertion-based directory helper
               without running test cases in the child process. */
            doctest::Context child_context;
            child_context.setAsDefaultForAssertsOutOfTestCases();
            child_context.setAssertHandler([](const doctest::AssertData &failure) {
                std::fprintf(stderr,"directory assertion %s:%d: %s\n",
                    failure.m_file,failure.m_line,failure.m_expr);
                std::fflush(stderr); std::exit(31);
            });
            JceTestOwnedDirectory directory{"_ut_editor_test_runner_isolation"};
            const char bytes[]="child owns these bytes";
            if(!jce_fs_host_write_new(directory.path("owned.txt").c_str(),bytes,sizeof(bytes)-1)) return 31;
            std::fprintf(stdout,"owned-directory-root=%s\nowned-directory-prefix=%s\n",
                directory.root.c_str(),directory.prefix.c_str());
            std::fflush(stdout); return 0;
        }
        if(std::strcmp(argv[2],"sleep")==0 || std::strcmp(argv[2],"sleep-fail")==0) {
            jce_thread_sleep_ms(2000); return std::strcmp(argv[2],"sleep-fail")==0?17:0;
        }
        if(std::strcmp(argv[2],"delayed-output")==0) jce_thread_sleep_ms(80);
        if(std::strcmp(argv[2],"flood")==0) {
            char data[4096]; std::memset(data,'x',sizeof(data));
            for(int i=0;i<320;++i) std::fwrite(data,1,sizeof(data),stdout);
            std::fflush(stdout); return 0;
        }
        if(std::strcmp(argv[2],"argv")==0) {
            if(argc!=4 || std::strcmp(argv[3],literal)!=0) return 29;
            std::fputs(argv[3],stdout); std::fputs("\n",stdout);
        }
        std::fputs("child stdout\n",stdout); std::fflush(stdout);
        std::fputs("child stderr\n",stderr); std::fflush(stderr);
        return std::strcmp(argv[2],"fail")==0?17:0;
    }
    executable=argv[0];
    doctest::Context context; context.applyCommandLine(argc,argv);
    return context.run();
}
