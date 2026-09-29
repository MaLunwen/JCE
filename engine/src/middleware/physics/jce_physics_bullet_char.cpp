/*
 * jce_physics_bullet_char.cpp — the capsule character controller.
 *
 * Split out of jce_physics_bullet.cpp when adding the moving-platform carry
 * took it to 3,056 lines, past AGENTS.md §11's 3,000-line cap.  The gate
 * called it "newly a god file", and extraction has beaten --raise on this
 * file twice already.
 *
 * THE SIBLING TU'S HEADER SAYS THIS BLOCK COULD NOT BE MOVED, AND THAT IS NO
 * LONGER TRUE.  jce_physics_bullet_vehicle.cpp records that the vehicle was
 * chosen because "its interface is the narrowest in the file" and that "the
 * character controller is twice the size and would have dragged the body and
 * shape tables across with it."  Measured before cutting, the character
 * block touches:
 *
 *     bw->char_bodies  char_capacity  char_alive  char_feel  char_shapes
 *     bw->char_jump  char_count  char_alloc_cursor  world  ghosts  characters
 *
 * and calls NO static function defined in the original TU.  Every one of
 * those members is already declared in jce_physics_bullet_internal.hpp, and
 * to_bt / from_bt_v3 are static inline there.  So its interface is exactly as
 * narrow as the vehicle's -- the sentence was either true of an older
 * JceBulletWorld or was a conservative estimate, and it has been sitting
 * there discouraging the move ever since.  Re-measured rather than believed.
 */

#include "jce_physics_bullet_internal.hpp"

/* ================================================================== */
/* Character controller                                                */
/* ================================================================== */

uint32_t jce_bullet_character_create(JceBulletWorld *bw,
                                      jce_vec3 pos, float radius,
                                      float height, float step_height,
                                      float max_slope_rad,
                                      float gravity, float jump_speed,
                                      float accel, float air_control,
                                      uint32_t col_group, uint32_t col_mask)
{
    if (!bw) return UINT32_MAX;

    /* Find a free slot (rotating cursor → O(1) amortized bursts). */
    uint32_t idx = UINT32_MAX;
    for (uint32_t n = 0; n < bw->char_capacity; ++n) {
        uint32_t i = (bw->char_alloc_cursor + n) % bw->char_capacity;
        if (!bw->char_alive[i]) { idx = i; break; }
    }
    if (idx == UINT32_MAX) return UINT32_MAX;
    bw->char_alloc_cursor = (idx + 1u) % bw->char_capacity;

    /* Capsule shape: total height = capsule_height + 2*radius. */
    float capsule_height = height - 2.0f * radius;
    if (capsule_height < 0.01f) capsule_height = 0.01f;

    (void)gravity;  /* dynamic capsule falls under world gravity */

    auto *cap_shape = new btCapsuleShape(
        static_cast<btScalar>(radius),
        static_cast<btScalar>(capsule_height));

    /* DYNAMIC capsule rigid body. The solver resolves it together with whatever
     * it rests on (floor, a crate, a stack of crates) so there is no kinematic-
     * vs-dynamic fight → stacking is stable, no jitter. Rotation is fully locked
     * so it never tips; horizontal motion is driven by setting velocity. A
     * modest mass keeps the mass ratio to light crates solver-stable. */
    btScalar mass = btScalar(10.0);
    btVector3 inertia(0, 0, 0);
    cap_shape->calculateLocalInertia(mass, inertia);

    btTransform start_xf;
    start_xf.setIdentity();
    start_xf.setOrigin(to_bt(pos));

    btRigidBody::btRigidBodyConstructionInfo ci(mass, nullptr, cap_shape, inertia);
    ci.m_startWorldTransform = start_xf;
    ci.m_friction            = btScalar(0.0);  /* horizontal is velocity-driven */
    ci.m_restitution         = btScalar(0.0);
    auto *body = new btRigidBody(ci);
    body->setAngularFactor(btVector3(0, 0, 0));   /* never tip / spin */
    body->setActivationState(DISABLE_DEACTIVATION);
    body->setCollisionFlags(body->getCollisionFlags() |
                            btCollisionObject::CF_CHARACTER_OBJECT);

    bw->world->addRigidBody(body,
                            static_cast<int>(col_group),
                            static_cast<int>(col_mask));

    bw->characters[idx]  = nullptr;
    bw->ghosts[idx]      = nullptr;
    bw->char_bodies[idx] = body;
    bw->char_jump[idx]   = jump_speed > 0.0f ? jump_speed : 5.0f;
    bw->char_shapes[idx] = cap_shape;
    bw->char_alive[idx]  = true;
    bw->char_count++;

    JceBulletCharFeel *f = &bw->char_feel[idx];
    f->accel       = accel > 0.0f ? accel : 40.0f;
    f->air_control = (air_control > 0.0f) ? air_control : 0.35f;
    if (f->air_control > 1.0f) f->air_control = 1.0f;
    f->step_height = step_height > 0.0f ? step_height : 0.35f;
    btScalar slope = max_slope_rad > 0.0f ? btScalar(max_slope_rad)
                                          : btRadians(btScalar(50.0));
    if (slope > btRadians(btScalar(89.0))) slope = btRadians(btScalar(89.0));
    f->max_slope_cos = (float)btCos(slope);
    f->filter_group  = static_cast<int>(col_group);
    f->filter_mask   = static_cast<int>(col_mask);
    f->grounded      = false;
    f->touching      = false;
    f->probe_valid   = false;
    f->jumping       = false;
    f->ground_normal = btVector3(0, 1, 0);
    f->ground_body   = nullptr;
    f->platform_vel  = btVector3(0, 0, 0);

    return idx;
}

