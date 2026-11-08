/* Built twice; the class and module identities stay fixed across a rebuild. */
#include <jce/script_vm/jce_script_cpp.hpp>

#include <cstdio>

extern "C" int jce_editor_native_image_dependency_value(void);

namespace {
class NativeImageProbe : public jce::script::Script {
public:
    void on_update(float) override
    {
        char text[96];
        std::snprintf(text, sizeof text, "v%d:%d:%d",
                      JCE_TEST_NATIVE_IMAGE_VERSION,
                      jce_editor_native_image_dependency_value(), ++count_);
        api().ui_set_text(entity(), text);
    }

private:
    int count_ = 0;
};
}  // namespace

JCE_CPP_SCRIPT_CLASS(NativeImageProbe, "EditorNativeImageProbe")
JCE_CPP_MODULE_BEGIN()
    JCE_CPP_MODULE_CLASS(NativeImageProbe)
JCE_CPP_MODULE_GLOBALS()
JCE_CPP_MODULE_END("editor_native_image_probe", jce_editor_native_image_module)
