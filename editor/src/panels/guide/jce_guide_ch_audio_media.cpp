/*
 * jce_guide_ch_audio_media.cpp — User guide chapter: Audio & Media
 * Generated content: every claim verified against the editor source.
 */
#include "panels/jce_panel_user_guide.h"

/* ── Topic 1: Audio Sources & the 3D Sound Field ────────────────────
 * Verified: editor/src/panels/jce_panel_inspector_audio.cpp (field set + ranges),
 * engine/src/application/jce_runtime.c rt_finish_audio_source (spatial
 * threshold 0.5, inverse attenuation 1..25, bus routing), rt_update_audio_3d
 * (listener follows primary camera, occlusion gain + low-pass),
 * rt_spawn_audio_async (PAK async decode, 1-2 frames late),
 * editor/src/viewers/jce_fv_audio.cpp (WAV/OGG/MP3/FLAC preview). */
static const JceGuideBlock b_src[] = {
    { JCE_GB_P,          "guide.audio.src.p1",  0, nullptr },
    { JCE_GB_OPEN_PANEL, "Hierarchy",           JCE_PANEL_HIERARCHY, "###hierarchy" },
    { JCE_GB_OPEN_PANEL, "Inspector",           JCE_PANEL_INSPECTOR, "###inspector" },
    { JCE_GB_H1,         "guide.audio.src.h1",  0, nullptr },
    { JCE_GB_STEP,       "guide.audio.src.s1",  0, nullptr },
    { JCE_GB_STEP,       "guide.audio.src.s2",  0, nullptr },
    { JCE_GB_STEP,       "guide.audio.src.s3",  0, nullptr },
    { JCE_GB_STEP,       "guide.audio.src.s4",  0, nullptr },
    { JCE_GB_HOTKEY,     "guide.audio.src.hk1", JCE_HK_PLAY_TOGGLE, nullptr },
    { JCE_GB_SEP,        nullptr,               0, nullptr },
    { JCE_GB_BULLET,     "guide.audio.src.b1",  0, nullptr },
    { JCE_GB_BULLET,     "guide.audio.src.b2",  0, nullptr },
    { JCE_GB_BULLET,     "guide.audio.src.b3",  0, nullptr },
    { JCE_GB_TIP,        "guide.audio.src.t1",  0, nullptr },
    { JCE_GB_TIP,        "guide.audio.src.t2",  0, nullptr },
};

/* ── Topic 2: Audio Mixer ───────────────────────────────────────────
 * Verified: editor/src/panels/jce_panel_audio_mixer.cpp (bus tree, volume
 * 0..1.5, M/S, add/remove group, save/reload, ~/.jce/audio_mixer.json,
 * rename popup is a stub), editor/src/core/jce_editor_play.cpp (config
 * consumed at Play start), jce_runtime.c rt_init_mixer/rt_apply_mixer. */
static const JceGuideBlock b_mixer[] = {
    { JCE_GB_P,          "guide.audio.mixer.p1", 0, nullptr },
    { JCE_GB_OPEN_PANEL, "audioMixer.title",     JCE_PANEL_AUDIO_MIXER, "###audio_mixer" },
    { JCE_GB_MENU_PATH,  nullptr,                0, "menu.window>window.group.workbenches>audioMixer.title" },
    { JCE_GB_H1,         "guide.audio.mixer.h1", 0, nullptr },
    { JCE_GB_STEP,       "guide.audio.mixer.s1", 0, nullptr },
    { JCE_GB_STEP,       "guide.audio.mixer.s2", 0, nullptr },
    { JCE_GB_STEP,       "guide.audio.mixer.s3", 0, nullptr },
    { JCE_GB_STEP,       "guide.audio.mixer.s4", 0, nullptr },
    { JCE_GB_P,          "guide.audio.mixer.p2", 0, nullptr },
    { JCE_GB_P,          "guide.audio.mixer.p3", 0, nullptr },
    { JCE_GB_WARN,       "guide.audio.mixer.w1", 0, nullptr },
    { JCE_GB_TIP,        "guide.audio.mixer.t1", 0, nullptr },
};

/* ── Topic 3: Reverb Zones ──────────────────────────────────────────
 * Verified: jce_panel_inspector_audio.cpp draw_comp_audio_reverb_zone (presets,
 * min/max distance), jce_runtime.c rt_reverb_zone_collect (sphere at world
 * pos, full strength inside min, falloff to max) + rt_reverb_preset_for
 * (family mapping), jce_panel_audio_mixer.cpp draw_reverb_zones_tab
 * (owner/preset/min/max table + ping). */
