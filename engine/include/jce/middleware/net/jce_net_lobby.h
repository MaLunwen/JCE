/*
 * jce_net_lobby.h  Lobby / matchmaking data layer.
 *
 * Standard pre-session UX for multiplayer games: a "lobby" is a
 * named room with N slots, each slot holding a peer + their ready
 * state.  Host owns authority; clients query state and toggle their
 * own ready flag.
 *
 * Wire transport is left to the consumer (typically jce_net's ENet
 * channel #1 — reliable but low-priority).  This module owns the
 * room-state machine + slot bookkeeping.
 *
 * Layer: middleware/net (Layer 4) — public.
 */

#ifndef JCE_NET_LOBBY_H
#define JCE_NET_LOBBY_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_LOBBY_NAME_LEN     48
#define JCE_LOBBY_TAG_LEN      24
#define JCE_LOBBY_SLOTS_MAX    16
#define JCE_LOBBY_LIST_MAX     64

typedef enum {
    JCE_LOBBY_STATE_OPEN     = 0,    /* accepting joins */
    JCE_LOBBY_STATE_LOCKED   = 1,    /* host locked, awaiting start */
    JCE_LOBBY_STATE_STARTING = 2,    /* transitioning to in-game */
    JCE_LOBBY_STATE_IN_GAME  = 3,
} JceLobbyState;

typedef struct {
    uint16_t peer_id;           /* 0 = empty slot */
    char     display_name[32];
    bool     ready;
    /* Team id for team-based games; 0 = unassigned. */
    uint8_t  team;
} JceLobbySlot;

typedef struct {
    uint32_t      id;                  /* server-assigned lobby id */
    char          name[JCE_LOBBY_NAME_LEN];
    char          game_mode[JCE_LOBBY_TAG_LEN];
    char          map[JCE_LOBBY_TAG_LEN];
    uint8_t       slot_count;          /* total slots (≤ JCE_LOBBY_SLOTS_MAX) */
    JceLobbySlot  slots[JCE_LOBBY_SLOTS_MAX];
    JceLobbyState state;
    uint16_t      host_peer_id;
    /* Password hash (0 = open lobby).  Hash so we can compare without
     * storing the plaintext. */
    uint32_t      password_hash;
    bool          active;
} JceLobby;

/* ── Local registry (host side) ──────────────────────────────── */

JCE_API void     jce_lobby_clear_all(void);
JCE_API JceLobby *jce_lobby_create(uint32_t id, const char *name,
                                    const char *game_mode,
                                    const char *map,
                                    uint8_t slot_count,
                                    uint16_t host_peer_id);
JCE_API bool      jce_lobby_destroy(uint32_t id);

JCE_API JceLobby *jce_lobby_find(uint32_t id);
JCE_API uint32_t  jce_lobby_list(JceLobby **out, uint32_t cap);

/* ── Slot management ─────────────────────────────────────────── */

/* Returns the slot index (0..slot_count-1) the peer was placed in,
 * or -1 if the lobby is full / locked. */
JCE_API int  jce_lobby_join(JceLobby *l, uint16_t peer_id,
                             const char *display_name);
JCE_API bool jce_lobby_leave(JceLobby *l, uint16_t peer_id);

JCE_API bool jce_lobby_set_ready(JceLobby *l, uint16_t peer_id, bool ready);
JCE_API bool jce_lobby_set_team (JceLobby *l, uint16_t peer_id, uint8_t team);

/* Returns true when every occupied slot is ready. */
JCE_API bool jce_lobby_all_ready(const JceLobby *l);

/* Host operations. */
JCE_API bool jce_lobby_lock   (JceLobby *l);
JCE_API bool jce_lobby_unlock (JceLobby *l);
JCE_API bool jce_lobby_start  (JceLobby *l);

/* Convenience hash for password comparison. */
JCE_API uint32_t jce_lobby_password_hash(const char *plaintext);

JCE_EXTERN_C_END

#endif /* JCE_NET_LOBBY_H */
