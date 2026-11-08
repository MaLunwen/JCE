/* Exact private Play module owner, typed services/scene ports. Actual helper
 * callbacks and teardown, not native ECS, OS window or GPU certification. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#ifndef JCE_TEST_EDITOR_PLAY_MODULE_HEADER
#define JCE_TEST_EDITOR_PLAY_MODULE_HEADER "core/jce_editor_play_module.h"
#endif
#include JCE_TEST_EDITOR_PLAY_MODULE_HEADER
#include <vector>
namespace {
struct Counts { unsigned init=0,exit=0,update=0,draw=0,stock_update=0; bool fail_init=false; int owned=0; std::vector<int> order; };
Counts counts;
JceScene *active_scene = nullptr;
JceEditorPlayModule *stop_in_init = nullptr;
bool init(const JceServices *services,void *) {
    CHECK(services != nullptr); ++counts.init; ++counts.owned; counts.order.push_back(1);
    if (stop_in_init) jce_editor_play_module_end(*stop_in_init);
    return !counts.fail_init;
}
void close(void *) { ++counts.exit; --counts.owned; counts.order.push_back(3); }
void update(float dt,void *) { CHECK(dt >= 0.0f); ++counts.update; counts.order.push_back(2); }
void draw(const JceServices *services,void *) { CHECK(services != nullptr); ++counts.draw; counts.order.push_back(4); }
void stock_update(float,void *) { ++counts.stock_update; }
JceGameModule stock = [] { JceGameModule result={}; result.name="stock"; result.update=stock_update; return result; }();
struct Fixture {
    JceEditorPlayModule state;
    JceServices services={};
    JceGameModule module={};
    /* Opaque identity only: callbacks never dereference a fake scene. */
    JceScene *scene=reinterpret_cast<JceScene *>(uintptr_t(1));
    Fixture() {
        counts={}; active_scene=nullptr; stop_in_init=nullptr;
        module.name="fixture"; module.init=init; module.exit=close; module.update=update; module.draw=draw;
        state.selected=&module;
    }
    ~Fixture() { jce_editor_play_module_end(state); }
};
}
extern "C" {
const JceGameModule *jce_game_module_default(void) { return &stock; }
void jce_game_module_set_active_scene(JceScene *scene) { active_scene=scene; }
}
TEST_CASE("hidden viewport does not defer init or regular module updates") {
    Fixture f; REQUIRE(jce_editor_play_module_begin(f.state,&f.services,f.scene));
    CHECK(counts.init==1); CHECK(counts.draw==0);
    jce_editor_play_module_tick(f.state,f.scene,0.016f);
    jce_editor_play_module_tick(f.state,f.scene,0.016f);
    CHECK(counts.update==2); CHECK(counts.draw==0);
}
TEST_CASE("admitted drawing never steps the module a second time") {
    Fixture f; REQUIRE(jce_editor_play_module_begin(f.state,&f.services,f.scene));
    jce_editor_play_module_tick(f.state,f.scene,0.016f);
    jce_editor_play_module_draw(f.state,f.scene,0.016f);
    jce_editor_play_module_draw(f.state,f.scene,0.016f);
    CHECK(counts.update==1); CHECK(counts.draw==2);
}
TEST_CASE("stock ECS update is skipped because the runtime owns the world step") {
    Fixture f; f.state.selected=nullptr;
    REQUIRE(jce_editor_play_module_begin(f.state,&f.services,f.scene));
    jce_editor_play_module_tick(f.state,f.scene,0.0f);
    jce_editor_play_module_tick(f.state,f.scene,0.016f);
    CHECK(counts.stock_update==0);
}
TEST_CASE("Stop while hidden exits once before borrowed scene services are detached") {
    Fixture f; REQUIRE(jce_editor_play_module_begin(f.state,&f.services,f.scene));
    jce_editor_play_module_end(f.state); CHECK(counts.exit==1); CHECK(counts.owned==0);
    CHECK(active_scene==nullptr); CHECK(f.state.services==&f.services);
    jce_editor_play_module_end(f.state); CHECK(counts.exit==1);
    jce_editor_play_module_tick(f.state,f.scene,0.016f); jce_editor_play_module_draw(f.state,f.scene,0.016f);
    CHECK(counts.update==0); CHECK(counts.draw==0);
}
TEST_CASE("partial failed initialization rolls back its callback ownership exactly once") {
    Fixture f; counts.fail_init=true;
    CHECK_FALSE(jce_editor_play_module_begin(f.state,&f.services,f.scene));
    CHECK(counts.init==1); CHECK(counts.exit==1); CHECK(counts.owned==0);
    CHECK_FALSE(f.state.initialized); CHECK_FALSE(f.state.init_attempted); CHECK(active_scene==nullptr);
    jce_editor_play_module_end(f.state); CHECK(counts.exit==1);
}
TEST_CASE("repeated begin cannot replace the active module or borrow different services") {
    Fixture f; REQUIRE(jce_editor_play_module_begin(f.state,&f.services,f.scene));
    JceServices other={}; REQUIRE(jce_editor_play_module_begin(f.state,&other,f.scene));
    CHECK(counts.init==1); CHECK(f.state.services==&f.services);
    jce_editor_play_module_end(f.state);
    REQUIRE(jce_editor_play_module_begin(f.state,&other,f.scene)); CHECK(counts.init==2);
    jce_editor_play_module_end(f.state); CHECK(counts.exit==2); CHECK(counts.owned==0);
}
TEST_CASE("null scene services and released state cannot produce callbacks") {
    Fixture f;
    CHECK_FALSE(jce_editor_play_module_begin(f.state,nullptr,f.scene));
    CHECK_FALSE(jce_editor_play_module_begin(f.state,&f.services,nullptr));
    CHECK(counts.init==0); CHECK(counts.exit==0);
}
TEST_CASE("scene swap repoints the current borrow instead of retaining an outgoing identity") {
    Fixture f; REQUIRE(jce_editor_play_module_begin(f.state,&f.services,f.scene));
    JceScene *next=reinterpret_cast<JceScene *>(uintptr_t(2));
    jce_editor_play_module_tick(f.state,next,0.016f); CHECK(active_scene==next);
    jce_editor_play_module_draw(f.state,next,0.016f); CHECK(active_scene==next);
}
TEST_CASE("init callback teardown cannot resurrect a released module owner") {
    Fixture f; stop_in_init=&f.state;
    CHECK_FALSE(jce_editor_play_module_begin(f.state,&f.services,f.scene));
    CHECK_FALSE(f.state.initialized); CHECK(counts.init==1); CHECK(counts.exit==1); CHECK(counts.owned==0);
}
TEST_CASE("lifecycle order stays init update optional draw exit with no duplicate release") {
    Fixture f; REQUIRE(jce_editor_play_module_begin(f.state,&f.services,f.scene));
    jce_editor_play_module_tick(f.state,f.scene,0.016f);
    jce_editor_play_module_draw(f.state,f.scene,0.016f);
    jce_editor_play_module_end(f.state);
    REQUIRE(counts.order.size()==4);
    CHECK(counts.order[0]==1); CHECK(counts.order[1]==2); CHECK(counts.order[2]==4); CHECK(counts.order[3]==3);
}
