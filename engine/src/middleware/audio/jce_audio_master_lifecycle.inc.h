/* Actual lifecycle entries shared with the explicit no-audio hook fixture. */
#include "jce_audio_master_lifecycle_internal.h"
#ifndef JCE_NO_AUDIO
bool jce_audio_master_is_callback_internal(void)
{
    return jce_audio_master_gate_self(&s_master.gate);
}

bool jce_audio_master_device_live_internal(void)
{
    return s_master.device_inited;
}

void jce_audio_master_session_open_internal(void)
{
    if (!jce_thread_is_main() || !master_control_allowed() || s_master.retiring) return;
    s_master.session_closed = false;
}

void jce_audio_master_session_close_internal(void)
{
    if (!jce_thread_is_main()) return;
    s_master.session_closed = true;
}

bool jce_audio_master_try_retire_internal(void)
{
    if (!jce_thread_is_main() || !master_control_allowed() ||
        !s_master.session_closed || s_master.retiring)
        return false;
    if (!jce_audio_master_gate_quiesce(&s_master.gate, 2000)) return false;
    s_master.retiring = true;
    /* Callback admission is closed and old user borrows have returned. Keep
     * raw graph/tap owners until app.exit releases them; never free strangers. */
    for (int i = 0; i < JCE_AUDIO_MAX_ENGINES; ++i)
        SDL_SetAtomicInt(&s_master.slots[i].live, 0);
    SDL_SetAtomicInt(&s_master.tap_live, 0);
    const bool had_device = s_master.device_inited;
    if (had_device) {
        /* miniaudio deliberately does not stop first: sleeping backends can
         * disagree about playing state. Its uninit owns the actual join. */
        ma_device_uninit(&s_master.device);
        s_master.device_inited = false;
    }
    const bool released = jce_audio_master_gate_release(&s_master.gate);
    s_master.device_failed = false;
    s_master.retiring = false;
    if (had_device)
        LOG_INFO("jce_audio", "master device retired after callback quiescence");
    return released;
}
#else
bool jce_audio_master_is_callback_internal(void) { return false; }
bool jce_audio_master_device_live_internal(void) { return false; }
void jce_audio_master_session_open_internal(void) {}
void jce_audio_master_session_close_internal(void) {}
bool jce_audio_master_try_retire_internal(void) { return true; }
#endif
