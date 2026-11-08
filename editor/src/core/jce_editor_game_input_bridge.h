/*
 * jce_editor_game_input_bridge.h  Game View input ownership and step order.
 */

#ifndef JCE_EDITOR_GAME_INPUT_BRIDGE_H
#define JCE_EDITOR_GAME_INPUT_BRIDGE_H

extern "C" {
#include <jce/application/jce_runtime.h>
#include <jce/os/platform/jce_input.h>
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
    float render_width;  /* zero: render at the displayed size */
    float render_height;
    bool visible;
    bool hovered;
    bool captured;
    bool focused;       /* current host-window focus observation */
    bool pointer_valid; /* real UI cursor snapshot, never a button/event */
    float pointer_x, pointer_y;
    JceEditorGameInputOwner owner;
};

struct JceEditorGameInputSample {
    float dx;
    float dy;
    float wheel;
    uint32_t buttons;
    JceEditorGameInputOwner owner;
    float x, y, width, height;
    bool synthetic; /* explicit virtual/replay stream, never physical input evidence */
    bool replayed;  /* the canonical JIRC owner applied this snapshot */
};

struct JceEditorGameVirtualPointer {
    float dx, dy, wheel, x, y;
    uint32_t buttons;
    bool active;
    bool replayed;
};

struct JceEditorGameInputBridge {
    JceEditorGameInputViewport viewport;
    float dx;
    float dy;
    float wheel;
    uint32_t buttons;
    float x, y;
    bool position_valid;
    JceEditorGameVirtualPointer virtual_pointer;
};

struct JceEditorGameInputCommitOps {
    void (*set_pointer)(void *ud, float dx, float dy, float wheel,
                        uint32_t buttons);
    void (*set_touches)(void *ud, const JceRuntimeTouch *touches, int count);
    void (*set_actions)(void *ud, const JceInputActions *actions);
    void (*step)(void *ud, float dt);
    void (*set_position)(void *ud, float x, float y, float width, float height);
};

void jce_editor_game_input_bridge_reset(JceEditorGameInputBridge *bridge);
void jce_editor_game_input_bridge_publish(
    JceEditorGameInputBridge *bridge,
    const JceEditorGameInputViewport *viewport);
void jce_editor_game_input_bridge_handle_event(
    JceEditorGameInputBridge *bridge, const JceEvent *event);
/* Explicit virtual pointer producer in host-window coordinates. Does not feed
 * ImGui, change OS focus/hover/capture, or relax native-event admission. A valid
 * cursor takes precedence until cleared; deltas drain per consume and buttons
 * persist. Hidden/non-runtime or changed display/render geometry cancels it.
 * An outside button-up cancels the stream without a click at an old position;
 * other invalid events leave the valid stream unchanged. Wheel needs a cursor. */
bool jce_editor_game_input_bridge_handle_virtual_event(
    JceEditorGameInputBridge *bridge, const JceEvent *event);
void jce_editor_game_input_bridge_clear_virtual(JceEditorGameInputBridge *bridge);
/* Call once per app-update before commit. Only the public current-snapshot
 * origin admits input; no environment probing, OS cursor or ImGui mutation.
 * Reuses the explicit pointer state/mapping and clears a retired replay source.
 * Existing consume drains deltas/wheel, including repeated commits this tick. */
void jce_editor_game_input_bridge_sync_replay(
    JceEditorGameInputBridge *bridge, const JceInput *input);

/* Read-only Canvas pointer sample in the current panel/FBO geometry. Does not
 * drain runtime deltas, wheel or held buttons; hidden/changed geometry refuses.
 * Native pointer/text/wheel admission is not changed by this explicit route. */
bool jce_editor_game_input_bridge_peek_virtual(
    const JceEditorGameInputBridge *bridge,
    const JceEditorGameInputViewport *current_viewport,
    JceEditorGameInputSample *out);
JceEditorGameInputSample jce_editor_game_input_bridge_consume(
    JceEditorGameInputBridge *bridge);

bool jce_editor_game_input_bridge_commit(
    JceEditorGameInputBridge *bridge,
    const JceRuntimeTouch *touches, int touch_count,
    const JceInputActions *actions, float dt,
    const JceEditorGameInputCommitOps *ops, void *ud);

JceEditorGameInputBridge *jce_editor_game_input_bridge_shared(void);

#endif /* JCE_EDITOR_GAME_INPUT_BRIDGE_H */
