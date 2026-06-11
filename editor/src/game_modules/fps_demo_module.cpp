/*
 * fps_demo_module.cpp  Editor-side built-in GameModule:
 *   "FPS Demo (Built-in)" — proves the editor's Play loop actually
 *   ticks game logic.  Owns its own physics world + character, no
 *   ECS-physics bridge required.
 *
 *   Behaviour while Playing:
 *     • Hidden ground plane at y=0
 *     • Capsule character at world origin (eye height 1.7m)
 *     • WASD               -> walk (camera-yaw-relative)
 *     • Space              -> jump
 *     • LeftShift          -> slow walk (×0.4 speed)
 *     • LeftCtrl           -> sprint  (×3 speed)
 *     • Mouse motion       -> yaw / pitch (relative-mouse mode)
 *
 *   The editor's game-view camera is driven from the character's
 *   position each frame, so this module works with whatever scene
 *   the user has loaded — the rendered view becomes "what a player
 *   standing in the scene would see".
 */

#include "scene/jce_editor_game_render.h"

extern "C" {
#include <jce/runtime/jce_game_module.h>
#include <jce/middleware/physics/jce_physics.h>
#include <jce/renderer/jce_camera.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_math.h>
}

#include <jce/tools/jce_imgui.hpp>
#include <cmath>

#define LOG_TAG "fps_demo"

namespace {

struct FpsDemoState {
    JcePhysicsWorld    *world      = nullptr;
    JceBodyHandle       ground     = { UINT32_MAX };
    JceBodyHandle       boxes[4]   = { { UINT32_MAX }, { UINT32_MAX },
                                        { UINT32_MAX }, { UINT32_MAX } };
    JceCharacterHandle  character  = { UINT32_MAX };
    float               yaw        = 0.0f;
    float               pitch      = 0.0f;
    bool                inited     = false;
};

FpsDemoState s;

constexpr float EYE_HEIGHT       = 1.6f;
constexpr float CHAR_RADIUS      = 0.35f;
constexpr float CHAR_HEIGHT      = 1.8f;
constexpr float WALK_SPEED       = 4.0f;
constexpr float SPRINT_MULT      = 3.0f;
constexpr float SLOW_MULT        = 0.4f;
constexpr float MOUSE_SENSITIVITY = 0.0025f;
constexpr float PITCH_LIMIT      = 1.55f;   /* ~89° */

bool fps_init(const JceServices *svc, void *ud)
{
    (void)svc; (void)ud;
    if (s.inited) return true;

    JcePhysicsWorldDesc desc = {};
    desc.gravity        = jce_v3(0.0f, -9.81f, 0.0f);
    desc.max_bodies     = 256;
    desc.fixed_timestep = 1.0f / 60.0f;
    desc.max_sub_steps  = 4;
    desc.split_impulse  = -1; /* leave Bullet default (ON) */
    s.world = jce_physics_create(&desc);
    if (!s.world) {
        LOG_WARN(LOG_TAG, "[init] physics world creation failed");
        return false;
    }

    /* Static ground plane (large box at y = -0.5, top surface at y = 0). */
    JceBodyDesc ground = {};
    ground.type         = JCE_BODY_STATIC;
    ground.shape        = JCE_SHAPE_BOX;
    ground.position     = jce_v3(0.0f, -0.5f, 0.0f);
    ground.rotation     = jce_q_identity();
    ground.half_extents = jce_v3(50.0f, 0.5f, 50.0f);
    ground.friction     = 0.8f;
    s.ground = jce_physics_body_create(s.world, &ground);

    /* A few obstacles so the player can collide with something. */
    const jce_vec3 box_positions[4] = {
        jce_v3( 3.0f, 0.5f,  0.0f),
        jce_v3(-3.0f, 0.5f,  0.0f),
        jce_v3( 0.0f, 0.5f,  3.0f),
        jce_v3( 0.0f, 0.5f, -3.0f),
    };
    for (int i = 0; i < 4; ++i) {
        JceBodyDesc b = {};
        b.type         = JCE_BODY_STATIC;
        b.shape        = JCE_SHAPE_BOX;
        b.position     = box_positions[i];
        b.rotation     = jce_q_identity();
        b.half_extents = jce_v3(0.5f, 0.5f, 0.5f);
        b.friction     = 0.5f;
        s.boxes[i] = jce_physics_body_create(s.world, &b);
    }

    /* Capsule character. */
    JceCharacterDesc cd = {};
    cd.position      = jce_v3(0.0f, 2.0f, 4.0f);
    cd.radius        = CHAR_RADIUS;
    cd.height        = CHAR_HEIGHT;
    cd.step_height   = 0.35f;
    cd.max_slope_deg = 50.0f;
    cd.gravity       = 9.81f;
    cd.jump_speed    = 5.0f;
    s.character = jce_physics_character_create(s.world, &cd);

    /* Initial yaw faces -Z so the player looks towards the obstacles. */
    s.yaw   = 0.0f;
    s.pitch = 0.0f;
    s.inited = true;

    LOG_INFO(LOG_TAG,
             "[init] FPS Demo ready — WASD/Space/Shift/Ctrl, mouse-look");
    return true;
}

void fps_exit(void *ud)
{
    (void)ud;
    if (!s.inited) return;
    if (s.world) {
        if (jce_character_valid(s.character))
            jce_physics_character_destroy(s.world, s.character);
        for (int i = 0; i < 4; ++i)
            if (jce_body_valid(s.boxes[i]))
                jce_physics_body_destroy(s.world, s.boxes[i]);
        if (jce_body_valid(s.ground))
            jce_physics_body_destroy(s.world, s.ground);
        jce_physics_destroy(s.world);
    }
    s = FpsDemoState{};
    LOG_INFO(LOG_TAG, "[exit] FPS Demo stopped");
}

void fps_update(float dt, void *ud)
{
    (void)ud;
    if (!s.inited || !s.world) return;
    if (dt <= 0.0f || dt > 0.1f) dt = 1.0f / 60.0f;

    bool captured = jce_editor_game_render_is_mouse_captured();
    ImGuiIO &io = ImGui::GetIO();

    /* Mouse-look only when the game viewport has captured the cursor. */
    if (captured) {
        s.yaw   -= io.MouseDelta.x * MOUSE_SENSITIVITY;
        s.pitch += -io.MouseDelta.y * MOUSE_SENSITIVITY;
        if (s.pitch >  PITCH_LIMIT) s.pitch =  PITCH_LIMIT;
        if (s.pitch < -PITCH_LIMIT) s.pitch = -PITCH_LIMIT;
    }

    /* Walk direction in world space, derived from yaw. */
    float fwd_x =  std::sin(s.yaw);
    float fwd_z = -std::cos(s.yaw);
    float right_x =  std::cos(s.yaw);
    float right_z =  std::sin(s.yaw);

    float wx = 0.0f, wz = 0.0f;
    if (captured) {
        if (ImGui::IsKeyDown(ImGuiKey_W)) { wx += fwd_x;   wz += fwd_z;   }
        if (ImGui::IsKeyDown(ImGuiKey_S)) { wx -= fwd_x;   wz -= fwd_z;   }
        if (ImGui::IsKeyDown(ImGuiKey_D)) { wx += right_x; wz += right_z; }
        if (ImGui::IsKeyDown(ImGuiKey_A)) { wx -= right_x; wz -= right_z; }
    }
    float wlen = std::sqrt(wx*wx + wz*wz);
    if (wlen > 0.0001f) { wx /= wlen; wz /= wlen; }

    float speed = WALK_SPEED;
    if (captured && (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) ||
                     ImGui::IsKeyDown(ImGuiKey_RightCtrl)))
        speed *= SPRINT_MULT;
    else if (captured && (ImGui::IsKeyDown(ImGuiKey_LeftShift) ||
                          ImGui::IsKeyDown(ImGuiKey_RightShift)))
        speed *= SLOW_MULT;

