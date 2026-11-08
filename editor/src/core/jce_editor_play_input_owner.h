/* Editor Play input ownership boundary.  Game View owns its input intent;
 * Play Stop invokes this API even when the Game View tab is hidden/clipped.
 * The function is idempotent and must run on the editor UI thread. */
#ifndef JCE_EDITOR_PLAY_INPUT_OWNER_H
#define JCE_EDITOR_PLAY_INPUT_OWNER_H
#ifdef __cplusplus
extern "C" {
#endif

void jce_editor_game_view_release_play_input(void);

#ifdef __cplusplus
}
#endif
#endif
