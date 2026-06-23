/*
 * jce_guide_ch_animation.cpp — User guide chapter: Animation
 * Generated content: every claim verified against the editor source.
 */
#include "panels/jce_panel_user_guide.h"

static const JceGuideBlock b_skeletal[] = {
    { JCE_GB_P,          "guide.anim.skeletal.p1", 0, nullptr },
    { JCE_GB_OPEN_PANEL, "Hierarchy", JCE_PANEL_HIERARCHY, "###hierarchy" },
    { JCE_GB_OPEN_PANEL, "Inspector", JCE_PANEL_INSPECTOR, "###inspector" },
    { JCE_GB_STEP,       "guide.anim.skeletal.s1", 0, nullptr },
    { JCE_GB_STEP,       "guide.anim.skeletal.s2", 0, nullptr },
    { JCE_GB_STEP,       "guide.anim.skeletal.s3", 0, nullptr },
    { JCE_GB_STEP,       "guide.anim.skeletal.s4", 0, nullptr },
    { JCE_GB_STEP,       "guide.anim.skeletal.s5", 0, nullptr },
    { JCE_GB_TIP,        "guide.anim.skeletal.tip1", 0, nullptr },
    { JCE_GB_WARN,       "guide.anim.skeletal.warn1", 0, nullptr },
    { JCE_GB_HOTKEY,     "guide.anim.skeletal.hk1", JCE_HK_PANEL_INSPECTOR, nullptr },
};

static const JceGuideBlock b_workbench[] = {
    { JCE_GB_P,          "guide.anim.workbench.p1", 0, nullptr },
    { JCE_GB_MENU_PATH,  nullptr, 0, "menu.window>window.group.workbenches>window.animationEditor" },
    { JCE_GB_OPEN_PANEL, "window.animationEditor", JCE_PANEL_ANIMATION_EDITOR, "###jce_anim_editor" },
    { JCE_GB_SEP,        nullptr, 0, nullptr },
    { JCE_GB_P,          "guide.anim.workbench.p2", 0, nullptr },
    { JCE_GB_STEP,       "guide.anim.workbench.s1", 0, nullptr },
    { JCE_GB_STEP,       "guide.anim.workbench.s2", 0, nullptr },
    { JCE_GB_STEP,       "guide.anim.workbench.s3", 0, nullptr },
    { JCE_GB_STEP,       "guide.anim.workbench.s4", 0, nullptr },
    { JCE_GB_TIP,        "guide.anim.workbench.tip1", 0, nullptr },
    { JCE_GB_WARN,       "guide.anim.workbench.warn1", 0, nullptr },
    { JCE_GB_TIP,        "guide.anim.workbench.tip2", 0, nullptr },
};

static const JceGuideBlock b_sm[] = {
    { JCE_GB_P,          "guide.anim.sm.p1", 0, nullptr },
    { JCE_GB_MENU_PATH,  nullptr, 0, "menu.window>window.group.workbenches>window.animatorSM" },
    { JCE_GB_OPEN_PANEL, "window.animationEditor", JCE_PANEL_ANIMATION_EDITOR, "###jce_anim_editor" },
    { JCE_GB_STEP,       "guide.anim.sm.s1", 0, nullptr },
    { JCE_GB_STEP,       "guide.anim.sm.s2", 0, nullptr },
    { JCE_GB_STEP,       "guide.anim.sm.s3", 0, nullptr },
    { JCE_GB_STEP,       "guide.anim.sm.s4", 0, nullptr },
    { JCE_GB_STEP,       "guide.anim.sm.s5", 0, nullptr },
    { JCE_GB_HOTKEY,     "guide.anim.sm.hk1", JCE_HK_PLAY_TOGGLE, nullptr },
    { JCE_GB_P,          "guide.anim.sm.p2", 0, nullptr },
    { JCE_GB_TIP,        "guide.anim.sm.tip1", 0, nullptr },
    { JCE_GB_TIP,        "guide.anim.sm.tip2", 0, nullptr },
    { JCE_GB_TIP,        "guide.anim.sm.tip3", 0, nullptr },
    { JCE_GB_WARN,       "guide.anim.sm.warn1", 0, nullptr },
};