void jce_bullet_character_destroy(JceBulletWorld *bw, uint32_t idx)
{
    if (!bw || idx >= bw->char_capacity || !bw->char_alive[idx]) return;

    if (bw->char_bodies[idx]) {
        bw->world->removeRigidBody(bw->char_bodies[idx]);
        delete bw->char_bodies[idx];
        bw->char_bodies[idx] = nullptr;
    }
    delete bw->char_shapes[idx];
    bw->char_shapes[idx] = nullptr;
    bw->char_alive[idx] = false;
    bw->char_count--;
}

/* Down-ray helper shared by the ground probe / snap / step-up.  Casts
 * from `from` straight down `reach` metres against the character mask;
 * fills hit point + normal.  Returns false on miss (or self-hit). */
static bool char_ray_down(JceBulletWorld *bw, const btRigidBody *self,
                          const btVector3 &from, btScalar reach,
                          const JceBulletCharFeel *f,
                          btVector3 *out_point, btVector3 *out_normal,
                          const btCollisionObject **out_obj)
{
    btVector3 to = from - btVector3(0, reach, 0);
    btCollisionWorld::ClosestRayResultCallback cb(from, to);
    cb.m_collisionFilterGroup = f->filter_group;
    cb.m_collisionFilterMask  = f->filter_mask;
    bw->world->rayTest(from, to, cb);
    if (!cb.hasHit() || cb.m_collisionObject == self) return false;
    if (out_point)  *out_point  = cb.m_hitPointWorld;
    if (out_normal) *out_normal = cb.m_hitNormalWorld;
    /* The hit object was already computed and thrown away.  It is what the
     * ground probe needs to know WHAT it is standing on, which is the one
     * thing the feel state could not previously recover. */
    if (out_obj)    *out_obj    = cb.m_collisionObject;
    return true;
}

/* Refresh the cached grounded state + ground normal: a 5-ray fan
 * (capsule axis + 4 compass points at 0.6 r) so standing on an edge or
 * stair lip still reads as grounded; keeps the most upright normal. */