static const JceGuideBlock b_reverb[] = {
    { JCE_GB_P,          "guide.audio.reverb.p1", 0, nullptr },
    { JCE_GB_H1,         "guide.audio.reverb.h1", 0, nullptr },
    { JCE_GB_STEP,       "guide.audio.reverb.s1", 0, nullptr },
    { JCE_GB_STEP,       "guide.audio.reverb.s2", 0, nullptr },
    { JCE_GB_STEP,       "guide.audio.reverb.s3", 0, nullptr },
    { JCE_GB_STEP,       "guide.audio.reverb.s4", 0, nullptr },
    { JCE_GB_BULLET,     "guide.audio.reverb.b1", 0, nullptr },
    { JCE_GB_SEP,        nullptr,                 0, nullptr },
    { JCE_GB_P,          "guide.audio.reverb.p2", 0, nullptr },
    { JCE_GB_OPEN_PANEL, "audioMixer.title",      JCE_PANEL_AUDIO_MIXER, "###audio_mixer" },
    { JCE_GB_MENU_PATH,  nullptr,                 0, "menu.window>window.group.workbenches>audioMixer.title>window.reverbZones" },
    { JCE_GB_TIP,        "guide.audio.reverb.t1", 0, nullptr },
};

/* ── Topic 4: Screenshots & Screen Recording ────────────────────────
 * Verified: editor/src/ui/jce_editor_layout.cpp cmd_screenshot_ (F12 →
 * .jce/screenshots/jce_screenshot_<ts>.png, deferred one frame) and
 * cmd_record_toggle_ (F9 → .jce/recordings/rec_<ts>.mkv),
 * editor/src/core/jce_editor_recorder.cpp (VP9 + Opus WASAPI loopback,
 * Windows-only audio, bounded queues, measured fps). */
static const JceGuideBlock b_capture[] = {
    { JCE_GB_P,      "guide.audio.capture.p1",  0, nullptr },
    { JCE_GB_HOTKEY, "guide.audio.capture.hk1", JCE_HK_UI_SCREENSHOT, nullptr },
    { JCE_GB_P,      "guide.audio.capture.p2",  0, nullptr },
    { JCE_GB_SEP,    nullptr,                   0, nullptr },
    { JCE_GB_HOTKEY, "guide.audio.capture.hk2", JCE_HK_UI_RECORD, nullptr },
    { JCE_GB_H1,     "guide.audio.capture.h1",  0, nullptr },
    { JCE_GB_STEP,   "guide.audio.capture.s1",  0, nullptr },
    { JCE_GB_STEP,   "guide.audio.capture.s2",  0, nullptr },
    { JCE_GB_STEP,   "guide.audio.capture.s3",  0, nullptr },
    { JCE_GB_P,      "guide.audio.capture.p3",  0, nullptr },
    { JCE_GB_WARN,   "guide.audio.capture.w1",  0, nullptr },
    { JCE_GB_WARN,   "guide.audio.capture.w2",  0, nullptr },
    { JCE_GB_TIP,    "guide.audio.capture.t1",  0, nullptr },
};

/* ── Topic 5: Video Playback ────────────────────────────────────────
 * Verified: jce_panel_inspector_audio.cpp draw_comp_video_player (clip/loop/autoplay/
 * transport + resolution readout), engine jce_scene_video.c (video-as-
 * texture albedo binding), engine/src/middleware/video (VP8/VP9/AV1 + Opus
 * patent-free; H.264/H.265/AAC behind JCE_ENABLE_PATENTED_CODECS),
 * editor/src/ui/jce_editor_panels.cpp About "Patented Codecs" row,
 * editor/src/viewers/jce_fv_video.cpp (transport/seek/zoom/perf, tab-away
 * release, patent-disabled message). */
static const JceGuideBlock b_video[] = {
    { JCE_GB_P,          "guide.audio.video.p1", 0, nullptr },
    { JCE_GB_H1,         "guide.audio.video.h1", 0, nullptr },
    { JCE_GB_STEP,       "guide.audio.video.s1", 0, nullptr },
    { JCE_GB_STEP,       "guide.audio.video.s2", 0, nullptr },
    { JCE_GB_STEP,       "guide.audio.video.s3", 0, nullptr },
    { JCE_GB_P,          "guide.audio.video.p2", 0, nullptr },
    { JCE_GB_P,          "guide.audio.video.p3", 0, nullptr },
    { JCE_GB_MENU_PATH,  nullptr,                0, "menu.help>menu.help.about" },
    { JCE_GB_WARN,       "guide.audio.video.w1", 0, nullptr },
    { JCE_GB_SEP,        nullptr,                0, nullptr },
    { JCE_GB_P,          "guide.audio.video.p4", 0, nullptr },
    { JCE_GB_OPEN_PANEL, "File Viewer",          JCE_PANEL_FILE_VIEWER, "###file_viewer" },
    { JCE_GB_TIP,        "guide.audio.video.t1", 0, nullptr },
    { JCE_GB_TIP,        "guide.audio.video.t2", 0, nullptr },
};

static const JceGuideTopic k_topics[] = {
    { "guide.audio.src.title",     b_src,     JCE_GUIDE_COUNT(b_src)     },
    { "guide.audio.mixer.title",   b_mixer,   JCE_GUIDE_COUNT(b_mixer)   },
    { "guide.audio.reverb.title",  b_reverb,  JCE_GUIDE_COUNT(b_reverb)  },
    { "guide.audio.capture.title", b_capture, JCE_GUIDE_COUNT(b_capture) },
    { "guide.audio.video.title",   b_video,   JCE_GUIDE_COUNT(b_video)   },
};

const JceGuideChapter g_jce_guide_ch_audio_media = {
    "guide.audio.title", k_topics, JCE_GUIDE_COUNT(k_topics)
};
