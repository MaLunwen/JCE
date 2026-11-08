/* Real Windows image ownership, using the complete actual Editor adapter.
 * Never overwrite a mapped ELF/Mach-O file: this target is Windows-only. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "core/jce_editor_script_backends.h"

#include <jce/middleware/script/jce_script.h>
#include <jce/middleware/script/jce_script_vm.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/script_vm/jce_script_vm_cpp.h>

#include <cstdlib>
#include <string>
#include <vector>

namespace {
constexpr const char *kClass = "EditorNativeImageProbe";
constexpr const char *kEnv = "JCE_SCRIPT_CPP_MODULES";

struct ModuleFixture {
    std::string directory;
    std::string source;
    std::string prior_env;
    bool had_env = false;
    bool owns_directory = false;
    JceScript *vm = nullptr;
    JceScriptInstance instance = 0;
    std::vector<std::string> events;

    ModuleFixture()
    {
        const char *prior = std::getenv(kEnv);
        had_env = prior != nullptr;
        if (prior) prior_env = prior;
    }

    ~ModuleFixture()
    {
        if (vm) jce_script_destroy(vm); // also releases an instance after REQUIRE
        jce_editor_script_modules_reload(nullptr);
        (void)_putenv_s(kEnv, had_env ? prior_env.c_str() : "");
        if (owns_directory)
            CHECK_MESSAGE(jce_fs_host_remove_recursive(directory.c_str()),
                          "owned fixture files remained mapped after project close: "
                              << directory);
    }

    bool prepare()
    {
        char unique[2048] = {};
        if (!jce_fs_host_create_directory(JCE_TEST_NATIVE_IMAGE_SCRATCH) ||
            !jce_fs_host_make_unique_path(JCE_TEST_NATIVE_IMAGE_SCRATCH "/project",
                                         unique, sizeof unique))
            return false;
        directory = unique;
        if (!jce_fs_host_create_directory(directory.c_str())) return false;
        owns_directory = true;
        source = directory + "/author_module.dll";
        const std::string dependency = directory + "/" JCE_TEST_NATIVE_IMAGE_DEPENDENCY_NAME;
        const std::string script_api = directory + "/" JCE_TEST_NATIVE_IMAGE_SCRIPT_API_NAME;
        if (!jce_fs_host_copy_file(JCE_TEST_NATIVE_IMAGE_V1, source.c_str()) ||
            !jce_fs_host_copy_file(JCE_TEST_NATIVE_IMAGE_DEPENDENCY, dependency.c_str()) ||
            !jce_fs_host_copy_file(JCE_TEST_NATIVE_IMAGE_SCRIPT_API, script_api.c_str()))
            return false;
        return _putenv_s(kEnv, source.c_str()) == 0;
    }

    static void text(void *user, JceScriptEntity, const char *value)
    {
        static_cast<ModuleFixture *>(user)->events.emplace_back(value ? value : "");
    }

    bool open_vm()
    {
        JceScriptHost host = {};
        host.user = this;
        host.ui_set_text = text;
        vm = jce_script_vm_create("cpp", &host, sizeof host);
        return vm != nullptr;
    }

    bool instantiate()
    {
        instance = jce_script_instantiate(vm, kClass, 42);
        return instance != 0;
    }

    void release()
    {
        if (instance) jce_script_release(vm, instance);
        instance = 0;
    }

    std::string observe()
    {
        const size_t before = events.size();
        jce_script_call_update(vm, instance, 0.25f);
        return events.size() == before + 1 ? events.back() : "no module callback";
    }

    std::vector<std::string> images() const
    {
        std::vector<std::string> result;
        CHECK(jce_fs_host_list_dir(directory.c_str(),
            [](const char *name, bool is_dir, void *user) {
                if (!is_dir && std::string(name).find("author_module.dll.jce-image-") == 0)
                    static_cast<std::vector<std::string> *>(user)->emplace_back(name);
                return true;
            }, &result));
        for (auto &name : result) name = directory + "/" + name;
        return result;
    }
};

ModuleFixture *shutdown_fixture = nullptr;
unsigned state_shutdown_calls = 0, i18n_shutdown_calls = 0;
void native_image_state_shutdown()
{
    ++state_shutdown_calls;
    REQUIRE(shutdown_fixture != nullptr);
    CHECK(jce_script_vm_cpp_has_class(kClass));
    jce_script_destroy(shutdown_fixture->vm);
    shutdown_fixture->vm = nullptr;
    shutdown_fixture->instance = 0;
    CHECK(jce_script_vm_cpp_has_class(kClass)); // registry still owns the image
}
void native_image_i18n_shutdown()
{
    ++i18n_shutdown_calls;
    CHECK(state_shutdown_calls == 1);
    CHECK_FALSE(jce_script_vm_cpp_has_class(kClass));
    CHECK(shutdown_fixture->images().empty());
}
#define jce_editor_state_shutdown native_image_state_shutdown
#define jce_editor_i18n_shutdown native_image_i18n_shutdown
void actual_editor_module_shutdown()
{
#include JCE_TEST_NATIVE_IMAGE_SHUTDOWN
}
#undef jce_editor_state_shutdown
#undef jce_editor_i18n_shutdown
}  // namespace

TEST_CASE("an authoring native module leaves its build output replaceable")
{
    REQUIRE(jce_script_vm_cpp_register());
    ModuleFixture fixture;
    REQUIRE(fixture.prepare());
    jce_editor_script_modules_reload(fixture.directory.c_str());
    REQUIRE_MESSAGE(jce_script_vm_cpp_has_class(kClass),
                    "actual Editor loader failed to register the staged module and sibling DLL");
    REQUIRE(fixture.open_vm());
    REQUIRE(fixture.instantiate());
    CHECK(fixture.observe() == "v1:73:1");
    const auto initial_images = fixture.images();
    CHECK(initial_images.size() == 1);

    // This ordinary false return is the old owner's Windows LNK1104 witness.
    const bool replaced = jce_fs_host_copy_file(JCE_TEST_NATIVE_IMAGE_V2,
                                                fixture.source.c_str());
    CHECK_MESSAGE(replaced,
                  "the Editor mapped the build output; rebuilding cannot replace the original DLL");
    if (!replaced) return; // teardown releases the old image before removing files

    CHECK(fixture.observe() == "v1:73:2");
    CHECK_FALSE(jce_editor_script_modules_reload_native(fixture.directory.c_str()));
    CHECK(fixture.observe() == "v1:73:3");
    CHECK(fixture.images() == initial_images);

    // An ordinary project reload retains the original-path identity and image.
    jce_editor_script_modules_reload(fixture.directory.c_str());
    CHECK(fixture.observe() == "v1:73:4");
    fixture.release();
    jce_editor_script_modules_reload(fixture.directory.c_str()); // no live-instance refusal fallback
    CHECK(fixture.images() == initial_images);
    REQUIRE(fixture.instantiate());
    CHECK(fixture.observe() == "v1:73:1");
    fixture.release();

    REQUIRE(jce_editor_script_modules_reload_native(fixture.directory.c_str()));
    const auto rebuilt_images = fixture.images();
    CHECK(rebuilt_images.size() == 1);
    CHECK(rebuilt_images != initial_images);
    for (const auto &path : initial_images) CHECK_FALSE(jce_fs_host_exists_file(path.c_str()));
    REQUIRE(fixture.instantiate());
    CHECK(fixture.observe() == "v2:73:1");
    fixture.release();

    jce_editor_script_modules_reload(nullptr);
    CHECK(fixture.images().empty());
    CHECK_FALSE(jce_script_vm_cpp_has_class(kClass));
    CHECK(jce_script_instantiate(fixture.vm, kClass, 42) == 0);
}

TEST_CASE("actual Editor shutdown releases instances before its native image lease")
{
    REQUIRE(jce_script_vm_cpp_register());
    ModuleFixture fixture;
    REQUIRE(fixture.prepare());
    jce_editor_script_modules_reload(fixture.directory.c_str());
    REQUIRE(fixture.open_vm());
    REQUIRE(fixture.instantiate());
    CHECK(fixture.observe() == "v1:73:1");
    CHECK(fixture.images().size() == 1);
    state_shutdown_calls = i18n_shutdown_calls = 0;
    shutdown_fixture = &fixture;
    actual_editor_module_shutdown();
    shutdown_fixture = nullptr;
    CHECK(state_shutdown_calls == 1);
    CHECK(i18n_shutdown_calls == 1);
    CHECK(fixture.vm == nullptr);
    CHECK(fixture.images().empty());
}
