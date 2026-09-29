/*
 * jce_physics_bullet_contact.cpp -- the one global contact-added hook, and
 * everything that rides on it.
 *
 * WHY ITS OWN TU.  Adding the per-contact material combine pushed
 * jce_physics_bullet.cpp to 3103 lines and check_file_size reported it "newly
 * a god file" -- the same gate that moved the constraint queries out twice
 * (see jce_physics_bullet_con_query.cpp, which records why its second cut was
 * the better boundary).  The alternative was --raise with a reason, and a
 * ratchet that can be raised by writing prose teaches people to write prose.
 *
 * AND THERE IS A REAL BOUNDARY HERE.  Bullet routes every added contact
 * through ONE global function pointer, so whoever owns it must chain rather
 * than stomp, and anything that wants to influence a contact has to go
 * through it.  Until now the hook was ARMED inside
 * jce_bullet_body_create_heightfield, guarded by `smooth_internal_edges` --
 * so a world with no smooth heightfield had no contact callback at all, and
 * a mechanism's installation lived somewhere with nothing to do with what the
 * mechanism does.  That is the exact drift the constraint-query file warns
 * about.  One file now holds the hook, the chaining, the per-body encoding
 * and the combine.
 *
 * WHAT RIDES ON IT, in order:
 *   1. internal-edge smoothing -- removes the ghost bumps a capsule feels
 *      crossing the shared diagonal of every heightfield cell.  Guarded by
 *      the SHAPE's user pointer, which is the marker set when edge info was
 *      generated, so it stays inert for every other shape.
 *   2. the authored physics-material combine -- Unity's precedence rule
 *      (MAX > MULTIPLY > MIN > AVERAGE) reaching the solver.  Guarded by the
 *      BODY's userIndex2, zero for any body that never got a material.
 */

#include "jce_physics_bullet_internal.hpp"
#include "jce_physics_internal.h"

#include <jce/middleware/physics/jce_physics_material.h>

#include <BulletCollision/CollisionDispatch/btInternalEdgeUtility.h>

static ContactAddedCallback s_prev_contact_added = nullptr;

/* The two JCE combine modes, packed into btCollisionObject::setUserIndex2.
 *
 * They ride on the BODY rather than in a world-side table because the contact
 * callback Bullet hands us is a free function with no world pointer, and
 * Bullet offers no object->world mapping.  A file-scope world pointer would
 * be a new global; this is four bits in a slot nothing else uses
 * (setUserIndex is the vehicle-chassis marker, userIndex3 is untouched).
 *
 * Both modes are stored +1 so that ZERO means "no JCE material was ever
 * applied to this body" -- distinct from AVERAGE, which is a real authored
 * choice.  Without that distinction a default-constructed body would be
 * indistinguishable from one an author deliberately set to AVERAGE, and the
 * first would silently change behaviour. */
int jce_bullet_pack_combine(JcePhysicsCombine f, JcePhysicsCombine r)
{
    return ((static_cast<int>(f) + 1) & 0xF) |
           (((static_cast<int>(r) + 1) & 0xF) << 4);
}

static bool jce_bullet_unpack_combine(int packed, JcePhysicsCombine *out_f,
                                      JcePhysicsCombine *out_r)
{
    if (packed == 0) return false;            /* no material on this body */
    const int f = (packed & 0xF) - 1;
    const int r = ((packed >> 4) & 0xF) - 1;
    if (f < 0 || r < 0) return false;
    *out_f = static_cast<JcePhysicsCombine>(f);
    *out_r = static_cast<JcePhysicsCombine>(r);
    return true;
}

/* Build the JCE view of a Bullet body's material.  Friction and restitution
 * come from Bullet, which has held them all along; only the modes are ours.
 * A body with no JCE material keeps its real friction and restitution and
 * takes the DEFAULT modes, so a material on one side still governs the pair
 * exactly as Unity's precedence rule says it should. */
static void jce_bullet_material_of(const btCollisionObject *obj,
                                   JcePhysicsMaterial *out)
{
    jce_physics_material_init_default(out);
    out->dynamic_friction = static_cast<float>(obj->getFriction());
    out->static_friction  = out->dynamic_friction;
    out->restitution      = static_cast<float>(obj->getRestitution());
    JcePhysicsCombine f, r;
    if (jce_bullet_unpack_combine(obj->getUserIndex2(), &f, &r)) {
        out->friction_combine    = f;
        out->restitution_combine = r;
    }
}

/* The hook itself.  Every rider named in the file header runs here, in that
 * order, and each is guarded so it costs nothing for the contacts it does
 * not concern.  It ends by chaining to whatever owned the pointer before,
 * because Bullet has exactly one and stomping it would silently disable
 * someone else's mechanism. */
static bool jce_bullet_edge_contact_added(btManifoldPoint &cp,
                                          const btCollisionObjectWrapper *a,
                                          int partId0, int index0,
                                          const btCollisionObjectWrapper *b,
                                          int partId1, int index1)
{
    /* getUserPointer() is the marker set when edge info was generated, so
     * shapes without it are skipped rather than mis-adjusted. */
    if (a && a->getCollisionShape() && a->getCollisionShape()->getUserPointer())
        btAdjustInternalEdgeContacts(cp, a, b, partId0, index0);
    if (b && b->getCollisionShape() && b->getCollisionShape()->getUserPointer())
        btAdjustInternalEdgeContacts(cp, b, a, partId1, index1);

    /* The authored combine modes reach the solver HERE and nowhere else.
     * Bullet has already written its own m_combinedFriction/Restitution from
     * its fixed multiply/max rule; overwriting them is the documented way to
     * impose a different one.  Skipped entirely unless at least one side
     * carries a JCE material, so the default path costs one int compare. */
    {
        const btCollisionObject *oa = a ? a->getCollisionObject() : nullptr;
        const btCollisionObject *ob = b ? b->getCollisionObject() : nullptr;
        if (oa && ob && (oa->getUserIndex2() != 0 || ob->getUserIndex2() != 0)) {
            JcePhysicsMaterial ma, mb;
            jce_bullet_material_of(oa, &ma);
            jce_bullet_material_of(ob, &mb);
            float f = 0.0f, r = 0.0f;
            /* ONE implementation of the rule.  The precedence
             * (MAX > MULTIPLY > MIN > AVERAGE) lives in
             * jce_physics_material_combine and is not restated here -- this
             * whole change exists because that function had no caller. */
            jce_physics_material_combine(&ma, &mb, &f, &r);
            cp.m_combinedFriction    = static_cast<btScalar>(f);
            cp.m_combinedRestitution = static_cast<btScalar>(r);
        }
    }

    if (s_prev_contact_added)
        return s_prev_contact_added(cp, a, partId0, index0, b, partId1, index1);
    return false;
}

void jce_bullet_install_contact_hook(void)
{
    /* Idempotent and chaining.  Called from jce_bullet_create, so every world
     * has it from its first step -- not from a heightfield that may never be
     * created.  The guard is against installing over ourselves on a second
     * world, which would make s_prev_contact_added point at us and recurse. */
    if (gContactAddedCallback != jce_bullet_edge_contact_added) {
        s_prev_contact_added = gContactAddedCallback;
        gContactAddedCallback = jce_bullet_edge_contact_added;
    }
}
