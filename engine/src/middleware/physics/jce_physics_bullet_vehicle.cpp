/*
 * jce_physics_bullet_vehicle.cpp — the raycast vehicle controller.
 *
 * Split out of jce_physics_bullet.cpp, which was 3,164 lines: past the
 * 3,000-line cap and frozen at the size gate's baseline, so every addition
 * had to be paid for by a removal.
 *
 * This block was chosen because its interface is the narrowest in the file:
 * it touches only bw->vehicle_* members and calls NOTHING else defined in
 * the original TU.  The character controller is twice the size and would
 * have dragged the body and shape tables across with it.
 */

#include "jce_physics_bullet_internal.hpp"

/* ================================================================== */
/* Vehicle controller                                                  */
/* ================================================================== */

uint32_t jce_bullet_vehicle_create(JceBulletWorld *bw,
                                    jce_vec3 pos, jce_quat rot,
                                    jce_vec3 chassis_half_ext,
                                    float chassis_mass,
                                    float max_engine_force,
                                    float max_brake_force,
                                    float max_steering_rad,
                                    uint32_t col_group, uint32_t col_mask)
{
    if (!bw) return UINT32_MAX;

    uint32_t idx = UINT32_MAX;
    for (uint32_t n = 0; n < bw->vehicle_capacity; ++n) {
        uint32_t i = (bw->vehicle_alloc_cursor + n) % bw->vehicle_capacity;
        if (!bw->vehicle_alive[i]) { idx = i; break; }
    }
    if (idx == UINT32_MAX) return UINT32_MAX;
    bw->vehicle_alloc_cursor = (idx + 1u) % bw->vehicle_capacity;

    /* Chassis collision shape (box). */
    btCollisionShape *chassis_shape = new btBoxShape(to_bt(chassis_half_ext));
    btVector3 inertia(0, 0, 0);
    if (chassis_mass > 0.0f) chassis_shape->calculateLocalInertia(chassis_mass, inertia);

    btTransform xf;
    xf.setIdentity();
    xf.setOrigin(to_bt(pos));
    xf.setRotation(to_bt_q(rot));
    auto *motion = new btDefaultMotionState(xf);
    btRigidBody::btRigidBodyConstructionInfo ci(chassis_mass, motion, chassis_shape, inertia);
    auto *chassis = new btRigidBody(ci);

    /* Chassis must never sleep — wheels rely on continuous integration. */
    chassis->setActivationState(DISABLE_DEACTIVATION);
    chassis->setUserPointer(reinterpret_cast<void *>(static_cast<uintptr_t>(idx)));
    /* Mark as a vehicle chassis so the contact dispatch skips it (its
     * user-pointer is a vehicle index, NOT a packed body handle). */
    chassis->setUserIndex(JCE_BULLET_VEHICLE_CHASSIS_USERINDEX);
    bw->world->addRigidBody(chassis,
        static_cast<int>(col_group), static_cast<int>(col_mask));

    /* Raycaster + vehicle. */
    auto *raycaster = new btDefaultVehicleRaycaster(bw->world);
    btRaycastVehicle::btVehicleTuning tuning;
    auto *vehicle = new btRaycastVehicle(tuning, chassis, raycaster);

    /* Bullet vehicle convention: forward = Z (axis index 2), up = Y (1), right = X (0). */
    vehicle->setCoordinateSystem(0, 1, 2);

    bw->world->addVehicle(vehicle);

    bw->vehicles[idx]                = vehicle;
    bw->vehicle_raycasters[idx]      = raycaster;
    bw->vehicle_chassis[idx]         = chassis;
    bw->vehicle_chassis_shapes[idx]  = chassis_shape;
    bw->vehicle_alive[idx]           = true;
    bw->vehicle_max_engine[idx]      = max_engine_force;
    bw->vehicle_max_brake[idx]       = max_brake_force;
    bw->vehicle_max_steer[idx]       = max_steering_rad;
    bw->vehicle_count++;
    return idx;
}