static bool char_ground_probe(JceBulletWorld *bw, uint32_t idx)
{
    btRigidBody *b = bw->char_bodies[idx];
    auto *cap = static_cast<btCapsuleShape *>(bw->char_shapes[idx]);
    JceBulletCharFeel *f = &bw->char_feel[idx];
    btScalar half = cap->getHalfHeight() + cap->getRadius();  /* centre→foot */
    btScalar ring = cap->getRadius() * btScalar(0.6);
    btVector3 c   = b->getWorldTransform().getOrigin();
    btScalar reach = half + btScalar(0.20);

    static const btScalar offs[5][2] = {
        {0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1}
    };
    bool      hit_any = false;
    btVector3 best_n(0, 1, 0);
    btScalar  best_y = btScalar(-2.0);
    /* Kept alongside the winning normal, not separately: the surface whose
     * normal wins is the surface the character is standing on, so its body
     * and the point it was hit at have to come from the SAME ray. */
    const btCollisionObject *best_obj = nullptr;
    btVector3                best_p(0, 0, 0);
    for (int i = 0; i < 5; ++i) {
        btVector3 from = c + btVector3(offs[i][0] * ring, 0, offs[i][1] * ring);
        btVector3 n, p;
        const btCollisionObject *obj = nullptr;
        if (char_ray_down(bw, b, from, reach, f, &p, &n, &obj)) {
            hit_any = true;
            if (n.y() > best_y) {
                best_y = n.y(); best_n = n; best_obj = obj; best_p = p;
            }
        }
    }
    bool grounded = hit_any;
    /* Ascending from a jump the feet stay within probe reach for a tick
     * or two — that must NOT read as grounded (it would re-arm coyote
     * time and skip the variable-jump cut). */
    if (f->jumping && b->getLinearVelocity().y() > btScalar(0.5))
        grounded = false;
    /* A face steeper than the slope limit supports no locomotion: report
     * airborne so animation shows the slide and jumps can't pogo up it. */
    if (grounded && best_n.y() < btScalar(f->max_slope_cos))
        grounded = false;
    f->grounded      = grounded;
    f->touching      = hit_any;
    f->probe_valid   = true;
    f->ground_normal = hit_any ? best_n : btVector3(0, 1, 0);

    /* What the feet are standing on, and how fast that surface is moving
     * THERE.  Gated on `grounded` rather than `hit_any` on purpose: a face
     * too steep to walk on, or a capsule still rising out of a jump, supports
     * no locomotion, so it must not impart any either.
     *
     * getVelocityInLocalPoint rather than getLinearVelocity, because a
     * platform that spins carries a rider tangentially and its centre of mass
     * may not be moving at all.  A STATIC floor is a btRigidBody with zero
     * linear and angular velocity, so this yields exactly zero there -- the
     * "standing on solid ground does not drift" case is true by construction
     * and needs no special-casing. */
    f->ground_body  = grounded ? best_obj : nullptr;
    f->platform_vel = btVector3(0, 0, 0);
    if (grounded && best_obj) {
        const btRigidBody *rb = btRigidBody::upcast(best_obj);
        if (rb)
            f->platform_vel =
                rb->getVelocityInLocalPoint(best_p - rb->getCenterOfMassPosition());
    }
    return grounded;
}

/* `walk_dir` is the desired planar VELOCITY (m/s).  The horizontal
 * velocity ACCELERATES toward it (accel on ground, accel*air_control
 * airborne) for natural starts/stops; the solver-owned vertical velocity
 * (gravity / jump / resting) is preserved.  Also handles, per fixed tick:
 *   - ground probe refresh (cached for is_grounded queries),
 *   - ground snap when walking down steps/slopes (kills the airborne arc),
 *   - max-slope limit (the uphill velocity component is removed on
 *     too-steep faces, so the capsule can't drive up them),
 *   - step-up assist (low blocker ahead + clearance at step height →
 *     teleport up the step, momentum preserved). */
