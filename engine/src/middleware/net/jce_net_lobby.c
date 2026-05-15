/*
 * jce_net_lobby.c  Lobby / matchmaking host-side state.
 *
 * Storage: fixed-cap array of lobbies (LIST_MAX = 64 — sufficient for
 * a casual matchmaking pool).  Each lobby is a POD struct so the
 * snapshot can be serialised straight to the wire.
 */

#include <jce/middleware/net/jce_net_lobby.h>

#include <string.h>

static JceLobby s_lobbies[JCE_LOBBY_LIST_MAX];

void jce_lobby_clear_all(void)
{
    memset(s_lobbies, 0, sizeof(s_lobbies));
}

JceLobby *jce_lobby_find(uint32_t id)
{
    for (int i = 0; i < JCE_LOBBY_LIST_MAX; ++i)
        if (s_lobbies[i].active && s_lobbies[i].id == id)
            return &s_lobbies[i];
    return NULL;
}

JceLobby *jce_lobby_create(uint32_t id, const char *name,
                            const char *mode, const char *map,
                            uint8_t slots, uint16_t host)
{
    if (jce_lobby_find(id)) return NULL;
    for (int i = 0; i < JCE_LOBBY_LIST_MAX; ++i) {
        if (s_lobbies[i].active) continue;
        JceLobby *l = &s_lobbies[i];
        memset(l, 0, sizeof(*l));
        l->id = id;
        if (name) strncpy(l->name, name, JCE_LOBBY_NAME_LEN - 1);
        if (mode) strncpy(l->game_mode, mode, JCE_LOBBY_TAG_LEN - 1);
        if (map ) strncpy(l->map,  map,  JCE_LOBBY_TAG_LEN - 1);
        l->slot_count = slots <= JCE_LOBBY_SLOTS_MAX ? slots
                                                     : JCE_LOBBY_SLOTS_MAX;
        l->host_peer_id = host;
        l->state = JCE_LOBBY_STATE_OPEN;
        l->active = true;
        /* Place host in slot 0. */
        if (host != 0) {
            l->slots[0].peer_id = host;
            strncpy(l->slots[0].display_name, "Host",
                    sizeof(l->slots[0].display_name) - 1);
        }
        return l;
    }
    return NULL;
}

bool jce_lobby_destroy(uint32_t id)
{
    JceLobby *l = jce_lobby_find(id);
    if (!l) return false;
    memset(l, 0, sizeof(*l));
    return true;
}

uint32_t jce_lobby_list(JceLobby **out, uint32_t cap)
{
    uint32_t n = 0;
    for (int i = 0; i < JCE_LOBBY_LIST_MAX && n < cap; ++i)
        if (s_lobbies[i].active) out[n++] = &s_lobbies[i];
    return n;
}

int jce_lobby_join(JceLobby *l, uint16_t peer, const char *name)
{
    if (!l || l->state != JCE_LOBBY_STATE_OPEN) return -1;
    /* Reject duplicate peer. */
    for (int i = 0; i < l->slot_count; ++i)
        if (l->slots[i].peer_id == peer) return i;
    for (int i = 0; i < l->slot_count; ++i) {
        if (l->slots[i].peer_id == 0) {
            l->slots[i].peer_id = peer;
            if (name) strncpy(l->slots[i].display_name, name,
                              sizeof(l->slots[i].display_name) - 1);
            return i;
        }
    }
    return -1;
}

bool jce_lobby_leave(JceLobby *l, uint16_t peer)
{
    if (!l) return false;
    for (int i = 0; i < l->slot_count; ++i) {
        if (l->slots[i].peer_id == peer) {
            memset(&l->slots[i], 0, sizeof(l->slots[i]));
            return true;
        }
    }
    return false;
}

static JceLobbySlot *find_slot(JceLobby *l, uint16_t peer)
{
    if (!l) return NULL;
    for (int i = 0; i < l->slot_count; ++i)
        if (l->slots[i].peer_id == peer) return &l->slots[i];
    return NULL;
}

bool jce_lobby_set_ready(JceLobby *l, uint16_t peer, bool ready)
{
    JceLobbySlot *s = find_slot(l, peer);
    if (!s) return false;
    s->ready = ready;
    return true;
}

bool jce_lobby_set_team(JceLobby *l, uint16_t peer, uint8_t team)
{
    JceLobbySlot *s = find_slot(l, peer);
    if (!s) return false;
    s->team = team;
    return true;
}

bool jce_lobby_all_ready(const JceLobby *l)
{
    if (!l) return false;
    bool any = false;
    for (int i = 0; i < l->slot_count; ++i) {
        if (l->slots[i].peer_id == 0) continue;
        any = true;
        if (!l->slots[i].ready) return false;
    }
    return any;
}

bool jce_lobby_lock(JceLobby *l)
{
    if (!l || l->state != JCE_LOBBY_STATE_OPEN) return false;
    l->state = JCE_LOBBY_STATE_LOCKED;
    return true;
}

bool jce_lobby_unlock(JceLobby *l)
{
    if (!l || l->state != JCE_LOBBY_STATE_LOCKED) return false;
    l->state = JCE_LOBBY_STATE_OPEN;
    return true;
}

bool jce_lobby_start(JceLobby *l)
{
    if (!l) return false;
    if (l->state != JCE_LOBBY_STATE_OPEN && l->state != JCE_LOBBY_STATE_LOCKED)
        return false;
    if (!jce_lobby_all_ready(l)) return false;
    l->state = JCE_LOBBY_STATE_STARTING;
    return true;
}

uint32_t jce_lobby_password_hash(const char *p)
{
    if (!p) return 0;
    uint32_t h = 2166136261u;
    while (*p) {
        h ^= (unsigned char)*p++;
        h *= 16777619u;
    }
    return h;
}
