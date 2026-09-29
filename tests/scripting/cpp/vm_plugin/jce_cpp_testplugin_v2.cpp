/* jce_cpp_testplugin_v2.cpp — the SAME module, REBUILT.
 *
 * Same module name, same class name, DIFFERENT behaviour. That combination is
 * the point: it is what a project's .jcec looks like after somebody edited a
 * C++ gameplay class and rebuilt it, and it is the only way to prove that
 * unload + load_library actually picks the new code up rather than re-using
 * an image the process already has mapped.
 *
 * WHY THAT NEEDS PROVING AT ALL. jce_library.h:36-39 records that on Windows
 * opening a library "attaches to an already-resident module of the same name
 * rather than loading a second copy". A reload that quietly re-attached would
 * succeed, log success, and run yesterday's code -- which is exactly the
 * complaint the editor command this exists for is meant to end, arriving one
 * layer deeper and much harder to see.
 *
 * Kept byte-for-byte parallel to jce_cpp_testplugin.cpp apart from the one
 * string, so a reader can diff the two and see that the ONLY variable is the
 * code inside the image.
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
        /* The one difference. "REBUILT" rather than a changed number, because
         * a number could also come from a stale instance's counter and the
         * assertion would not be able to tell those apart. */
        std::snprintf(b, sizeof(b), "plugin REBUILT n=%d", n_);
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
