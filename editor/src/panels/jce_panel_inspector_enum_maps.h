/*
 * jce_panel_inspector_enum_maps.h
 *
 * Combo index <-> engine enum, for the inspector combos whose list order is
 * NOT the enum's order.
 *
 * WHY THIS IS A FILE AND NOT A LOCAL ARRAY.  The Rigidbody2D body-type combo
 * used its raw ImGui index as the stored value.  JceBodyType is STATIC=0,
 * DYNAMIC=1, KINEMATIC=2; the list reads static / kinematic / dynamic.  So
 * picking "Kinematic" stored DYNAMIC and picking "Dynamic" stored KINEMATIC --
 * a body that falls when you asked it to be driven, and ignores gravity when
 * you asked it to fall.  The panel agreed with itself in BOTH directions
 * (reading the stored value back through the same index showed the label you
 * picked), so nothing about the editor could reveal it; only the physics
 * disagreed.
 *
 * Index order is a UI decision -- alphabetical, or most-common-first, or
 * whatever reads well in a dropdown.  Enum order is an ABI one, frozen in
 * contracts/abi-snapshot.txt.  Nothing keeps them equal, so the mapping is
 * data with a name, in a translation unit that pulls in no ImGui, and a
 * headless test asserts it.
 *
 * The shape-type pair lives here for a second reason: the same three-entry
 * table was copy-pasted into both the 3D and the 2D rigidbody drawers.
 */
#ifndef JCE_PANEL_INSPECTOR_ENUM_MAPS_H
#define JCE_PANEL_INSPECTOR_ENUM_MAPS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Rigidbody2D "Body Type": list order static / kinematic / dynamic. */
enum { JCE_INSP_BODY_TYPE_COUNT = 3 };
uint8_t insp_rb2d_body_type_value(int combo_index);
int     insp_rb2d_body_type_index(uint8_t body_type);

/* 3D Rigidbody "Body Type": list order auto / static / kinematic / dynamic.
   A DIFFERENT map from the 2D one above, and deliberately so: the 3D field is
   JCE_RB_KIND_* with AUTO at 0, the 2D field is JceBodyType with STATIC at 0.
   Sharing one map would make "Auto" store STATIC on a 2D body. */
enum { JCE_INSP_RB_KIND_COUNT = 4 };
uint8_t insp_rb_kind_value(int combo_index);
int     insp_rb_kind_index(uint8_t kind);

/* Rigidbody "Shape Type" (3D and 2D): list order box / sphere / capsule.
   Only the three derivable from the transform scale are offered -- a hull or
   a triangle mesh needs authored geometry, so listing it would offer a choice
   that silently falls back to Box. */
enum { JCE_INSP_SHAPE_TYPE_COUNT = 3 };
uint8_t insp_rb_shape_type_value(int combo_index);
int     insp_rb_shape_type_index(uint8_t shape_type);

#ifdef __cplusplus
}
#endif

#endif /* JCE_PANEL_INSPECTOR_ENUM_MAPS_H */
