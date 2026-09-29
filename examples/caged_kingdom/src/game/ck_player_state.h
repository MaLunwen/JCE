/*
 * ck_player_state.h  Persistent player state for caged_kingdom.
 *
 * Plain-data record that survives a scene transition.  Entities are
 * rebuilt fresh on every scene load, so anything the player *carries*
 * across levels (HP, money, weapons, current quest) lives here instead
 * of in ECS components.
 *
 * Owned by the scene director (one instance per game session); per-scene
 * code reads/writes via ck_player_state_*.
 *
 * Contract: pure C99, no engine handles, freely memcpy-able.
 * See examples/caged_kingdom/SCENES_DESIGN.md appendix A.2.3.
 */

#ifndef CK_PLAYER_STATE_H
#define CK_PLAYER_STATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bitmask of unlocked weapons (max 32 in v1). */
enum {
    CK_WEAPON_FIST       = 1u << 0,
    CK_WEAPON_IRON_STICK = 1u << 1,
    CK_WEAPON_RUNIC_BOW  = 1u << 2,
};

typedef struct CkPlayerState {
    float    hp;
    float    hp_max;
    float    armor;

    int      currency_crystal;  /* common drop */
    int      currency_kingmark; /* faction reputation token */

    int      wanted_stars;      /* 0..5, GTA-style */

    uint32_t weapon_flags;      /* CK_WEAPON_* bitmask */

    /* Current main-storyline quest id (e.g. "act1_m02_first_kill").
       Zero-terminated; truncated silently if a caller writes >63 chars. */
    char     active_quest[64];
} CkPlayerState;

/* Initialise to act1_m01_wake defaults: full HP, fists only, no money,
   no wanted, active_quest = "act1_m01_wake". */
void ck_player_state_init(CkPlayerState *st);

/* Reset to init defaults; cheap memset path. */
void ck_player_state_reset(CkPlayerState *st);

#ifdef __cplusplus
}
#endif

#endif /* CK_PLAYER_STATE_H */
