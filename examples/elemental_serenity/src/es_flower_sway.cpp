/*
 * es_flower_sway.cpp — the meadow's flower sway, as a C++ script class.
 *
 * WHAT THE SCENE WAS MISSING.  gen_scene.py scatters 102 flower cards
 * (flower_0 .. flower_101), authored standing upright (rotX 90) on the basin
 * floor.  The Lua director touches exactly one field on them —
 * MeshRenderer.visible, per season, in apply_state_discrete — and nothing has
 * ever touched their TRANSFORM.  The wind ribbons blow, the grass has its own
 * GPU sway in the shader, and the flowers stand perfectly still in the middle
 * of it.  This script gives them the sway, and it deliberately writes only
 * rotation: `visible` stays the director's, so two scripts own two fields of
 * two different components on the same entity and never collide.
 *
 * ── WHY C++ AND NOT PYTHON OR LUA ────────────────────────────────────────
 *
 * This is the one script in the project with an honest performance argument.
 * 102 entities x one set_rotation per frame is ~6100 calls/second at 60 Hz,
 * every one of them crossing the script boundary.  In Lua or Python that is
 * 6100 interpreter dispatches and 6100 marshalled argument tuples; here the
 * loop is compiled and each call is a direct call through jce::script::Api
 * into the C ABI.  Same work, no interpreter in the middle.  That is the
 * shape the cpp backend exists for, and picking it for a 12-entity ambience
 * loop instead would have been decoration.
 *
 * ── A C++ SCRIPT IS A PLUGIN THE PROJECT BUILDS ───────────────
 *
 * 1. THE PATH NAMES A CLASS.  jce_script_instantiate()'s `path` is resolved
 *    by cpp_instantiate through a registry of named classes that MODULES
 *    fill (jce_script_vm_cpp.h); it is never opened as a file.  Three
 *    candidates are tried in order — the whole string, its basename, and the
 *    basename with its extension removed — so the scene's
 *    "scripts/EsFlowerSway.jcecpp" reaches the class published below as plain
 *    "EsFlowerSway".  es_script_langs.c publishes the owning module before
 *    the first entity is instantiated.
 *
 * 2. THE ENGINE ROUTES IT BY EXTENSION, and ".jcecpp" is the ENGINE's claim,
 *    made by jce_script_vm_cpp_register().  This project needs no claim of
 *    its own.
 *
 *    IT USED TO.  Until 2026-08-16 the cpp backend claimed no extension at
 *    all, so a Script component naming a C++ class resolved to NOTHING and
 *    was refused before any VM was asked (JCE_SCRIPT_LANGUAGE is a
 *    WHOLE-PROCESS override and a four-language scene cannot use it).  The
 *    only way through was for this project to invent ".escpp" and to name its
 *    class "EsFlowerSway.escpp", so an exact strcmp would match the whole
 *    stored string.  It worked and it cost two things: the editor's OFFLINE
 *    catalog cannot know about a claim made at runtime, so a working script
 *    was flagged amber, and editor Play could not run it at all.  Both are
 *    gone.  The old spelling still resolves — the engine tries the whole
 *    string first — so this migration was a choice, not a forced move.
 *
 * 3. THE MODULE IS BUILT TWICE, ON PURPOSE.  This one translation unit is
 *    compiled INTO elemental_serenity.exe (jce_script_vm_cpp_add_module, no
 *    loader involved) and into es_scripts.dll, which the EDITOR loads at
 *    project-open through jce_script_vm_cpp_load_library().  The editor is a
 *    different process and does not link this game, so the DLL is how its
 *    Play mode reaches these classes; jce_project.json's "script_modules"
 *    names it.  JCE_CPP_MODULE_END emits both the in-process accessor and the
 *    exported entry symbol, so one macro serves both builds.
 *
 * ── NO LOG.  AT ALL. ─────────────────────────────────────────────────────
 *
 * Lua has jce.log, Python has jce.log (jce_script/vm.py's _Jce.log) and Java
 * has JceEntityScript.log — all three reach JceScriptHost::log.  A C++ module
 * reaches the engine ONLY through jce::script::Api, the wrapper over
 * scripting/c_abi, and `log` is one of the seven entries that ABI
 * deliberately does not export.  jce_script_vm_cpp.h states it and a test
 * holds it.  So this class has exactly one way to say it is alive: the
 * EsCppProbe transform — x = on_start count, y = on_update count, z = flowers
 * resolved.  stderr is available (this is compiled into the exe) and is used
 * only for a hard failure, because it is not the engine log and nothing
 * collects it.
 */

#include <jce/api.h>
#include <jce/script_api/jce_script_api.hpp>
#include <jce/script_vm/jce_script_cpp.hpp>

#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

using jce::script::Api;
using jce::script::Entity;

/* Sway, in DEGREES — jce_script_api_get_rotation/set_rotation are Euler
 * degrees XYZ (jce_script.h:104-110), not radians.  Amplitudes are small: a
 * flower card is ~0.3 world units tall and the reference footage shows a
 * nod, not a wave. */
constexpr float SWAY_DEG_Z   = 5.5f;
constexpr float SWAY_DEG_Y   = 3.0f;
constexpr float SWAY_RATE    = 1.35f;   /* radians/second                   */
constexpr float SWAY_RATE_Y  = 0.87f;   /* different, so the two never sync */

