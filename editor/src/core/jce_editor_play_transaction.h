/* Private Play transaction sequencing; callbacks use the existing scene history. */
#ifndef JCE_EDITOR_PLAY_TRANSACTION_H
#define JCE_EDITOR_PLAY_TRANSACTION_H

template<class Capture, class Discard>
bool jce_editor_play_capture(bool &valid, Capture capture, Discard discard)
{
    if (valid) return false;
    valid = capture();
    if (!valid) discard();
    return valid;
}

template<class Restore, class Discard>
bool jce_editor_play_restore(bool &valid, Restore restore, Discard discard)
{
    if (!valid) return true;
    if (!restore()) return false;
    discard();
    valid = false;
    return true;
}

template<class Create, class Teardown, class Restore, class Discard>
bool jce_editor_play_create(bool &valid, Create create, Teardown teardown,
                            Restore restore, Discard discard)
{
    if (!valid) return false;
    if (create()) return true;
    teardown();
    jce_editor_play_restore(valid, restore, discard);
    return false;
}
/* Host exit reuses Stop before releasing its borrowed view/scene services. */
template<class Stop, class Shutdown>
void jce_editor_exit_after_play(bool play_active, Stop stop, Shutdown shutdown)
{
    if (play_active) stop();
    shutdown();
}
#endif