void jce_bullet_character_move(JceBulletWorld *bw, uint32_t idx,
                                jce_vec3 walk_dir, float dt)
{
    if (!bw || idx >= bw->char_capacity || !bw->char_alive[idx]) return;
    btRigidBody *b = bw->char_bodies[idx];
    if (!b) return;
    if (dt <= 0.0f) dt = 1.0f / 60.0f;

    JceBulletCharFeel *f = &bw->char_feel[idx];
    auto *cap = static_cast<btCapsuleShape *>(bw->char_shapes[idx]);
    btScalar half   = cap->getHalfHeight() + cap->getRadius();
    btScalar radius = cap->getRadius();

    bool was_grounded = f->probe_valid && f->grounded;
    char_ground_probe(bw, idx);

    btVector3 v = b->getLinearVelocity();
    if (f->grounded && v.y() <= btScalar(0.5)) f->jumping = false;

    /* Ground snap: just walked off a step/slope crest (not a jump, not
     * rising) and the ground is within step_height below the feet →
     * glue the capsule back down instead of arcing off. */
    if (was_grounded && !f->grounded && !f->jumping &&
        v.y() <= btScalar(0.1)) {
        btVector3 c = b->getWorldTransform().getOrigin();
        btVector3 feet = c - btVector3(0, half, 0);
        btVector3 hit, n;
        if (char_ray_down(bw, b, feet, btScalar(f->step_height) + btScalar(0.05), f,
                          &hit, &n, nullptr) &&
            n.y() >= btScalar(f->max_slope_cos)) {
            btScalar drop = feet.y() - hit.y();
            if (drop > btScalar(0.0)) {
                btTransform xf = b->getWorldTransform();
                xf.setOrigin(c - btVector3(0, drop - btScalar(0.01), 0));
                b->setWorldTransform(xf);
                b->setInterpolationWorldTransform(xf);
                v.setY(0);
                f->grounded      = true;
                f->ground_normal = n;
            }
        }
    }

    /* Max-slope limit: in contact with a too-steep face — strip the uphill
     * component of the commanded velocity (along/downhill still allowed),
     * so a frictionless capsule cannot power up a cliff face.  Uses the
     * raw `touching` contact (steep faces deliberately don't count as
     * `grounded` for jumps/animation). */
    btVector3 target(static_cast<btScalar>(walk_dir.x), 0,
                     static_cast<btScalar>(walk_dir.z));
    if (f->touching && f->ground_normal.y() < btScalar(f->max_slope_cos)) {
        btVector3 uphill(-f->ground_normal.x(), 0, -f->ground_normal.z());
        btScalar ul = uphill.length();
        if (ul > btScalar(1e-4)) {
            uphill /= ul;
            btScalar into = target.dot(uphill);
            if (into > btScalar(0.0)) target -= uphill * into;
        }
    }

    /* Ride the surface.  The commanded velocity is relative to whatever the
     * feet are on, so `walk_dir == 0` converges on the platform's own velocity
     * and standing still means standing still RELATIVE TO IT.
     *
     * This is the whole fix for a character on a moving platform, and nothing
     * else in this controller can supply it: the capsule is created with
     * m_friction = 0 on purpose (horizontal motion is velocity-driven), so no
     * friction drags it along, and the block below recomputes the horizontal
     * velocity ABSOLUTELY every tick, so any momentum the solver did impart is
     * overwritten before it can move anything.
     *
     * AFTER the max-slope strip, not before: that strip exists to stop a
     * frictionless capsule DRIVING up a face it cannot walk on, which is a
     * statement about the command.  A platform's motion is not a command, and
     * subtracting it there would make a moving ramp push the character in a
     * direction neither the author nor the physics asked for.
     *
     * ON LEAVING, platform_vel is zero and the capsule decelerates toward the
     * bare walk command at accel*air_control.  The inherited speed is NOT
     * conserved -- it bleeds off over the airtime rather than carrying, which
     * is UE's bImpartBaseVelocity switched off.  That is a choice and it is
     * written here because it is invisible in the code: conserving it means
     * latching platform_vel at the moment the probe loses the body, which is a
     * separate behaviour with its own failure modes. */
    target += btVector3(f->platform_vel.x(), 0, f->platform_vel.z());

    /* Accelerate the horizontal velocity toward the target. */
    btScalar rate   = btScalar(f->grounded ? f->accel
                                           : f->accel * f->air_control);
    btScalar max_dv = rate * btScalar(dt);
    btVector3 dv(target.x() - v.x(), 0, target.z() - v.z());
    btScalar  dl = dv.length();
    if (dl > max_dv && dl > SIMD_EPSILON) dv *= max_dv / dl;
    v.setX(v.x() + dv.x());
    v.setZ(v.z() + dv.z());
    b->setLinearVelocity(v);
    b->activate();

    /* Step-up assist: pushing into a low blocker while grounded. */
    btVector3 dir = target;
    btScalar  sp  = dir.length();
    if (f->grounded && sp > btScalar(0.1)) {
        dir /= sp;
        btVector3 c    = b->getWorldTransform().getOrigin();
        btScalar  feet = c.y() - half;
        btScalar  step = btScalar(f->step_height);

        auto fwd_hit = [&](btScalar lift_y, btScalar reach,
                           btVector3 *n_out) -> bool {
            btVector3 from(c.x(), feet + lift_y, c.z());
            btVector3 to = from + dir * reach;
            btCollisionWorld::ClosestRayResultCallback cb(from, to);
            cb.m_collisionFilterGroup = f->filter_group;
            cb.m_collisionFilterMask  = f->filter_mask;
            bw->world->rayTest(from, to, cb);
            if (!cb.hasHit() || cb.m_collisionObject == b) return false;
            if (n_out) *n_out = cb.m_hitNormalWorld;
            return true;
        };

        /* Blocked at ankle height by a RISER (a face too steep to walk —
         * a walkable ramp ahead also intersects the ankle ray, but that is
         * the slope/solver's job, not a step) and clear at step height? */
        btVector3 ankle_n(0, 1, 0);
        if (fwd_hit(btScalar(0.05), radius + btScalar(0.12), &ankle_n) &&
            ankle_n.y() < btScalar(f->max_slope_cos) &&
            !fwd_hit(step + btScalar(0.05), radius + btScalar(0.15), nullptr)) {
            /* Find the step's top surface just past the blocker. */
            btVector3 top_from = btVector3(c.x(), feet + step + btScalar(0.05),
                                           c.z()) + dir * (radius + btScalar(0.15));
            btVector3 hit, n;
            if (char_ray_down(bw, b, top_from, step + btScalar(0.10), f, &hit, &n,
                              nullptr) &&
                n.y() >= btScalar(f->max_slope_cos)) {
                btScalar lift = hit.y() - feet;
                if (lift > btScalar(0.02) && lift <= step + btScalar(0.01)) {
                    /* Head clearance: test ABOVE the capsule top (a ray from
                     * the center would lie inside our own volume and always
                     * report clear). */
                    btVector3 head_from = c + btVector3(0, half, 0);
                    btVector3 head_to   = head_from +
                                          btVector3(0, lift + btScalar(0.05), 0);
                    btCollisionWorld::ClosestRayResultCallback hc(head_from, head_to);
                    hc.m_collisionFilterGroup = f->filter_group;
                    hc.m_collisionFilterMask  = f->filter_mask;
                    bw->world->rayTest(head_from, head_to, hc);
                    if (!hc.hasHit() || hc.m_collisionObject == b) {
                        btTransform xf = b->getWorldTransform();
                        xf.setOrigin(c + btVector3(0, lift + btScalar(0.02), 0)
                                       + dir * btScalar(0.02));
                        b->setWorldTransform(xf);
                        b->setInterpolationWorldTransform(xf);
                        btVector3 vv = b->getLinearVelocity();
                        if (vv.y() < btScalar(0.0)) {
                            vv.setY(0);
                            b->setLinearVelocity(vv);
                        }
                    }
                }
            }
        }
    }
}

