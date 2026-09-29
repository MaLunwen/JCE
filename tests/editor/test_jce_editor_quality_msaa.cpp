/*
 * test_jce_editor_quality_msaa.cpp — the per-level Anti Aliasing combo has to
 * decide something.
 *
 * Project Settings offers Off / 2x / 4x / 8x per quality level; the value
 * serialises, reloads, and shows in the panel.  The one composition that
 * feeds BOTH the editor viewport and the packager then wrote
 *
 *     out->msaa = ps->graphics.default_msaa;
 *
 * unconditionally, while every other line in the same block took the active
 * level's value.  So a project whose Low level says "AA Off" shipped with the
 * project-wide 4x and nothing an author did to that level could change it.
 *
 * jce_editor_quality_msaa() is that precedence rule, split out so it can be
 * asserted here rather than inferred from a build.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "core/jce_editor_effective_render_settings.h"
#include "core/jce_project_settings.h"

#include <string>

TEST_CASE("a quality level's Anti Aliasing reaches the effective settings")
{
    SUBCASE("the level wins when it states a value")
    {
        /* THE WHOLE DEFECT.  A level that says Off, against a project default
         * of 4x, must ship as Off. */
        CHECK(jce_editor_quality_msaa(0, 4) == 0);
        CHECK(jce_editor_quality_msaa(2, 4) == 2);
        CHECK(jce_editor_quality_msaa(8, 4) == 8);
    }

    SUBCASE("the sentinel inherits the project default")
    {
        /* The other half of keeping both knobs meaningful: a level may decline
         * to state one.  Without this, wiring the level would simply have
         * killed graphics.default_msaa instead. */
        CHECK(jce_editor_quality_msaa(JCE_PS_AA_USE_PROJECT_DEFAULT, 4) == 4);
        CHECK(jce_editor_quality_msaa(JCE_PS_AA_USE_PROJECT_DEFAULT, 0) == 0);
        /* A nonsense project default must not become a nonsense sample count
         * just because a level deferred to it. */
        CHECK(jce_editor_quality_msaa(JCE_PS_AA_USE_PROJECT_DEFAULT, 7) == 0);
    }

    SUBCASE("only real sample counts survive")
    {
        /* project.json is a text file people edit.  1 and 0 both mean "no
         * multisampling" downstream, and 3 must never reach the swapchain. */
        CHECK(jce_editor_quality_msaa(1, 4) == 0);
        CHECK(jce_editor_quality_msaa(3, 4) == 0);
        CHECK(jce_editor_quality_msaa(-7, 4) == 0);
        CHECK(jce_editor_quality_msaa(16, 4) == 16);
    }

    SUBCASE("and the resolver actually asks it")
    {
        /* THE WIRING, not just the rule.  Testing the pure function alone
         * would stay green if the resolver went back to the unconditional
         * line -- which is the defect.  So drive the real composition: install
         * a settings snapshot whose active level asks for 8x against a project
         * default of 2x, and read what the viewport and the packager would
         * both be handed. */
        JceProjectSettings ps;
        jce_project_settings_defaults(&ps);
        ps.graphics.default_msaa = 2;
        ps.quality.current_level = 0;
        ps.quality.levels[0].anti_aliasing = 8;
        jce_project_settings_apply(&ps);

        JceRenderSettings rs = jce_render_settings_default();
        std::string src;
        REQUIRE(jce_editor_effective_render_settings(false, ".", &rs, &src));
        CHECK(rs.msaa == 8);

        ps.quality.levels[0].anti_aliasing = JCE_PS_AA_USE_PROJECT_DEFAULT;
        jce_project_settings_apply(&ps);
        rs = jce_render_settings_default();
        REQUIRE(jce_editor_effective_render_settings(false, ".", &rs, &src));
        CHECK(rs.msaa == 2);
    }

    SUBCASE("a default project renders exactly as before")
    {
        /* THE NO-CHANGE CONTROL.  current_level defaults to 2 (High), whose
         * authored AA is 4, against a project default of 4 -- so wiring the
         * level must move nothing for a project nobody has touched.  If this
         * goes red, the change repainted every existing project instead of
         * honouring the ones that asked. */
        JceProjectSettings ps;
        jce_project_settings_defaults(&ps);
        const JceProjectQualityLevel *lvl =
            &ps.quality.levels[ps.quality.current_level];
        CHECK(jce_editor_quality_msaa(lvl->anti_aliasing,
                                      ps.graphics.default_msaa)
              == ps.graphics.default_msaa);
    }
}
