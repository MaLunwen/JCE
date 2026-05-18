/*
 * ck_player_state.c  Defaults and reset for CkPlayerState.
 */

#include "ck_player_state.h"

#include <string.h>

void ck_player_state_init(CkPlayerState *st)
{
    if (!st) return;
    memset(st, 0, sizeof(*st));
    st->hp           = 100.0f;
    st->hp_max       = 100.0f;
    st->armor        = 0.0f;
    st->weapon_flags = CK_WEAPON_FIST;
    /* Default starting quest matches the SETTING_BIBLE opening beat. */
    strcpy(st->active_quest, "act1_m01_wake");
}

void ck_player_state_reset(CkPlayerState *st)
{
    ck_player_state_init(st);
}
