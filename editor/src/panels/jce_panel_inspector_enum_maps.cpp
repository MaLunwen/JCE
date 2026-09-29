/*
 * jce_panel_inspector_enum_maps.cpp — see the header for why this exists.
 *
 * No ImGui, no editor globals: it is a pair of tables, so a headless test can
 * link it on its own and assert the mapping the panels could not reveal.
 */
#include "jce_panel_inspector_enum_maps.h"

extern "C" {
#include <jce/middleware/physics/jce_physics_types.h>
#include <jce/middleware/scene/jce_scene.h>
}

namespace {

/* The stored value for each combo row, IN THE ROW ORDER THE PANEL DRAWS.
   Written out rather than derived: an expression like `index` or
   `index ? index + 1 : 0` is exactly the kind of accidental agreement that
   let the original defect through. */
const uint8_t kBodyTypeVals[JCE_INSP_BODY_TYPE_COUNT] = {
    (uint8_t)JCE_BODY_STATIC,      /* row 0: "Static"    */
    (uint8_t)JCE_BODY_KINEMATIC,   /* row 1: "Kinematic" */
    (uint8_t)JCE_BODY_DYNAMIC,     /* row 2: "Dynamic"   */
};

const uint8_t kRbKindVals[JCE_INSP_RB_KIND_COUNT] = {
    (uint8_t)JCE_RB_KIND_AUTO,       /* row 0: "Auto (mass / kinematic)" */
    (uint8_t)JCE_RB_KIND_STATIC,     /* row 1: "Static"    */
    (uint8_t)JCE_RB_KIND_KINEMATIC,  /* row 2: "Kinematic" */
    (uint8_t)JCE_RB_KIND_DYNAMIC,    /* row 3: "Dynamic"   */
};

const uint8_t kShapeTypeVals[JCE_INSP_SHAPE_TYPE_COUNT] = {
    (uint8_t)JCE_SHAPE_BOX,        /* row 0: "Box"     */
    (uint8_t)JCE_SHAPE_SPHERE,     /* row 1: "Sphere"  */
    (uint8_t)JCE_SHAPE_CAPSULE,    /* row 2: "Capsule" */
};

int index_of(const uint8_t *vals, int n, uint8_t v)
{
    for (int i = 0; i < n; ++i)
        if (vals[i] == v) return i;
    /* A value the list cannot show -- a hull on shape_type, say, or a byte
       from a newer scene.  Row 0 is what the combo already fell back to; the
       point is that it does NOT silently rewrite the component, which only
       insp_undo_set on an actual edit may do. */
    return 0;
}

} /* namespace */

extern "C" {

uint8_t insp_rb2d_body_type_value(int combo_index)
{
    if (combo_index < 0 || combo_index >= JCE_INSP_BODY_TYPE_COUNT)
        return (uint8_t)JCE_BODY_STATIC;
    return kBodyTypeVals[combo_index];
}

int insp_rb2d_body_type_index(uint8_t body_type)
{
    return index_of(kBodyTypeVals, JCE_INSP_BODY_TYPE_COUNT, body_type);
}

uint8_t insp_rb_kind_value(int combo_index)
{
    if (combo_index < 0 || combo_index >= JCE_INSP_RB_KIND_COUNT)
        return (uint8_t)JCE_RB_KIND_AUTO;
    return kRbKindVals[combo_index];
}

int insp_rb_kind_index(uint8_t kind)
{
    return index_of(kRbKindVals, JCE_INSP_RB_KIND_COUNT, kind);
}

uint8_t insp_rb_shape_type_value(int combo_index)
{
    if (combo_index < 0 || combo_index >= JCE_INSP_SHAPE_TYPE_COUNT)
        return (uint8_t)JCE_SHAPE_BOX;
    return kShapeTypeVals[combo_index];
}

int insp_rb_shape_type_index(uint8_t shape_type)
{
    return index_of(kShapeTypeVals, JCE_INSP_SHAPE_TYPE_COUNT, shape_type);
}

} /* extern "C" */
