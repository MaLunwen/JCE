/* Actual editor grid + engine localization + host JSON/files/logger. No GUI. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <initializer_list>
#include "core/jce_editor_game_l10n.h"
#include "jce_test_owned_directory.h"
#include <jce/middleware/ui/jce_localization.h>
#include <jce/os/core/jce_log.h>
#include <cstring>
#include <string>
namespace {
unsigned warnings;
void sink(const JceLogRecord *record, void *)
{ if (record->level >= JCE_LOG_LEVEL_WARN) ++warnings; }
struct Session {
    JceTestOwnedDirectory dir{"_ut_editor_game_l10n"};
    Session() { warnings=0; jce_log_set_level(JCE_LOG_LEVEL_INFO); jce_log_set_sink(sink,nullptr); }
    ~Session() { jce_editor_gl10n_unload(); jce_loc_shutdown(); jce_log_set_sink(nullptr,nullptr); }
    void catalog(const char *project, const char *name, const char *text)
    {
        const std::string base=dir.path(project)+"/assets/i18n";
        REQUIRE(jce_fs_host_create_directory(base.c_str()));
        REQUIRE(jce_fs_host_write_all((base+"/"+name).c_str(),text,std::strlen(text)));
    }
};
}
TEST_CASE("empty optional project catalog detaches previous project without missing-en warning")
{
    Session s; s.catalog("A","en.json","{\"greeting\":\"project A\"}");
    REQUIRE(jce_editor_gl10n_load(s.dir.path("A").c_str(),"assets"));
    CHECK(std::strcmp(jce_loc_t("greeting"),"project A")==0);
    warnings=0;
    CHECK_FALSE(jce_editor_gl10n_load(s.dir.path("B").c_str(),"assets"));
    CHECK(jce_editor_gl10n_loaded());
    CHECK(jce_editor_gl10n_locale_count()==0);
    CHECK(std::strcmp(jce_loc_t("greeting"),"greeting")==0);
    CHECK(warnings==0);
}
TEST_CASE("first catalog save after detach attaches actual new locale bytes")
{
    Session s;
    CHECK_FALSE(jce_editor_gl10n_load(s.dir.path("B").c_str(),"assets"));
    REQUIRE(jce_editor_gl10n_add_locale("en"));
    jce_editor_gl10n_set("en","greeting","new project");
    REQUIRE(jce_editor_gl10n_save_all());
    CHECK(std::strcmp(jce_loc_t("greeting"),"new project")==0);
    CHECK_FALSE(jce_editor_gl10n_dirty());
    CHECK(warnings==0);
}
TEST_CASE("malformed present catalogs remain diagnostic and unadvertised")
{
    Session s;
    for (const char *bad : {"{", "[]", "{\"greeting\":42}"}) {
        s.catalog("B","en.json",bad); warnings=0;
        CHECK_FALSE(jce_editor_gl10n_load(s.dir.path("B").c_str(),"assets"));
        CHECK(jce_editor_gl10n_locale_count()==0);
        CHECK(warnings>=1);
        CHECK(std::strcmp(jce_loc_t("greeting"),"greeting")==0);
    }
}
TEST_CASE("explicit unavailable locale in a configured catalog still reports failure")
{
    Session s; s.catalog("A","en.json","{\"greeting\":\"valid\"}");
    REQUIRE(jce_editor_gl10n_load(s.dir.path("A").c_str(),"assets"));
    warnings=0; jce_loc_set_locale("missing_requested_locale");
    CHECK(warnings>=1);
}