static const JceGuideBlock b_ik[] = {
    { JCE_GB_P,          "guide.anim.ik.p1", 0, nullptr },
    { JCE_GB_MENU_PATH,  nullptr, 0, "menu.window>window.group.workbenches>window.animationRigging" },
    { JCE_GB_OPEN_PANEL, "window.animationEditor", JCE_PANEL_ANIMATION_EDITOR, "###jce_anim_editor" },
    { JCE_GB_STEP,       "guide.anim.ik.s1", 0, nullptr },
    { JCE_GB_STEP,       "guide.anim.ik.s2", 0, nullptr },
    { JCE_GB_STEP,       "guide.anim.ik.s3", 0, nullptr },
    { JCE_GB_STEP,       "guide.anim.ik.s4", 0, nullptr },
    { JCE_GB_STEP,       "guide.anim.ik.s5", 0, nullptr },
    { JCE_GB_WARN,       "guide.anim.ik.warn1", 0, nullptr },
    { JCE_GB_WARN,       "guide.anim.ik.warn2", 0, nullptr },
    { JCE_GB_TIP,        "guide.anim.ik.tip1", 0, nullptr },
};

static const JceGuideBlock b_sequencer[] = {
    { JCE_GB_P,          "guide.anim.sequencer.p1", 0, nullptr },
    { JCE_GB_MENU_PATH,  nullptr, 0, "menu.window>window.group.workbenches>window.sequencer" },
    { JCE_GB_OPEN_PANEL, "window.animationEditor", JCE_PANEL_ANIMATION_EDITOR, "###jce_anim_editor" },
    { JCE_GB_STEP,       "guide.anim.sequencer.s1", 0, nullptr },
    { JCE_GB_STEP,       "guide.anim.sequencer.s2", 0, nullptr },
    { JCE_GB_STEP,       "guide.anim.sequencer.s3", 0, nullptr },
    { JCE_GB_STEP,       "guide.anim.sequencer.s4", 0, nullptr },
    { JCE_GB_STEP,       "guide.anim.sequencer.s5", 0, nullptr },
    { JCE_GB_TIP,        "guide.anim.sequencer.tip1", 0, nullptr },
    { JCE_GB_WARN,       "guide.anim.sequencer.warn1", 0, nullptr },
    { JCE_GB_HOTKEY,     "guide.anim.sequencer.hk1", JCE_HK_PLAY_TOGGLE, nullptr },
};

static const JceGuideBlock b_curves[] = {
    { JCE_GB_P,          "guide.anim.curves.p1", 0, nullptr },
    { JCE_GB_MENU_PATH,  nullptr, 0, "menu.window>window.group.workbenches>window.curveEditor" },
    { JCE_GB_OPEN_PANEL, "window.animationEditor", JCE_PANEL_ANIMATION_EDITOR, "###jce_anim_editor" },
    { JCE_GB_STEP,       "guide.anim.curves.s1", 0, nullptr },
    { JCE_GB_STEP,       "guide.anim.curves.s2", 0, nullptr },
    { JCE_GB_STEP,       "guide.anim.curves.s3", 0, nullptr },
    { JCE_GB_STEP,       "guide.anim.curves.s4", 0, nullptr },
    { JCE_GB_WARN,       "guide.anim.curves.warn1", 0, nullptr },
    { JCE_GB_SEP,        nullptr, 0, nullptr },
    { JCE_GB_P,          "guide.anim.curves.p2", 0, nullptr },
    { JCE_GB_MENU_PATH,  nullptr, 0, "menu.window>window.group.workbenches>Timeline" },
    { JCE_GB_STEP,       "guide.anim.curves.s5", 0, nullptr },
    { JCE_GB_TIP,        "guide.anim.curves.tip1", 0, nullptr },
};

static const JceGuideTopic k_topics[] = {
    { "guide.anim.skeletal.title",  b_skeletal,  JCE_GUIDE_COUNT(b_skeletal)  },
    { "guide.anim.workbench.title", b_workbench, JCE_GUIDE_COUNT(b_workbench) },
    { "guide.anim.sm.title",        b_sm,        JCE_GUIDE_COUNT(b_sm)        },
    { "guide.anim.ik.title",        b_ik,        JCE_GUIDE_COUNT(b_ik)        },
    { "guide.anim.sequencer.title", b_sequencer, JCE_GUIDE_COUNT(b_sequencer) },
    { "guide.anim.curves.title",    b_curves,    JCE_GUIDE_COUNT(b_curves)    },
};

const JceGuideChapter g_jce_guide_ch_animation = {
    "guide.anim.title", k_topics, JCE_GUIDE_COUNT(k_topics)
};
