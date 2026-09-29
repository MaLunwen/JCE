/*
 * test_jce_inspector_enum_maps.cpp
 *
 * The Rigidbody2D "Body Type" combo stored its raw ImGui row index.
 * JceBodyType is STATIC=0, DYNAMIC=1, KINEMATIC=2; the list reads
 *
 *     row 0  Static        row 1  Kinematic       row 2  Dynamic
 *
 * so picking "Kinematic" stored 1 == JCE_BODY_DYNAMIC and picking "Dynamic"
 * stored 2 == JCE_BODY_KINEMATIC.  A body that falls when the author asked
 * for it to be driven, and hangs in the air when they asked it to fall.
 *
 * WHY NOTHING IN THE EDITOR COULD REVEAL IT.  The read-back used the same
 * index, so the combo showed the label you picked; the panel agreed with
 * itself in both directions and only the physics disagreed.  That is why the
 * mapping is now data in a translation unit with no ImGui in it, and why this
 * test asserts the mapping directly rather than through the panel.
 *
 * ASSERTED AGAINST THE ENGINE ENUM, not against numbers.  Writing
 * `CHECK(insp_rb2d_body_type_value(1) == 2)` would pin today's byte and pass
 * just as happily if JceBodyType were renumbered underneath it -- and a
 * renumbering is exactly the change this mapping exists to survive.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "panels/jce_panel_inspector_enum_maps.h"

extern "C" {
#include <jce/middleware/physics/jce_physics_types.h>
#include <jce/middleware/scene/jce_scene.h>
}

TEST_CASE("the body-type combo's rows mean what their labels say")
{
    /* Row order is the panel's: static / kinematic / dynamic. */
    CHECK(insp_rb2d_body_type_value(0) == (uint8_t)JCE_BODY_STATIC);
    CHECK(insp_rb2d_body_type_value(1) == (uint8_t)JCE_BODY_KINEMATIC);
    CHECK(insp_rb2d_body_type_value(2) == (uint8_t)JCE_BODY_DYNAMIC);

    /* THE DEFECT, stated as what must NOT be true.  Both halves are needed:
       row 1 and row 2 were swapped with each other, so an assertion about
       only one of them would pass on a mapping that still swapped the pair. */
    CHECK(insp_rb2d_body_type_value(1) != (uint8_t)JCE_BODY_DYNAMIC);
    CHECK(insp_rb2d_body_type_value(2) != (uint8_t)JCE_BODY_KINEMATIC);
}

TEST_CASE("the round trip is the identity, in both directions")
{
    /* index -> value -> index, for every row the combo can draw. */
    for (int row = 0; row < JCE_INSP_BODY_TYPE_COUNT; ++row)
        CHECK(insp_rb2d_body_type_index(insp_rb2d_body_type_value(row)) == row);

    /* value -> index -> value, for every value the enum can hold.  This is
       the direction the panel does on every frame: it turns the component's
       stored byte into a highlighted row. */
    const uint8_t all[] = { (uint8_t)JCE_BODY_STATIC,
                            (uint8_t)JCE_BODY_DYNAMIC,
                            (uint8_t)JCE_BODY_KINEMATIC };
    for (uint8_t v : all)
        CHECK(insp_rb2d_body_type_value(insp_rb2d_body_type_index(v)) == v);
}

TEST_CASE("a value the list cannot show highlights row 0 and rewrites nothing")
{
    /* A byte from a newer scene, or a shape the combo does not offer.  The
       combo already fell back to row 0; what matters is that asking the
       question does not itself change the component -- only insp_undo_set on
       an actual edit may do that, and it is not called here. */
    CHECK(insp_rb2d_body_type_index((uint8_t)200) == 0);
    CHECK(insp_rb2d_body_type_value(-1) == (uint8_t)JCE_BODY_STATIC);
    CHECK(insp_rb2d_body_type_value(JCE_INSP_BODY_TYPE_COUNT) ==
          (uint8_t)JCE_BODY_STATIC);
}

TEST_CASE("the 3D body-kind map is NOT the 2D one")
{
    /* Two maps on purpose.  The 3D field is JCE_RB_KIND_* with AUTO at 0; the
     * 2D field is JceBodyType with STATIC at 0.  Sharing one map would make
     * the 3D combo's "Auto" row store STATIC on a 2D body -- a body that stops
     * moving because somebody reused a table. */
    CHECK(insp_rb_kind_value(0) == (uint8_t)JCE_RB_KIND_AUTO);
    CHECK(insp_rb_kind_value(1) == (uint8_t)JCE_RB_KIND_STATIC);
    CHECK(insp_rb_kind_value(2) == (uint8_t)JCE_RB_KIND_KINEMATIC);
    CHECK(insp_rb_kind_value(3) == (uint8_t)JCE_RB_KIND_DYNAMIC);

    /* Row 0 must store 0.  If AUTO ever stops being 0, every scene written
     * before it changes meaning -- so this is pinned in two places. */
    CHECK(insp_rb_kind_value(0) == 0);

    for (int row = 0; row < JCE_INSP_RB_KIND_COUNT; ++row)
        CHECK(insp_rb_kind_index(insp_rb_kind_value(row)) == row);

    /* And the two maps genuinely disagree, which is the point of having both:
     * the same row number means different bytes. */
    CHECK(insp_rb_kind_value(1) != insp_rb2d_body_type_value(1));

    CHECK(insp_rb_kind_index((uint8_t)200) == 0);
}

TEST_CASE("the shape-type map, which both rigidbody drawers now share")
{
    /* This one was already correct in both panels -- as two copies of the
       same three-entry table.  It is here because the copies are gone, and a
       shared table that nothing asserts is how the next swap gets in. */
    CHECK(insp_rb_shape_type_value(0) == (uint8_t)JCE_SHAPE_BOX);
    CHECK(insp_rb_shape_type_value(1) == (uint8_t)JCE_SHAPE_SPHERE);
    CHECK(insp_rb_shape_type_value(2) == (uint8_t)JCE_SHAPE_CAPSULE);

    for (int row = 0; row < JCE_INSP_SHAPE_TYPE_COUNT; ++row)
        CHECK(insp_rb_shape_type_index(insp_rb_shape_type_value(row)) == row);

    /* CONVEX_HULL is deliberately not offered: it needs authored geometry and
       the spawn falls back to Box for it.  It must not be silently mapped
       onto one of the three rows as if the author had picked it. */
    CHECK(insp_rb_shape_type_index((uint8_t)JCE_SHAPE_CONVEX_HULL) == 0);
    CHECK(insp_rb_shape_type_value(insp_rb_shape_type_index(
              (uint8_t)JCE_SHAPE_CONVEX_HULL)) != (uint8_t)JCE_SHAPE_CONVEX_HULL);
}
