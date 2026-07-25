/*
 * jce_net_input_command.c — CLIENT->SERVER input command channel (F12 slice).
 *
 * See jce_net_input_command.h for the design contract.  This TU is
 * intentionally PURE: a fixed-layout little-endian codec plus a bounded
 * per-client latest-input store.  It has NO transport / RPC / runtime /
 * physics dependency, so the unit test links jce_core + jce_net only.
 *
 * Wire layout (little-endian, JCE_INPUT_COMMAND_WIRE_SIZE = 20 bytes):
 *
 *   [ tick        : u32 ]
 *   [ walk_x      : f32 ]   (bit-copied then byte-ordered LE)
 *   [ walk_z      : f32 ]
 *   [ speed_mult  : f32 ]
 *   [ jump        : u8  ]
 *   [ sprint      : u8  ]
 *   [ pad         : u8  ]   (must be 0; reserved)
 *   [ pad         : u8  ]
 *
 * The struct's own padding never travels — encode/decode read+write each
 * field explicitly, so the round-trip is exact across compilers / ABIs.
 */

#include <jce/middleware/net/jce_net_input_command.h>

#include "jce_net_bytes.h"

#include <string.h>

/* ================================================================== */
/* PURE codec                                                          */
/* ================================================================== */

uint32_t jce_input_command_encode(const JceInputCommand *cmd,
                                  void *dst, uint32_t cap)
{
    if (!cmd || !dst || cap < JCE_INPUT_COMMAND_WIRE_SIZE)
        return 0u;

    uint8_t *p = (uint8_t *)dst;
    jce_net_put_u32(p +  0, cmd->tick);
    jce_net_put_f32(p +  4, cmd->walk_x);
    jce_net_put_f32(p +  8, cmd->walk_z);
    jce_net_put_f32(p + 12, cmd->speed_mult);
    p[16] = cmd->jump   ? 1u : 0u;   /* normalise to 0/1 on the wire */
    p[17] = cmd->sprint ? 1u : 0u;
    p[18] = 0u;                      /* reserved pad */
    p[19] = 0u;
    return JCE_INPUT_COMMAND_WIRE_SIZE;
}

uint32_t jce_input_command_decode(const void *src, uint32_t size,
                                  JceInputCommand *out)
{
    if (!src || !out || size < JCE_INPUT_COMMAND_WIRE_SIZE)
        return 0u;

    const uint8_t *p = (const uint8_t *)src;
    memset(out, 0, sizeof *out);
    out->tick       = jce_net_get_u32(p +  0);
    out->walk_x     = jce_net_get_f32(p +  4);
    out->walk_z     = jce_net_get_f32(p +  8);
    out->speed_mult = jce_net_get_f32(p + 12);
    out->jump       = p[16] ? 1u : 0u;
    out->sprint     = p[17] ? 1u : 0u;
    /* p[18], p[19] are reserved padding — ignored. */
    return JCE_INPUT_COMMAND_WIRE_SIZE;
}

/* ================================================================== */
/* Per-client latest-input store                                       */
/* ================================================================== */

typedef struct InputSlot {
    bool            used;
    uint32_t        client_id;
    JceInputCommand cmd;
} InputSlot;

static InputSlot g_store[JCE_INPUT_COMMAND_MAX_CLIENTS];

static InputSlot *store_find(uint32_t client_id)
{
    for (uint32_t i = 0; i < JCE_INPUT_COMMAND_MAX_CLIENTS; ++i)
        if (g_store[i].used && g_store[i].client_id == client_id)
            return &g_store[i];
    return NULL;
}

static InputSlot *store_find_free(void)
{
    for (uint32_t i = 0; i < JCE_INPUT_COMMAND_MAX_CLIENTS; ++i)
        if (!g_store[i].used)
            return &g_store[i];
    return NULL;
}

void jce_input_command_store(uint32_t client_id, const JceInputCommand *cmd)
{
    if (!cmd) return;

    InputSlot *slot = store_find(client_id);
    if (slot) {
        /* Latest-wins: ignore an out-of-order / duplicate older tick. */
        if (cmd->tick <= slot->cmd.tick)
            return;
        slot->cmd = *cmd;
        return;
    }

    /* New client id — claim a free slot.  Bounded: drop when full. */
    slot = store_find_free();
    if (!slot) return;
    slot->used      = true;
    slot->client_id = client_id;
    slot->cmd       = *cmd;
}

bool jce_input_command_get_latest(uint32_t client_id, JceInputCommand *out)
{
    InputSlot *slot = store_find(client_id);
    if (!slot) return false;
    if (out) *out = slot->cmd;
    return true;
}

void jce_input_command_store_reset(void)
{
    memset(g_store, 0, sizeof g_store);
}

/* ================================================================== */
/* Server-receive wrapper (decode + store)                             */
/* ================================================================== */

bool jce_input_command_server_receive(uint32_t client_id,
                                      const void *src, uint32_t size)
{
    JceInputCommand cmd;
    if (jce_input_command_decode(src, size, &cmd) == 0u)
        return false;
    jce_input_command_store(client_id, &cmd);
    return true;
}
