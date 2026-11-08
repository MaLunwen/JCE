/* Actual scaffold/pipeline/manifest owners with typed editor activation/save ports. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "core/jce_editor_new_project.h"
#include "core/jce_editor_project.h"
#include "core/jce_editor_default_camera.h"
#include "jce_test_owned_directory.h"
#include <jce/middleware/scene/jce_scene_camera.h>
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/renderer/jce_render_pipeline.h>
#include <jce/os/core/jce_alloc.h>
#include <cstring>
#include <vector>
namespace {
struct Run;
Run *active;
struct Run {
    JceTestOwnedDirectory dir{"_ut_editor_new_project"};
    JceTestEnvironment templates{"JCE_TEMPLATES_DIR", JCE_TEST_TEMPLATES_DIR};
    std::string failure, root;
    std::vector<std::string> calls;
    JceScene *scene = nullptr;
    bool selected_2d = false;
    Run() : root(dir.path("New")) { active=this; }
    ~Run() { if(scene) jce_scene_destroy(scene); jce_editor_project_set_root(nullptr); active=nullptr; }
};
bool activate(const char *root)
{
    active->calls.push_back("activate");
    if(active->failure=="activate") return false;
    jce_editor_project_set_root(root);
    return jce_editor_project_get()!=nullptr;
}
bool new_scene(bool is_2d)
{
    active->calls.push_back("scene"); active->selected_2d=is_2d;
    if(active->failure=="scene") return false;
    if(active->scene) jce_scene_destroy(active->scene);
    active->scene=jce_scene_create();
    if(!active->scene) return false;
    const auto e=jce_scene_create_entity(active->scene,"Main Camera");
    JceTransform t{}; t.position=jce_v3(0,5,10); t.rotation=jce_q_identity(); t.scale=jce_v3(1,1,1);
    jce_scene_set_transform(active->scene,e,&t);
    JceCameraComponent c{}; c.fov_deg=60; c.near_plane=.1f; c.far_plane=1000;
    jce_scene_set_camera(active->scene,e,&c);
    return jce_editor_seed_default_camera(active->scene,e,is_2d);
}
bool save_scene(const char *path)
{
    active->calls.push_back("save");
    if(active->failure=="save") return false;
    JceJson *json=jce_scene_save_json(active->scene);
    return json && jce_json_write_file(path,json,true,true);
}
bool update_field(const char *field,const char *value)
{
    active->calls.push_back("manifest");
    CHECK(std::strcmp(field,"startup_scene")==0);
    CHECK(std::strcmp(value,"scenes/Main.scene")==0);
    if(active->failure=="manifest") return false;
    if(active->failure=="manifest-no-write") return true;
    if(active->failure=="wrong-startup")
        return jce_editor_project_update_field(field,"scenes/Wrong.scene");
    const bool saved=jce_editor_project_update_field(field,value);
    if(active->failure=="lost-current-project") jce_editor_project_set_root(nullptr);
    if(active->failure=="lost-disk-manifest")
        CHECK(jce_fs_host_remove_file((active->root+"/jce_project.json").c_str()));
    return saved;
}
const JceEditorNewProjectPorts ports={activate,new_scene,save_scene,update_field};
bool create(Run &run, bool is_2d, char *error)
{
    return jce_editor_new_project_create(run.root.c_str(),"New",
        is_2d?JCE_PROJECT_TEMPLATE_BASIC_2D:JCE_PROJECT_TEMPLATE_BASIC_3D,ports,error,256);
}
void failed_stage(const char *stage,unsigned calls)
{
    Run run; run.failure=stage; char error[256]{};
    CHECK_FALSE(create(run,false,error));
    const char *reason = std::strcmp(stage,"activate")==0 ? "activation" : stage;
    CHECK(std::strstr(error,reason)!=nullptr);
    CHECK(run.calls.size()==calls);
    CHECK(jce_fs_host_exists_dir(run.root.c_str())); /* Explicit partial output is retained. */
    JceProject *project=jce_project_load(run.root.c_str()); REQUIRE(project!=nullptr);
    CHECK((project->startup_scene==nullptr || project->startup_scene[0]=='\0'));
    jce_project_free(project);
}
void successful(bool is_2d)
{
    Run run; char error[256]{};
    /* Distinct old project bytes must survive creation of the new tree. */
    const std::string old=run.dir.path("old-project-note.txt");
    const char note[]="old project remains untouched";
    REQUIRE(jce_fs_host_write_all(old.c_str(),note,sizeof(note)));
    REQUIRE(create(run,is_2d,error)); CHECK(error[0]=='\0');
    const std::vector<std::string> expected_calls={"activate","scene","save","manifest"};
    CHECK(run.calls==expected_calls);
    CHECK(run.selected_2d==is_2d);
    CHECK(jce_fs_host_exists_file(old.c_str()));
    JceProject *project=jce_project_load(run.root.c_str()); REQUIRE(project!=nullptr);
    REQUIRE(project->startup_scene!=nullptr);
    CHECK(std::strcmp(project->startup_scene,"scenes/Main.scene")==0);
    const std::string path=run.root+"/"+project->source_assets+"/"+project->startup_scene;
    jce_project_free(project);
    JceJson *saved=jce_json_parse_file(path.c_str()); REQUIRE(saved!=nullptr);
    JceScene *loaded=jce_scene_create(); REQUIRE(loaded!=nullptr);
    CHECK(jce_scene_load_json(loaded,saved)==1);
    jce_json_free(saved);
    JceSceneCameraPose pose{};
    REQUIRE(jce_scene_camera_resolve_primary(loaded,&pose)==JCE_SCENE_CAMERA_RESOLVE_OK);
    CHECK(pose.orthographic==is_2d);
    jce_scene_destroy(loaded);
    JceRenderPipelineDesc expected, actual;
    jce_render_pipeline_preset_for_current_tier(&expected);
    REQUIRE(jce_render_pipeline_load((run.root+"/Settings/RenderPipeline.rp.json").c_str(),&actual));
    CHECK(actual.shadow_resolution==expected.shadow_resolution);
    CHECK(actual.csm_cascade_count==expected.csm_cascade_count);
    CHECK(actual.hdr_color==expected.hdr_color);
}
}
TEST_CASE("new 3D project saves its actual startup scene and current-tier pipeline") { successful(false); }
TEST_CASE("new 2D project selects orthographic default and reloads its startup scene") { successful(true); }
TEST_CASE("occupied target refuses before modifying user contents")
{
    Run run; char error[256]{};
    REQUIRE(jce_fs_host_create_directory(run.root.c_str()));
    const std::string path=run.root+"/user.txt"; const char bytes[]="preserve";
    REQUIRE(jce_fs_host_write_all(path.c_str(),bytes,sizeof(bytes)));
    CHECK_FALSE(create(run,false,error)); CHECK(run.calls.empty());
    CHECK_FALSE(jce_fs_host_exists_file((run.root+"/jce_project.json").c_str()));
    uint64_t size=0; void *read=jce_fs_host_read_all(path.c_str(),&size); REQUIRE(read!=nullptr);
    CHECK(size==sizeof(bytes)); CHECK(std::memcmp(read,bytes,sizeof(bytes))==0);
    /* Host-reader buffers use the shared default allocator. */
    jce_free(read);
}
TEST_CASE("invalid continuation ports refuse before creating files")
{
    Run run; char error[256]{}; auto missing=ports; missing.save_scene=nullptr;
    CHECK_FALSE(jce_editor_new_project_create(run.root.c_str(),"New",JCE_PROJECT_TEMPLATE_BASIC_3D,missing,error,sizeof(error)));
    CHECK(run.calls.empty()); CHECK_FALSE(jce_fs_host_exists_dir(run.root.c_str()));
}
TEST_CASE("activation failure cannot call scene or publish completion") { failed_stage("activate",1); }
TEST_CASE("default-scene failure cannot call save or manifest") { failed_stage("scene",2); }
TEST_CASE("scene save failure cannot publish a startup-scene manifest") { failed_stage("save",3); }
TEST_CASE("manifest failure remains an explicit incomplete project") { failed_stage("manifest",4); }

TEST_CASE("complete creation rejects a successful writer with missing or mismatched readback")
{
    for(const char *failure:{"manifest-no-write","wrong-startup","lost-current-project","lost-disk-manifest"}) {
        Run run; run.failure=failure; char error[256]{};
        CHECK_FALSE(create(run,false,error));
        CHECK(std::strstr(error,"startup-scene readback")!=nullptr);
        CHECK(run.calls.size()==4);
        CHECK(jce_fs_host_exists_file((run.root+"/assets/scenes/Main.scene").c_str()));
    }
}