/* A gust envelope that runs slowly across the basin along +X, so the meadow
 * ripples rather than pulsing as one block.  0.11 rad per world unit puts
 * roughly one full wave across the ~28-unit basin. */
constexpr float GUST_PER_UNIT = 0.11f;

class EsFlowerSway : public jce::script::Script {
public:
    void on_start() override
    {
        Api &a = api();
        ++starts_;

        probe_ = first(a, "EsCppProbe");

        flowers_ = a.find_by_prefix("flower_");
        base_.reserve(flowers_.size());
        gust_.reserve(flowers_.size());
        for (Entity e : flowers_) {
            /* std::optional<std::array<float,3>>: absent when the entity has
             * no transform.  Checked, because a missing base would otherwise
             * sway the flower away from wherever it actually stands. */
            auto r = a.get_rotation(e);
            base_.push_back(r ? *r : std::array<float, 3>{0.0f, 0.0f, 0.0f});

            auto p = a.get_position(e);
            gust_.push_back(p ? (*p)[0] * GUST_PER_UNIT : 0.0f);
        }
        found_ = int(flowers_.size());

        if (probe_)
            a.set_position(probe_, float(starts_), 0.0f, float(found_));
        else
            std::fprintf(stderr,
                         "es_flower_sway (cpp): EsCppProbe is MISSING — this "
                         "script has no other way to report that it ran\n");
    }

    void on_update(float dt) override
    {
        Api &a = api();
        ++frames_;
        t_ += dt;

        const std::size_t n = flowers_.size();
        for (std::size_t i = 0; i < n; ++i) {
            const float ph = gust_[i];
            const std::array<float, 3> &b = base_[i];
            a.set_rotation(flowers_[i],
                           b[0],
                           b[1] + SWAY_DEG_Y * std::sin(t_ * SWAY_RATE_Y + ph),
                           b[2] + SWAY_DEG_Z * std::sin(t_ * SWAY_RATE + ph));
        }

        if (probe_)
            a.set_position(probe_, float(starts_), float(frames_),
                           float(found_));
    }

private:
    static Entity first(Api &a, const char *name)
    {
        auto r = a.find_by_name(name);
        return r.first ? *r.first : Entity(0);
    }

    std::vector<Entity>                 flowers_;
    std::vector<std::array<float, 3>>   base_;
    std::vector<float>                  gust_;
    Entity probe_  = 0;
    int    starts_ = 0;
    int    frames_ = 0;
    int    found_  = 0;
    float  t_      = 0.0f;
};

}  // namespace

/* The class name is what C++ calls the class.  The scene stores
 * "scripts/EsFlowerSway.jcecpp"; cpp_instantiate strips the directory and the
 * extension and lands here.  See the header comment. */
JCE_CPP_SCRIPT_CLASS(EsFlowerSway, "EsFlowerSway")

JCE_CPP_MODULE_BEGIN()
    JCE_CPP_MODULE_CLASS(EsFlowerSway)
JCE_CPP_MODULE_GLOBALS()
JCE_CPP_MODULE_END("elemental_serenity", es_flower_sway_module)

#if !defined(ES_SCRIPT_MODULE_SHARED)
/* Publish the module.  Called once from es_script_langs.c BEFORE the scene is
 * loaded — a module added after the first instantiate is a module the scene
 * has already been refused by.
 *
 * COMPILED OUT OF es_scripts.dll, AND THE LINKER IS THE ONE THAT SAYS WHY.
 * jce_script_vm_cpp_add_module() lives in the REGISTRY (jce_script_vm_cpp),
 * and jce_script_vm_cpp.h is explicit that a module must not link it: a
 * plugin carrying its own copy of a per-process registry would publish its
 * classes into a table the host never reads — a module that loads, reports
 * success, and whose classes no instantiate() can find.  So the shared build
 * links jce_script_cpp_module (headers + the call-down wrapper) and nothing
 * else, and this function does not exist in it.  Building it anyway is not a
 * subtle failure: LNK2019, unresolved jce_script_vm_cpp_add_module, which is
 * exactly how this line came to be written.
 *
 * The DLL needs no equivalent: JCE_CPP_MODULE_END exports
 * jce_cpp_script_module, and jce_script_vm_cpp_load_library() calls it.
 *
 * ONE CALL NOW, NOT TWO.  This also used to claim ".escpp", because the cpp
 * backend claimed nothing and without a claim
 * jce_script_vm_language_for_path() returned NULL and the runtime refused the
 * script before any VM was asked.  jce_script_vm_cpp_register() makes the
 * claim itself now, so the only project-side duty left is publishing the
 * classes.
 *
 * When THIS fails, the failure is: language_for_path resolves "cpp", the VM
 * is created, and cpp_instantiate logs "no cpp script class resolves ...",
 * naming every candidate it tried and the fact that no module is loaded. */
extern "C" bool es_flower_sway_register(void)
{
    if (!jce_script_vm_cpp_add_module(es_flower_sway_module())) {
        std::fprintf(stderr,
                     "es_flower_sway: module registration FAILED — "
                     "instantiate(\"scripts/EsFlowerSway.jcecpp\") will "
                     "return 0\n");
        return false;
    }
    return true;
}
#endif  /* !ES_SCRIPT_MODULE_SHARED */
