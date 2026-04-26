/*
 * jce_net_proto.h  Protobuf encode/decode bridge (internal).
 *
 * Provides a C-callable layer over protobuf-generated C++ code
 * for encoding and decoding structured network messages.
 */

#ifndef JCE_NET_PROTO_H
#define JCE_NET_PROTO_H

#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Message types ──────────────────────────────────────────────── */

typedef enum {
    JCE_NET_MSG_PLAYER_ACTION = 0,
    JCE_NET_MSG_GAME_STATE    = 1,
    JCE_NET_MSG_CHAT          = 2,
    JCE_NET_MSG_PING          = 3
} JceNetMsgType;

/* ── Flat C structs mirroring the protobuf messages ─────────────── */

typedef struct {
    uint64_t entity_id;
    uint32_t action_id;
    jce_vec3 position;
    jce_vec3 direction;
    float    value;
} JceNetPlayerAction;

typedef struct {
    uint64_t entity_id;
    jce_vec3 position;
    jce_quat rotation;
    jce_vec3 velocity;
    uint32_t state;
} JceNetEntitySnapshot;

typedef struct {
    uint32_t               tick;
    JceNetEntitySnapshot  *entities;
    uint32_t               entity_count;
} JceNetGameState;

typedef struct {
    uint64_t sender_id;
    const char *text;
    uint32_t   channel;
} JceNetChatMessage;

typedef struct {
    uint64_t client_time_us;
    uint64_t server_time_us;
} JceNetPing;

/* ── Envelope ───────────────────────────────────────────────────── */

typedef struct {
    uint32_t      sequence;
    uint64_t      timestamp_us;
    JceNetMsgType type;
    union {
        JceNetPlayerAction player_action;
        JceNetGameState    game_state;
        JceNetChatMessage  chat;
        JceNetPing         ping;
    };
} JceNetEnvelope;

/* ── Encode / Decode ────────────────────────────────────────────── */

/*
 * Encode an envelope into a byte buffer.
 * Returns the number of bytes written, or 0 on failure.
 * out_buf must be at least out_buf_size bytes.
 */
uint32_t jce_net_proto_encode(const JceNetEnvelope *env,
                              uint8_t *out_buf, uint32_t out_buf_size);

/*
 * Decode a byte buffer into an envelope.
 * Returns true on success.
 *
 * NOTE: For JCE_NET_MSG_GAME_STATE, the caller must free
 * env->game_state.entities with jce_net_proto_free_state() after use.
 * For JCE_NET_MSG_CHAT, env->chat.text points to an internal buffer
 * that is valid until the next decode call.
 */
bool jce_net_proto_decode(const uint8_t *data, uint32_t size,
                          JceNetEnvelope *out_env);

/* Free resources allocated by decode (specifically game_state.entities). */
void jce_net_proto_free_state(JceNetGameState *gs);

#ifdef __cplusplus
}
#endif

#endif /* JCE_NET_PROTO_H */
