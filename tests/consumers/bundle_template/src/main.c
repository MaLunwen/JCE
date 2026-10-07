/* Public SDK project creation and embedded-bundle consumer preparation. */
#include <jce/api_app.h>
#include <jce/application/jce_main.h>
#include <stdlib.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>
#include <jce/resource/jce_bundle_loader.h>
#include <stdio.h>
#include <string.h>

static bool emit_project(const JceServices *services, void *user)
{
    (void)services;
    (void)user;
    bool result = false;
    const char *input = getenv("JCE_TEMPLATE_INPUT_BUNDLE");
    const char *output = getenv("JCE_TEMPLATE_OUTPUT_PROJECT");
    const char *name = getenv("JCE_TEMPLATE_PROJECT_NAME");
    char error[512] = {0};
    char directory[4096];
    char destination[4096];
    JceFileSystem *fs = NULL;
    JceBundleFile *bundle = NULL;
    JceProject *project = NULL;
    if (!input || !input[0] || !output || !output[0] || !name || !name[0]) {
        LOG_ERROR("bundle_template", "%s", "expected bundle, new project directory and name");
        goto done;
    }
    fs = jce_fs_create();
    if (!fs) goto done;
    bundle = jce_bundle_file_open(fs, input, NULL);
    if (!bundle || !jce_bundle_file_scene_path(bundle) ||
        !jce_bundle_file_scene_path(bundle)[0]) {
        LOG_ERROR("bundle_template", "%s", "valid selected-scene bundle required");
        goto done;
    }
    if (!jce_project_create_from_template(output, name,
            JCE_PROJECT_TEMPLATE_EMPTY, error, sizeof(error))) {
        LOG_ERROR("bundle_template", "project template rejected: %s", error);
        goto done;
    }
    int n = snprintf(directory, sizeof(directory), "%s/bundles", output);
    if (n <= 0 || n >= (int)sizeof(directory) || !jce_fs_host_create_directory(directory))
        goto done;
    n = snprintf(destination, sizeof(destination), "%s/application.jbundle", directory);
    if (n <= 0 || n >= (int)sizeof(destination) ||
        !jce_fs_host_copy_file(input, destination)) goto done;
    project = jce_project_load(output);
    const char *paths[] = {"bundles/application.jbundle"};
    if (!project || !jce_project_set_field(project, "startup_scene", NULL) ||
        !jce_project_bundle_set_all(project, paths, 1) || !jce_project_save(project))
        goto done;
    LOG_INFO("bundle_template", "PUBLIC_SDK_BUNDLE_PROJECT_READY scene=%s bundles=1",
             jce_bundle_file_scene_path(bundle));
    result = true;
done:
    if (project) jce_project_free(project);
    if (bundle) jce_bundle_file_close(bundle);
    if (fs) jce_fs_destroy(fs);
    return result;
}

static bool emit_done(void *user)
{
    (void)user;
    return true;
}

static JceAppDesc emit_desc(void)
{
    JceAppDesc desc = {0};
    desc.name = "Public SDK bundle template emitter";
    desc.init = emit_project;
    desc.should_quit = emit_done;
    desc.headless = true;
    return desc;
}

JCE_MAIN(emit_desc)
