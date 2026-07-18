/*
 * jce_editor_game_input_bridge.h  Game View input ownership and step order.
 */

#ifndef JCE_EDITOR_GAME_INPUT_BRIDGE_H
#define JCE_EDITOR_GAME_INPUT_BRIDGE_H

extern "C" {
#include <jce/application/jce_runtime.h>
#include <jce/os/platform/jce_window_event.h>
}

#include <stdbool.h>
#include <stdint.h>

enum JceEditorGameInputOwner {
    JCE_EDITOR_GAME_INPUT_NONE = 0,
    JCE_EDITOR_GAME_INPUT_RUNTIME,
    JCE_EDITOR_GAME_INPUT_FREE_FLY,
};

struct JceEditorGameInputViewport {
    float x;
    float y;
    float width;
    float height;
    bool visible;
    bool hovered;
    bool captured;
    JceEditorGameInputOwner owner;
};

struct JceEditorGameInputSample {
    float dx;
    float dy;
    float wheel;
    uint32_t buttons;
    JceEditorGameInputOwner owner;
};

struct JceEditorGameInputBridge {
    JceEditorGameInputViewport viewport;
    float dx;
    float dy;
    float wheel;
    uint32_t buttons;
};

struct JceEditorGameInputCommitOps {
    void (*set_pointer)(void *ud, float dx, float dy, float wheel,
                        uint32_t buttons);
    void (*set_touches)(void *ud, const JceRuntimeTouch *touches, int count);
    void (*set_actions)(void *ud, const JceInputActions *actions);
    void (*step)(void *ud, float dt);
};

void jce_editor_game_input_bridge_reset(JceEditorGameInputBridge *bridge);
void jce_editor_game_input_bridge_publish(
    JceEditorGameInputBridge *bridge,
    const JceEditorGameInputViewport *viewport);
void jce_editor_game_input_bridge_handle_event(
    JceEditorGameInputBridge *bridge, const JceEvent *event);
JceEditorGameInputSample jce_editor_game_input_bridge_consume(
    JceEditorGameInputBridge *bridge);

bool jce_editor_game_input_bridge_commit(
    JceEditorGameInputBridge *bridge,
    const JceRuntimeTouch *touches, int touch_count,
    const JceInputActions *actions, float dt,
    const JceEditorGameInputCommitOps *ops, void *ud);

JceEditorGameInputBridge *jce_editor_game_input_bridge_shared(void);

#endif /* JCE_EDITOR_GAME_INPUT_BRIDGE_H */

