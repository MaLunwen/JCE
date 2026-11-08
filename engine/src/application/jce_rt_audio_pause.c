/* Runtime audio pause ownership. This is part of the existing application
 * binding owner, not a second mixer or voice registry. Public audio commands
 * validate full-generation handles; stop/unload/recycle invalidates a saved
 * handle, so focus regain cannot start a replacement sound in the same slot. */
#include "jce_rt_internal.h"

JCE_API void JCE_CALL jce_runtime_pause_audio(JceRuntime *rt)
{
    if (!rt || !rt->audio) return;
    for (int i = 0; i < rt->voice_count; ++i) {
        VoiceEntry *entry = &rt->voices[i];
        if (entry->paused_by_runtime == entry->voice &&
            entry->voice != JCE_VOICE_INVALID) continue;
        /* Do not acquire an already user-paused or ended voice. */
        if (!jce_audio_is_playing(rt->audio, entry->voice)) continue;
        entry->paused_by_runtime = entry->voice;
        jce_audio_pause(rt->audio, entry->voice);
    }
}

JCE_API void JCE_CALL jce_runtime_resume_audio(JceRuntime *rt)
{
    if (!rt || !rt->audio) return;
    for (int i = 0; i < rt->voice_count; ++i) {
        VoiceEntry *entry = &rt->voices[i];
        const JceVoice paused = entry->paused_by_runtime;
        /* Clear ownership before calling the device owner, also on mismatch. */
        entry->paused_by_runtime = JCE_VOICE_INVALID;
        if (paused != JCE_VOICE_INVALID && paused == entry->voice)
            jce_audio_resume(rt->audio, paused);
    }
}