    jce_vec3 walk = jce_v3(wx * speed, 0.0f, wz * speed);
    jce_physics_character_move(s.world, s.character, walk, dt);

    /* jump() no longer gates on ground contact (the scene runtime owns
     * coyote-time/buffer forgiveness) — gate here to keep no-air-jumps. */
    if (captured && ImGui::IsKeyPressed(ImGuiKey_Space, false) &&
        jce_physics_character_is_grounded(s.world, s.character))
        jce_physics_character_jump(s.world, s.character);

    jce_physics_step(s.world, dt);

    /* Read back character position and drive the editor's game-view
     * camera so the user sees through the player's eyes. */
    jce_vec3 cpos = jce_v3(0,0,0);
    jce_physics_character_get_position(s.world, s.character, &cpos);

    JceCamera *cam = jce_editor_game_render_get_camera();
    if (cam) {
        jce_vec3 eye = jce_v3(cpos.x, cpos.y + EYE_HEIGHT, cpos.z);
        jce_vec3 look_dir = jce_v3(
            std::sin(s.yaw) * std::cos(s.pitch),
            std::sin(s.pitch),
           -std::cos(s.yaw) * std::cos(s.pitch));
        jce_vec3 target = jce_v3(eye.x + look_dir.x,
                                 eye.y + look_dir.y,
                                 eye.z + look_dir.z);
        jce_camera_set_position(cam, eye);
        jce_camera_set_target(cam, target);
    }
}

void fps_draw(const JceServices *svc, void *ud) { (void)svc; (void)ud; }
void fps_on_event(const JceEvent *ev, void *ud) { (void)ev; (void)ud; }
bool fps_should_quit(void *ud) { (void)ud; return false; }

JceGameModule g_fps_demo_desc = {
    "FPS Demo (Built-in)",
    fps_init,
    fps_exit,
    fps_update,
    fps_draw,
    fps_on_event,
    nullptr,
    fps_should_quit,
    nullptr,
    false,
    0, 0,
};

} /* anon namespace */

extern "C" void jce_editor_register_builtin_modules(void);
void jce_editor_register_builtin_modules(void)
{
    /* The previous standalone "FPS Demo" module has been replaced by
     * the editor's ECS-physics Play path: scenes that contain a
     * CharacterController + RigidBody/Colliders now drive a real
     * character through `jce_editor_play.cpp`.  See
     * `caged_kingdom/resources/assets/scenes/fps_demo.scene.json`.
     * Left as a no-op so existing call sites still link. */
}