/* Launch the jump.  Grounded/coyote gating is the RUNTIME's job (it has
 * the timers); here we only refuse re-triggering mid-ascent.  Returns
 * whether the jump actually fired so the caller doesn't consume buffers
 * or pulse animation triggers on a refusal. */
bool jce_bullet_character_jump(JceBulletWorld *bw, uint32_t idx)
{
    if (!bw || idx >= bw->char_capacity || !bw->char_alive[idx]) return false;
    btRigidBody *b = bw->char_bodies[idx];
    if (!b) return false;
    JceBulletCharFeel *f = &bw->char_feel[idx];
    if (f->jumping) return false;   /* already mid-jump */
    btVector3 v = b->getLinearVelocity();
    v.setY(static_cast<btScalar>(bw->char_jump[idx]));
    b->setLinearVelocity(v);
    b->activate();
    f->jumping  = true;
    f->grounded = false;
    return true;
}

void jce_bullet_character_get_position(JceBulletWorld *bw, uint32_t idx,
                                        jce_vec3 *pos)
{
    if (!bw || idx >= bw->char_capacity || !bw->char_alive[idx] || !pos) return;
    if (!bw->char_bodies[idx]) return;
    *pos = from_bt_v3(bw->char_bodies[idx]->getWorldTransform().getOrigin());
}

