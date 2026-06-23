/*
 * jce_net_input_command.h — CLIENT->SERVER input command channel (F12 slice).
 *
 * The upstream half of authoritative networked movement: a client samples its
 * local player input each fixed tick, packs it into a fixed-size POD command,
 * and uploads it to the server.  The server stores the latest command per
 * client and replays it to drive that client's owned entity, producing the
 * authoritative transform that jce_net_transform broadcasts (and which the
 * client reconciles against via jce_net_prediction).
 *
 * This header is intentionally TRANSPORT-FREE.  It carries only:
 *
 *   - JceInputCommand : the fixed-size wire POD.
 *   - A PURE little-endian codec (encode / decode) — unit-testable in
 *     isolation, no RPC / runtime / physics dependency.
 *   - A bounded per-client latest-input STORE (server-side): latest-wins by
 *     tick, keyed by client id.
 *   - A thin server-receive wrapper (decode + store) so the RPC handler logic
 *     is directly exercisable headless.
 *
 * The actual RPC registration + send/receive wiring lives in the runtime
 * (jce_runtime.c), which depends on <jce/middleware/net/jce_rpc.h>.  Keeping
 * this module pure means a test can link jce_core + jce_net only.
 *
 * Layer: L4 (middleware/net).  Consumed via <jce/api_net.h>.
 */

#ifndef JCE_NET_INPUT_COMMAND_H
#define JCE_NET_INPUT_COMMAND_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* ================================================================== */
/* Wire POD                                                            */
/* ================================================================== */

/*
 * One client->server input sample.  Fixed-size POD; the codec writes a fixed
 * 20-byte little-endian layout (see jce_input_command_encode), so the struct's
 * own padding never travels the wire.
 *
 *   tick       : the client's fixed-clock tick at sample time (latest-wins).
 *   walk_x/z   : scene-space move DIRECTION (the same semantics as
 *                JceRuntimeInput.walk_x / walk_z — clamped to unit length on
 *                consume, speed comes from the CharacterController).
 *   speed_mult : extra gameplay multiplier (crouch / slow zones; 0 => 1).
 *   jump       : 0/1 — jump requested this tick (covers pressed-or-held; the
 *                server feeds a jump buffer like the local driver).
 *   sprint     : 0/1 — sprint held.
 */
typedef struct JceInputCommand {
    uint32_t tick;
    float    walk_x;
    float    walk_z;
    float    speed_mult;
    uint8_t  jump;
    uint8_t  sprint;
    uint8_t  _pad[2];     /* keep the struct 4-byte aligned + 20 bytes */
} JceInputCommand;

/* Exact number of bytes jce_input_command_encode writes / decode reads:
 * 4 (tick) + 4 (walk_x) + 4 (walk_z) + 4 (speed_mult) + 1 (jump) + 1 (sprint)
 * + 2 (pad). */
#define JCE_INPUT_COMMAND_WIRE_SIZE  ((uint32_t)20u)

/* ================================================================== */
/* PURE codec (no transport)                                          */
/* ================================================================== */

/*
 * Encode `cmd` into `dst` (little-endian, fixed layout).  Returns the number
 * of bytes written (JCE_INPUT_COMMAND_WIRE_SIZE) on success, or 0 when cmd /
 * dst is NULL or cap < JCE_INPUT_COMMAND_WIRE_SIZE.  Floats are bit-copied
 * (memcpy) then byte-ordered little-endian, so the round-trip is exact.
 */
JCE_API uint32_t JCE_CALL
jce_input_command_encode(const JceInputCommand *cmd, void *dst, uint32_t cap);

/*
 * Decode a command from `src` (`size` bytes).  Returns the number of bytes
 * read (JCE_INPUT_COMMAND_WIRE_SIZE) on success, or 0 when src / out is NULL
 * or size < JCE_INPUT_COMMAND_WIRE_SIZE (wrong-size rejection).
 */
JCE_API uint32_t JCE_CALL
jce_input_command_decode(const void *src, uint32_t size, JceInputCommand *out);

/* ================================================================== */
/* Per-client latest-input STORE (server-side)                        */
/* ================================================================== */

/*
 * Store the latest command for `client_id` (latest-wins by `cmd->tick`):
 * an arriving command whose tick is <= the stored tick for that client is
 * ignored.  Bounded to a small fixed number of distinct clients
 * (JCE_INPUT_COMMAND_MAX_CLIENTS); once full, commands from a NEW client id
 * are dropped (existing clients keep updating).  No-op on NULL cmd.
 */
JCE_API void JCE_CALL
jce_input_command_store(uint32_t client_id, const JceInputCommand *cmd);

/* Fetch the latest stored command for `client_id` into `out`.  Returns true
 * when a command is stored for that client, false otherwise (out untouched
 * on false). */
JCE_API bool JCE_CALL
jce_input_command_get_latest(uint32_t client_id, JceInputCommand *out);

/* Clear the entire store (tests + scene teardown). */
JCE_API void JCE_CALL
jce_input_command_store_reset(void);

/* Maximum number of distinct clients the store tracks. */
#define JCE_INPUT_COMMAND_MAX_CLIENTS  32u

/* ================================================================== */
/* Server-receive wrapper (decode + store)                            */
/* ================================================================== */

/*
 * The exact decode+store logic the server RPC handler runs, exposed as a thin
 * directly-callable seam for headless tests (the live RPC handler in the
 * runtime forwards to this).  Decodes `src` (`size` bytes) and, on success,
 * stores the command for `client_id` (latest-wins).  Returns true on a
 * successful decode+store, false when the bytes don't decode.
 */
JCE_API bool JCE_CALL
jce_input_command_server_receive(uint32_t client_id,
                                 const void *src, uint32_t size);

JCE_EXTERN_C_END

#endif /* JCE_NET_INPUT_COMMAND_H */
