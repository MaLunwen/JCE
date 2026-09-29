/* jce_cpp_testplugin.cpp — a script module built as a SHARED OBJECT.
 *
 * The statically linked module — the one declared in
 * test_jce_script_vm_cpp_lifecycle.cpp's own translation unit — proves the
 * lifecycle against Lua.
 * This one proves the thing only a real shared object can: that the engine
 * holds function pointers into an image it does not own, and that unloading
 * that image while an instance lives is refused instead of crashing later.
 *
 * It is deliberately tiny.  Everything about the class ABI is already covered
 * by the static module; what has to be true HERE is that the same source shape
 * builds as a plugin, exports exactly one symbol, and answers the ABI
 * handshake — which is the whole of JCE_CPP_MODULE_END.
 *
 * Its class names must not collide with the static module's: class names are
 * the keys instantiate() resolves and the registry refuses a duplicate, so a
 * shared name would make this plugin's registration fail for a reason that
 * looks like a plugin defect.
 */
#include <jce/script_vm/jce_script_cpp.hpp>

#include <cstdio>

namespace {

class PluginProbe : public jce::script::Script {
public:
    void on_update(float dt) override
    {
        char b[128];
        (void)dt;
        ++n_;
        std::snprintf(b, sizeof(b), "plugin n=%d", n_);
        api().ui_set_text(entity(), b);
    }

private:
    int n_ = 0;
};

}  // namespace

JCE_CPP_SCRIPT_CLASS(PluginProbe, "PluginProbe")

JCE_CPP_MODULE_BEGIN()
    JCE_CPP_MODULE_CLASS(PluginProbe)
JCE_CPP_MODULE_GLOBALS()
JCE_CPP_MODULE_END("jce_cpp_testplugin", jce_cpp_testplugin_module)