/* Teleport the capsule CENTER to `pos` (clears momentum). */
void jce_bullet_character_set_position(JceBulletWorld *bw, uint32_t idx,
                                        jce_vec3 pos)
{
    if (!bw || idx >= bw->char_capacity || !bw->char_alive[idx]) return;
    btRigidBody *b = bw->char_bodies[idx];
    if (!b) return;
    btTransform xf = b->getWorldTransform();
    xf.setOrigin(to_bt(pos));
    b->setWorldTransform(xf);
    b->setLinearVelocity(btVector3(0, 0, 0));
    b->setInterpolationWorldTransform(xf);
    b->setInterpolationLinearVelocity(btVector3(0, 0, 0));
    b->activate();
    /* Teleport invalidates the cached ground state and any in-flight jump. */
    bw->char_feel[idx].probe_valid = false;
    bw->char_feel[idx].jumping     = false;
}

bool jce_bullet_character_is_grounded(JceBulletWorld *bw, uint32_t idx)
{
    if (!bw || idx >= bw->char_capacity || !bw->char_alive[idx]) return false;
    if (!bw->char_bodies[idx]) return false;
    JceBulletCharFeel *f = &bw->char_feel[idx];
    /* move() refreshes the probe every fixed tick; fall back to a fresh
     * probe only for queries before the first move (e.g. spawn frame). */
    if (!f->probe_valid) return char_ground_probe(bw, idx);
    return f->grounded;
}

void jce_bullet_character_get_velocity(JceBulletWorld *bw, uint32_t idx,
                                        jce_vec3 *out_vel)
{
    if (!out_vel) return;
    *out_vel = jce_v3(0.0f, 0.0f, 0.0f);
    if (!bw || idx >= bw->char_capacity || !bw->char_alive[idx]) return;
    btRigidBody *b = bw->char_bodies[idx];
    if (!b) return;
    *out_vel = from_bt_v3(b->getLinearVelocity());
}

void jce_bullet_character_cut_jump(JceBulletWorld *bw, uint32_t idx,
                                    float factor)
{
    if (!bw || idx >= bw->char_capacity || !bw->char_alive[idx]) return;
    btRigidBody *b = bw->char_bodies[idx];
    if (!b) return;
    if (factor < 0.0f) factor = 0.0f;
    if (factor > 1.0f) factor = 1.0f;
    btVector3 v = b->getLinearVelocity();
    if (v.y() > btScalar(0.0)) {
        v.setY(v.y() * btScalar(factor));
        b->setLinearVelocity(v);
    }
}
