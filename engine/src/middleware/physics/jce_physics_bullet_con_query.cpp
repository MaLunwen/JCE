/*
 * jce_physics_bullet_con_query.cpp -- a constraint's joint feedback: install
 * it at creation, read it after each step.
 *
 * WHY ITS OWN TU.  The queries lived in jce_physics_bullet.cpp until adding
 * the torque one pushed that file to 3034 lines and check_file_size reported
 * it "newly a god file"; jce_bullet_con_register followed them when the same
 * gate fired again at 3015.  That second move is the better boundary anyway:
 * INSTALLING the btJointFeedback storage and READING it are one mechanism,
 * and keeping them apart is exactly how they drifted -- the install existed
 * on one creator and not the other while both queries assumed it.  Putting
 * the whole mechanism in one file is what makes that drift visible.
 *
 * THE TWO ARE NOT INTERCHANGEABLE, which is the point of having both.
 * getAppliedImpulse() returns ONE COMBINED MAGNITUDE with no separable
 * angular component; a joint's break_torque could not be built on it, which
 * is why that field was authored, serialised, tracked by the runtime and read
 * by nothing.  btJointFeedback carries force and torque apart, and the
 * constraint is given storage for it at creation.
 */

#include "jce_physics_bullet_internal.hpp"
#include "jce_physics_internal.h"

#include <string.h>

/* Put a finished constraint into the world and into the registry.
 *
 * ONE FUNCTION BECAUSE THERE ARE TWO CREATORS AND ONE WAS MISSING A LINE.
 * jce_bullet_constraint_create (typed) and jce_bullet_configurable_joint_create
 * both end with the same four steps and had drifted: the typed path installed
 * btJointFeedback storage, the configurable path called only
 * enableFeedback(true).  enableFeedback makes Bullet accumulate
 * m_appliedImpulse -- ONE combined scalar, which is why break_FORCE worked on
 * that path for its whole life -- but the separable force and torque are
 * written only if the constraint owns somewhere to put them.
 *
 * Measured cost: break_torque on a ConfigurableJoint read 0.0000 N*m while
 * the joint held an authored 50 N*m.  Every line of the query below was
 * correct; the storage was never installed on that path, so it returned at
 * `if (!fb)`.  The configurable joint is the ONLY authored joint that has a
 * break_torque -- the one creator that missed the line is the one where it
 * mattered.  Both callers go through here now, so a future joint type cannot
 * inherit half the wiring by being written next to the wrong neighbour. */
void jce_bullet_con_register(JceBulletWorld *bw, uint32_t idx,
                             btTypedConstraint *con, bool disable_collision)
{
    con->enableFeedback(true);
    /* No NULL guard on con_feedback, deliberately: jce_bullet_create fails the
     * whole world if any of the three constraint-pool arrays is NULL, so this
     * is guaranteed exactly as much as `constraints` and `con_alive` are two
     * lines below.  A guard here would be a second, silent way back into the
     * bug this function exists to prevent -- feedback enabled with nowhere to
     * write it reads as 0.0000 N*m forever.
     *
     * The memset is NOT optional: slots are recycled through con_alloc_cursor,
     * so without it a reused slot starts holding the previous constraint's
     * last-step forces and the first applied_torque after reuse is stale. */
    memset(&bw->con_feedback[idx], 0, sizeof(btJointFeedback));
    con->setJointFeedback(&bw->con_feedback[idx]);
    bw->world->addConstraint(con, disable_collision);
    bw->constraints[idx] = con;
    bw->con_alive[idx]   = true;
    bw->con_count++;
}

float jce_bullet_constraint_applied_impulse(JceBulletWorld *bw, uint32_t idx)
{
    if (!bw || idx >= bw->con_capacity || !bw->con_alive[idx]) return 0.0f;
    btTypedConstraint *con = bw->constraints[idx];
    if (!con) return 0.0f;
    /* btTypedConstraint accumulates m_appliedImpulse each solver step while the
     * constraint is in the world; getAppliedImpulse() returns its magnitude.
     * btFabs comes from Bullet's btScalar.h (always included via
     * btBulletDynamicsCommon.h) so no extra <cmath> dependency is needed. */
    return static_cast<float>(btFabs(con->getAppliedImpulse()));
}

/* The last step's applied TORQUE magnitude on body A, or 0.
 *
 * Separate from jce_bullet_constraint_applied_impulse on purpose: that one
 * returns getAppliedImpulse(), a single combined magnitude with no angular
 * component, which is exactly why a torque break could not be built on it.
 * btJointFeedback carries force and torque apart, and the constraint was
 * given storage for it at creation.
 *
 * WHAT THIS IS IN.  Bullet ships headers only here, so whether the solver
 * scales by the timestep before storing is not readable from the source and
 * is not asserted from memory: test_jce_joint_break_torque.c measures the
 * relationship against an authored threshold.  The runtime's comparison is in
 * one named place so that if the test says otherwise, exactly one line
 * moves. */
float jce_bullet_constraint_applied_torque(JceBulletWorld *bw, uint32_t idx)
{
    if (!bw || idx >= bw->con_capacity || !bw->con_alive[idx]) return 0.0f;
    btTypedConstraint *con = bw->constraints[idx];
    if (!con) return 0.0f;
    const btJointFeedback *fb = con->getJointFeedback();
    if (!fb) return 0.0f;

    /* BOTH SIDES, AND THAT IS NOT BELT-AND-BRACES -- reading only "body A"
     * returns 0 for every world-anchored joint.
     *
     * Bullet's single-body constructor is btGeneric6DofSpringConstraint(
     * btRigidBody& rbB, const btTransform& frameInB, bool): the body you hand
     * it becomes side B, and side A is Bullet's shared static getFixedBody().
     * So for a joint anchored to the world -- which is what a
     * JceConfigurableJoint with connected_body == 0 builds, and what most
     * authored joints in a scene are -- m_appliedTorqueBodyA describes the
     * immovable world, and the torque the author cares about is entirely in
     * m_appliedTorqueBodyB.  Measured before this read both: the runtime
     * break monitor saw 0.0000 N*m while the joint was holding an authored
     * 50 N*m, and break_torque stayed exactly as inert as it was before it
     * had a query at all.
     *
     * A constraint transmits equal and opposite torque to the two sides, so
     * the magnitude is the same quantity whichever side is populated, and the
     * larger of the two is the one that is not the degenerate fixed body.
     * Taking the max rather than branching on has_b keeps this independent of
     * which constructor the joint happened to be built with -- the caller
     * asks "how hard is this joint being twisted", and that has one answer. */
    const btScalar ta = fb->m_appliedTorqueBodyA.length();
    const btScalar tb = fb->m_appliedTorqueBodyB.length();
    return static_cast<float>(ta > tb ? ta : tb);
}