void jce_bullet_vehicle_destroy(JceBulletWorld *bw, uint32_t idx)
{
    if (!bw || idx >= bw->vehicle_capacity || !bw->vehicle_alive[idx]) return;
    if (bw->vehicles[idx]) {
        bw->world->removeVehicle(bw->vehicles[idx]);
        delete bw->vehicles[idx];
        bw->vehicles[idx] = nullptr;
    }
    delete bw->vehicle_raycasters[idx];
    bw->vehicle_raycasters[idx] = nullptr;
    if (bw->vehicle_chassis[idx]) {
        bw->world->removeRigidBody(bw->vehicle_chassis[idx]);
        delete bw->vehicle_chassis[idx]->getMotionState();
        delete bw->vehicle_chassis[idx];
        bw->vehicle_chassis[idx] = nullptr;
    }
    delete bw->vehicle_chassis_shapes[idx];
    bw->vehicle_chassis_shapes[idx] = nullptr;
    bw->vehicle_alive[idx] = false;
    bw->vehicle_count--;
}

uint32_t jce_bullet_vehicle_add_wheel(JceBulletWorld *bw, uint32_t idx,
                                       jce_vec3 connection,
                                       jce_vec3 wheel_dir,
                                       jce_vec3 wheel_axle,
                                       float suspension_rest_len,
                                       float wheel_radius,
                                       bool is_front,
                                       float susp_stiffness,
                                       float susp_damping,
                                       float susp_compression,
                                       float friction_slip,
                                       float roll_influence)
{
    if (!bw || idx >= bw->vehicle_capacity || !bw->vehicle_alive[idx])
        return UINT32_MAX;
    btRaycastVehicle *vehicle = bw->vehicles[idx];
    btRaycastVehicle::btVehicleTuning tuning;
    btWheelInfo &wi = vehicle->addWheel(to_bt(connection), to_bt(wheel_dir),
                                          to_bt(wheel_axle),
                                          static_cast<btScalar>(suspension_rest_len),
                                          static_cast<btScalar>(wheel_radius),
                                          tuning, is_front);
    if (susp_stiffness   > 0) wi.m_suspensionStiffness   = susp_stiffness;
    if (susp_damping     > 0) wi.m_wheelsDampingRelaxation = susp_damping;
    if (susp_compression > 0) wi.m_wheelsDampingCompression = susp_compression;
    if (friction_slip    > 0) wi.m_frictionSlip           = friction_slip;
    wi.m_rollInfluence   = roll_influence;
    return static_cast<uint32_t>(vehicle->getNumWheels()) - 1u;
}

void jce_bullet_vehicle_set_input(JceBulletWorld *bw, uint32_t idx,
                                   float throttle, float brake, float steer)
{
    if (!bw || idx >= bw->vehicle_capacity || !bw->vehicle_alive[idx]) return;
    btRaycastVehicle *vehicle = bw->vehicles[idx];

    /* Clamp inputs. */
    if (throttle >  1.0f) throttle =  1.0f;
    if (throttle < -1.0f) throttle = -1.0f;
    if (brake    <  0.0f) brake    =  0.0f;
    if (brake    >  1.0f) brake    =  1.0f;
    if (steer    >  1.0f) steer    =  1.0f;
    if (steer    < -1.0f) steer    = -1.0f;

    float engine_force = throttle * bw->vehicle_max_engine[idx];
    float brake_force  = brake    * bw->vehicle_max_brake[idx];
    float steer_rad    = steer    * bw->vehicle_max_steer[idx];

    /* Wake the chassis whenever the user is driving. */
    if (bw->vehicle_chassis[idx])
        bw->vehicle_chassis[idx]->activate(true);

    int n = vehicle->getNumWheels();
    for (int i = 0; i < n; ++i) {
        const btWheelInfo &wi = vehicle->getWheelInfo(i);
        /* Drive: rear-wheel-drive on non-steering wheels, brake everywhere,
         * steer only on front wheels.  Sane GTA-style default. */
        if (wi.m_bIsFrontWheel) {
            vehicle->applyEngineForce(0.0f, i);
            vehicle->setSteeringValue(steer_rad, i);
        } else {
            vehicle->applyEngineForce(engine_force, i);
            vehicle->setSteeringValue(0.0f, i);
        }
        vehicle->setBrake(brake_force, i);
    }
}

