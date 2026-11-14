/* One per-user policy for the existing Scene/Game live physics overlay.
 * Reuse editor-session.json's generic UI state owner; never change simulation. */
#ifndef JCE_EDITOR_PHYSICS_DEBUG_SETTINGS_H
#define JCE_EDITOR_PHYSICS_DEBUG_SETTINGS_H
#include "../ui/jce_editor_ui_state.h"
#include <jce/application/jce_runtime.h>
#include <cmath>

namespace jce_editor_physics_debug_settings {
constexpr float default_radius_m = 30.0f, maximum_radius_m = 10000.0f;
constexpr const char *radius_key = "physics_debug.radius_m";
constexpr const char *frustum_key = "physics_debug.frustum";
constexpr const char *center_key = "physics_debug.center";
enum Center { CurrentCamera = 0, Player = 1 };
inline float sanitize_radius(float value) {
    return std::isfinite(value) ? std::fmin(maximum_radius_m, std::fmax(0.0f, value))
                                : default_radius_m;
}
inline float radius() {
    return sanitize_radius(jce_editor_ui_state_load_float(
        radius_key, default_radius_m, 0.0f, maximum_radius_m));
}
inline void set_radius(float value) {
    jce_editor_ui_state_save_float(radius_key, sanitize_radius(value));
}
inline bool frustum() {
    return jce_editor_ui_state_load_int(frustum_key, 0, 0, 1) != 0;
}
inline void set_frustum(bool value) {
    jce_editor_ui_state_save_int(frustum_key, value ? 1 : 0);
}
inline Center center() {
    return jce_editor_ui_state_load_int(center_key, CurrentCamera, CurrentCamera, Player) == Player
        ? Player : CurrentCamera;
}
inline void set_center(Center value) {
    jce_editor_ui_state_save_int(center_key, value == Player ? Player : CurrentCamera);
}
inline bool player_center(const JceRuntime *runtime, jce_vec3 &position) {
    return center() == Player && jce_runtime_get_player_position(runtime, &position) &&
        std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z);
}
} // namespace jce_editor_physics_debug_settings
#endif