/* Per-wheel drive trim, ADDED on top of whatever set_input just applied.
 *
 * Bullet has taken per-wheel arguments all along -- the loop above calls
 * applyEngineForce(f, i) / setBrake(f, i) / setSteeringValue(rad, i) for each
 * wheel -- but nothing above the wrapper could reach one wheel, so
 * WheelCollider.motor_torque / .brake_torque / .steer_angle_deg had no way to
 * mean anything.
 *
 * ADDITIVE, not authoritative: set_input drives every wheel from the vehicle-
 * level throttle, and a wheel whose authored trim is 0 (the default, and what
 * every existing scene has) must come out exactly as before.  Brake takes the
 * MAX rather than the sum -- two brake sources are not twice the brake.
 */
void jce_bullet_vehicle_add_wheel_input(JceBulletWorld *bw, uint32_t idx,
                                        uint32_t wheel, float engine_force,
                                        float brake_force, float steer_rad)
{
    if (!bw || idx >= bw->vehicle_capacity || !bw->vehicle_alive[idx]) return;
    btRaycastVehicle *vehicle = bw->vehicles[idx];
    if ((int)wheel >= vehicle->getNumWheels()) return;

    const btWheelInfo &wi = vehicle->getWheelInfo((int)wheel);
    if (engine_force != 0.0f || brake_force != 0.0f || steer_rad != 0.0f)
        if (bw->vehicle_chassis[idx]) bw->vehicle_chassis[idx]->activate(true);

    vehicle->applyEngineForce(wi.m_engineForce + engine_force, (int)wheel);
    vehicle->setSteeringValue(wi.m_steering + steer_rad, (int)wheel);
    if (brake_force > wi.m_brake)
        vehicle->setBrake(brake_force, (int)wheel);
}

void jce_bullet_vehicle_get_chassis_transform(JceBulletWorld *bw, uint32_t idx,
                                                jce_vec3 *pos, jce_quat *rot)
{
    if (!bw || idx >= bw->vehicle_capacity || !bw->vehicle_alive[idx]) return;
    btTransform xf;
    bw->vehicle_chassis[idx]->getMotionState()->getWorldTransform(xf);
    if (pos) *pos = from_bt_v3(xf.getOrigin());
    if (rot) *rot = from_bt_q(xf.getRotation());
}

void jce_bullet_vehicle_get_wheel_transform(JceBulletWorld *bw, uint32_t idx,
                                              uint32_t wheel,
                                              jce_vec3 *pos, jce_quat *rot)
{
    if (!bw || idx >= bw->vehicle_capacity || !bw->vehicle_alive[idx]) return;
    btRaycastVehicle *vehicle = bw->vehicles[idx];
    if (static_cast<int>(wheel) >= vehicle->getNumWheels()) return;
    /* Update interpolated wheel transform from current suspension state. */
    vehicle->updateWheelTransform(static_cast<int>(wheel), true);
    const btTransform &xf = vehicle->getWheelInfo(wheel).m_worldTransform;
    if (pos) *pos = from_bt_v3(xf.getOrigin());
    if (rot) *rot = from_bt_q(xf.getRotation());
}

float jce_bullet_vehicle_get_speed(JceBulletWorld *bw, uint32_t idx)
{
    if (!bw || idx >= bw->vehicle_capacity || !bw->vehicle_alive[idx]) return 0.0f;
    /* Bullet returns km/h — convert to m/s for SI consistency. */
    return static_cast<float>(bw->vehicles[idx]->getCurrentSpeedKmHour()) * (1.0f / 3.6f);
}
