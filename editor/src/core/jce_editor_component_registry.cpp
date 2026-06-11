/*
 * jce_editor_component_registry.cpp
 */

#include "jce_editor_component_registry.h"

#include <stddef.h>
#include <string.h>

namespace {

/* clang-format off */
static const JceEditorComponentDescriptor kDescriptors[] = {
    { JCE_COMP_FLAG_TRANSFORM,            JCE_COMP_FLAG_TRANSFORM,            "Transform",             "comp.transform",            false, false, true  },
    { JCE_EDITOR_COMP_SLOT_LIGHT_GROUP,   0,                                  "Light",                 NULL,                        false, true,  false },
    { JCE_COMP_FLAG_DIR_LIGHT,            JCE_COMP_FLAG_DIR_LIGHT,            "Directional Light",     "comp.dirLight",             true,  true,  false },
    { JCE_COMP_FLAG_POINT_LIGHT,          JCE_COMP_FLAG_POINT_LIGHT,          "Point Light",           "comp.pointLight",           true,  true,  false },
    { JCE_COMP_FLAG_SPOT_LIGHT,           JCE_COMP_FLAG_SPOT_LIGHT,           "Spot Light",            "comp.spotLight",            true,  true,  false },
    { JCE_COMP_FLAG_CAMERA,               JCE_COMP_FLAG_CAMERA,               "Camera",                "comp.camera",               true,  true,  true  },
    { JCE_COMP_FLAG_MESH_RENDERER,        JCE_COMP_FLAG_MESH_RENDERER,        "Mesh Renderer",         "comp.meshRenderer",         true,  true,  true  },
    { JCE_COMP_FLAG_SPRITE_RENDERER,      JCE_COMP_FLAG_SPRITE_RENDERER,      "Sprite Renderer",       "comp.spriteRenderer",       true,  true,  true  },
    { JCE_COMP_FLAG_ANIMATOR,             JCE_COMP_FLAG_ANIMATOR,             "Animator",              "comp.animator",             true,  true,  false },
    { JCE_COMP_FLAG_SKELETAL_ANIMATOR,    JCE_COMP_FLAG_SKELETAL_ANIMATOR,    "Skeletal Animator",     "comp.skeletalAnimator",     true,  true,  true  },
    { JCE_EDITOR_COMP_SLOT_IK_CONSTRAINTS, 0,                                 "IK Constraints",        "comp.ikConstraints",        true,  true,  false },
    { JCE_COMP_FLAG_RIGIDBODY,            JCE_COMP_FLAG_RIGIDBODY,            "Rigidbody",             "comp.rigidbody",            true,  true,  true  },
    { JCE_COMP_FLAG_BOX_COLLIDER,         JCE_COMP_FLAG_BOX_COLLIDER,         "Box Collider",          "comp.boxCollider",          true,  true,  true  },
    { JCE_COMP_FLAG_SPHERE_COLLIDER,      JCE_COMP_FLAG_SPHERE_COLLIDER,      "Sphere Collider",       "comp.sphereCollider",       true,  true,  true  },
    { JCE_COMP_FLAG_CHARACTER_CONTROLLER, JCE_COMP_FLAG_CHARACTER_CONTROLLER, "Character Controller",  "comp.characterController", true,  true,  false },
    { JCE_COMP_FLAG_AUDIO_SOURCE,         JCE_COMP_FLAG_AUDIO_SOURCE,         "Audio Source",          "comp.audioSource",          true,  true,  true  },
    { JCE_EDITOR_COMP_SLOT_VIDEO_PLAYER,  0,                                  "Video Player",          "comp.videoPlayer",          true,  true,  false },
    { JCE_COMP_FLAG_SCRIPT,               JCE_COMP_FLAG_SCRIPT,               "Script",                "comp.script",               true,  true,  false },
    { JCE_COMP_FLAG_SKYBOX,               JCE_COMP_FLAG_SKYBOX,               "Skybox",                "comp.skybox",               true,  true,  false },
    { JCE_COMP_FLAG_SPRITE_ANIMATOR,      JCE_COMP_FLAG_SPRITE_ANIMATOR,      "Sprite Animator",       "comp.spriteAnimator",       true,  true,  false },
    { JCE_COMP_FLAG_CONSTRAINT,           JCE_COMP_FLAG_CONSTRAINT,           "Constraint",            "comp.constraint",           true,  true,  true  },
    { JCE_COMP_FLAG_TERRAIN,              JCE_COMP_FLAG_TERRAIN,              "Terrain",               "comp.terrain",              true,  true,  false },
    { JCE_COMP_FLAG_RIGIDBODY_2D,         JCE_COMP_FLAG_RIGIDBODY_2D,         "Rigidbody 2D",          "comp.rigidbody2d",          true,  true,  false },
    { JCE_COMP_FLAG_PARTICLE_EMITTER,     JCE_COMP_FLAG_PARTICLE_EMITTER,     "Particle Emitter",      "comp.particleEmitter",      true,  true,  false },
    { JCE_COMP_FLAG_BEHAVIOR_TREE,        JCE_COMP_FLAG_BEHAVIOR_TREE,        "Behavior Tree",         "comp.behaviorTree",         true,  true,  false },
    { JCE_COMP_FLAG_EDITOR_META,          JCE_COMP_FLAG_EDITOR_META,          "Editor Meta",           "comp.editorMeta",           false, true,  false },
    { JCE_COMP_FLAG_LOD_GROUP,            JCE_COMP_FLAG_LOD_GROUP,            "LOD Group",             "comp.lodGroup",             true,  true,  false },
    { JCE_COMP_FLAG_VIRTUAL_CAMERA,       JCE_COMP_FLAG_VIRTUAL_CAMERA,       "Virtual Camera",        "comp.virtualCamera",        true,  true,  false },
    { JCE_COMP_FLAG_TRIGGER_VOLUME,       JCE_COMP_FLAG_TRIGGER_VOLUME,       "Trigger Volume",        "comp.triggerVolume",        true,  true,  false },
    { JCE_COMP_FLAG_CAPSULE_COLLIDER,     JCE_COMP_FLAG_CAPSULE_COLLIDER,     "Capsule Collider",      "comp.capsuleCollider",      true,  true,  true  },
    { JCE_COMP_FLAG_MESH_COLLIDER,        JCE_COMP_FLAG_MESH_COLLIDER,        "Mesh Collider",         "comp.meshCollider",         true,  true,  true  },
    { JCE_EDITOR_COMP_SLOT_COMPOUND_COLLIDER, 0,                              "Compound Collider",     "comp.compoundCollider",     true,  true,  true  },
    { JCE_COMP_FLAG_COLLIDER_2D,          JCE_COMP_FLAG_COLLIDER_2D,          "Collider 2D",           "comp.collider2d",           true,  true,  false },
    { JCE_COMP_FLAG_TRAIL_RENDERER,       JCE_COMP_FLAG_TRAIL_RENDERER,       "Trail Renderer",        "comp.trailRenderer",        true,  true,  false },
    { JCE_COMP_FLAG_LINE_RENDERER,        JCE_COMP_FLAG_LINE_RENDERER,        "Line Renderer",         "comp.lineRenderer",         true,  true,  false },
    { JCE_COMP_FLAG_REFLECTION_PROBE,     JCE_COMP_FLAG_REFLECTION_PROBE,     "Reflection Probe",      "comp.reflectionProbe",      true,  true,  false },
    { JCE_COMP_FLAG_DECAL,                JCE_COMP_FLAG_DECAL,                "Decal Projector",       "comp.decal",                true,  true,  false },
    { JCE_COMP_FLAG_LIGHT_PROBE_GROUP,    JCE_COMP_FLAG_LIGHT_PROBE_GROUP,    "Light Probe Group",     "comp.lightProbeGroup",      true,  true,  false },
    { JCE_COMP_FLAG_AUDIO_LISTENER,       JCE_COMP_FLAG_AUDIO_LISTENER,       "Audio Listener",        "comp.audioListener",        true,  true,  false },
    { JCE_COMP_FLAG_AUDIO_REVERB_ZONE,    JCE_COMP_FLAG_AUDIO_REVERB_ZONE,    "Audio Reverb Zone",     "comp.audioReverbZone",      true,  true,  false },
    { JCE_COMP_FLAG_AUDIO_OCCLUSION,      JCE_COMP_FLAG_AUDIO_OCCLUSION,      "Audio Occlusion",       "comp.audioOcclusion",       true,  true,  false },
    { JCE_COMP_FLAG_SPAWN_MANAGER,        JCE_COMP_FLAG_SPAWN_MANAGER,        "Spawn Manager",         "comp.spawnManager",         true,  true,  false },
    { JCE_COMP_FLAG_WEAPON,               JCE_COMP_FLAG_WEAPON,               "Weapon",                "comp.weapon",               true,  true,  false },
    { JCE_COMP_FLAG_SAVE_POINT,           JCE_COMP_FLAG_SAVE_POINT,           "Save Point",            "comp.savePoint",            true,  true,  false },
    { JCE_COMP_FLAG_WHEEL_COLLIDER,       JCE_COMP_FLAG_WHEEL_COLLIDER,       "Wheel Collider",        "comp.wheelCollider",        true,  true,  false },
    { JCE_COMP_FLAG_CONSTANT_FORCE,       JCE_COMP_FLAG_CONSTANT_FORCE,       "Constant Force",        "comp.constantForce",        true,  true,  false },
    { JCE_COMP_FLAG_CONFIGURABLE_JOINT,   JCE_COMP_FLAG_CONFIGURABLE_JOINT,   "Configurable Joint",    "comp.configurableJoint",    true,  true,  false },
    { JCE_COMP_FLAG_JOINT_2D,             JCE_COMP_FLAG_JOINT_2D,             "Joint 2D",              "comp.joint2d",              true,  true,  false },
    { JCE_COMP_FLAG_BILLBOARD_RENDERER,   JCE_COMP_FLAG_BILLBOARD_RENDERER,   "Billboard Renderer",    "comp.billboardRenderer",    true,  true,  false },
    { JCE_COMP_FLAG_CANVAS,               JCE_COMP_FLAG_CANVAS,               "Canvas",                "comp.canvas",               true,  true,  false },
    { JCE_COMP_FLAG_CANVAS_GROUP,         JCE_COMP_FLAG_CANVAS_GROUP,         "Canvas Group",          "comp.canvasGroup",          true,  true,  false },
    { JCE_COMP_FLAG_LAYOUT_GROUP,         JCE_COMP_FLAG_LAYOUT_GROUP,         "Layout Group",          "comp.layoutGroup",          true,  true,  false },
    { JCE_COMP_FLAG_UI_IMAGE,             JCE_COMP_FLAG_UI_IMAGE,             "UI Image",              "comp.uiImage",              true,  true,  false },
    { JCE_COMP_FLAG_UI_TEXT,              JCE_COMP_FLAG_UI_TEXT,              "UI Text",               "comp.uiText",               true,  true,  false },
    { JCE_COMP_FLAG_UI_BUTTON,            JCE_COMP_FLAG_UI_BUTTON,            "UI Button",             "comp.uiButton",             true,  true,  false },
    { JCE_COMP_FLAG_CLOTH,                JCE_COMP_FLAG_CLOTH,                "Cloth",                 "comp.cloth",                true,  true,  false },
    { JCE_COMP_FLAG_NETWORK_OBJECT,       JCE_COMP_FLAG_NETWORK_OBJECT,       "Network Object",        "comp.networkObject",        true,  true,  false },
    { JCE_COMP_FLAG_NET_TRANSFORM,        JCE_COMP_FLAG_NET_TRANSFORM,        "Network Transform",     "comp.netTransform",         true,  true,  false },
    { JCE_COMP_FLAG_NET_ANIMATOR,         JCE_COMP_FLAG_NET_ANIMATOR,         "Network Animator",      "comp.netAnimator",          true,  true,  false },
    { JCE_COMP_FLAG_NET_RIGIDBODY,        JCE_COMP_FLAG_NET_RIGIDBODY,        "Network Rigidbody",     "comp.netRigidbody",         true,  true,  false },
    /* JCE_COMP_FLAG_VFX_GRAPH retired (v0.9.9): scene loads migrate it onto
     * Particle Emitter; the VFX Graph panel now exports .particles.json. */
    { JCE_COMP_FLAG_TILEMAP,              JCE_COMP_FLAG_TILEMAP,              "Tilemap",               "comp.tilemap",              true,  true,  false },
    { JCE_COMP_FLAG_TILEMAP_COLLIDER_2D,  JCE_COMP_FLAG_TILEMAP_COLLIDER_2D,  "Tilemap Collider 2D",   "comp.tilemapCollider2d",    true,  true,  false },
    { JCE_COMP_FLAG_AVATAR,               JCE_COMP_FLAG_AVATAR,               "Avatar",                "comp.avatar",               true,  true,  false },
    { JCE_COMP_FLAG_VOLUME,               JCE_COMP_FLAG_VOLUME,               "Volume",                "comp.volume",               true,  true,  false },
    { JCE_COMP_FLAG_OCCLUSION_PORTAL,     JCE_COMP_FLAG_OCCLUSION_PORTAL,     "Occlusion Portal",      "comp.occlusionPortal",      true,  true,  false },
    { JCE_EDITOR_COMP_SLOT_NAV_AGENT,     0,                                  "Nav Agent",             "comp.navAgent",             true,  true,  false },
    { JCE_EDITOR_COMP_SLOT_SEQUENCE_PLAYER, 0,                                "Sequence Player",       "comp.sequencePlayer",       true,  true,  false },
};

static const JceEditorComponentSlot kDefaultOrder[] = {
    JCE_COMP_FLAG_TRANSFORM,
    JCE_EDITOR_COMP_SLOT_LIGHT_GROUP,
    JCE_COMP_FLAG_CAMERA,
    JCE_COMP_FLAG_MESH_RENDERER,
    JCE_COMP_FLAG_SPRITE_RENDERER,
    JCE_COMP_FLAG_ANIMATOR,
    JCE_COMP_FLAG_SKELETAL_ANIMATOR,
    JCE_EDITOR_COMP_SLOT_IK_CONSTRAINTS,
    JCE_COMP_FLAG_RIGIDBODY,
    JCE_COMP_FLAG_BOX_COLLIDER,
    JCE_COMP_FLAG_SPHERE_COLLIDER,
    JCE_COMP_FLAG_CHARACTER_CONTROLLER,
    JCE_COMP_FLAG_AUDIO_SOURCE,
    JCE_EDITOR_COMP_SLOT_VIDEO_PLAYER,
    JCE_COMP_FLAG_SCRIPT,
    JCE_COMP_FLAG_SKYBOX,
    JCE_COMP_FLAG_SPRITE_ANIMATOR,
    JCE_COMP_FLAG_CONSTRAINT,
    JCE_COMP_FLAG_TERRAIN,
    JCE_COMP_FLAG_RIGIDBODY_2D,
    JCE_COMP_FLAG_PARTICLE_EMITTER,
    JCE_COMP_FLAG_BEHAVIOR_TREE,
    JCE_COMP_FLAG_LOD_GROUP,
    JCE_COMP_FLAG_VIRTUAL_CAMERA,
    JCE_COMP_FLAG_TRIGGER_VOLUME,
    JCE_COMP_FLAG_CAPSULE_COLLIDER,
    JCE_COMP_FLAG_MESH_COLLIDER,
    JCE_EDITOR_COMP_SLOT_COMPOUND_COLLIDER,
    JCE_COMP_FLAG_COLLIDER_2D,
    JCE_COMP_FLAG_TRAIL_RENDERER,
    JCE_COMP_FLAG_LINE_RENDERER,
    JCE_COMP_FLAG_REFLECTION_PROBE,
    JCE_COMP_FLAG_DECAL,
    JCE_COMP_FLAG_LIGHT_PROBE_GROUP,
    JCE_COMP_FLAG_AUDIO_LISTENER,
    JCE_COMP_FLAG_AUDIO_REVERB_ZONE,
    JCE_COMP_FLAG_AUDIO_OCCLUSION,
    JCE_COMP_FLAG_SPAWN_MANAGER,
    JCE_COMP_FLAG_WEAPON,
    JCE_COMP_FLAG_SAVE_POINT,
    JCE_COMP_FLAG_WHEEL_COLLIDER,
    JCE_COMP_FLAG_CONSTANT_FORCE,
    JCE_COMP_FLAG_CONFIGURABLE_JOINT,
    JCE_COMP_FLAG_JOINT_2D,
    JCE_COMP_FLAG_BILLBOARD_RENDERER,
    JCE_COMP_FLAG_CANVAS,
    JCE_COMP_FLAG_CANVAS_GROUP,
    JCE_COMP_FLAG_LAYOUT_GROUP,
    JCE_COMP_FLAG_UI_IMAGE,
    JCE_COMP_FLAG_UI_TEXT,
    JCE_COMP_FLAG_UI_BUTTON,
    JCE_COMP_FLAG_CLOTH,
    JCE_COMP_FLAG_NETWORK_OBJECT,
    JCE_COMP_FLAG_NET_TRANSFORM,
    JCE_COMP_FLAG_NET_ANIMATOR,
    JCE_COMP_FLAG_NET_RIGIDBODY,
    JCE_COMP_FLAG_TILEMAP,
    JCE_COMP_FLAG_TILEMAP_COLLIDER_2D,
    JCE_COMP_FLAG_AVATAR,
    JCE_COMP_FLAG_VOLUME,
    JCE_COMP_FLAG_OCCLUSION_PORTAL,
    JCE_EDITOR_COMP_SLOT_NAV_AGENT,
    JCE_EDITOR_COMP_SLOT_SEQUENCE_PLAYER,
};
/* clang-format on */

} /* namespace */

int jce_editor_component_descriptor_count(void)
{
    return (int)(sizeof(kDescriptors) / sizeof(kDescriptors[0]));
}

const JceEditorComponentDescriptor *jce_editor_component_descriptor_at(
    int index)
{
    if (index < 0 || index >= jce_editor_component_descriptor_count())
        return NULL;
    return &kDescriptors[index];
}

const JceEditorComponentDescriptor *jce_editor_component_find(
    JceEditorComponentSlot slot)
{
    for (const JceEditorComponentDescriptor &d : kDescriptors) {
        if (d.slot == slot)
            return &d;
    }
    return NULL;
}

const JceEditorComponentDescriptor *jce_editor_component_find_legacy(
    uint64_t legacy_flag)
{
    if (legacy_flag == 0)
        return NULL;
    for (const JceEditorComponentDescriptor &d : kDescriptors) {
        if (d.legacy_flag == legacy_flag)
            return &d;
    }
    return NULL;
}

int jce_editor_component_default_order_count(void)
{
    return (int)(sizeof(kDefaultOrder) / sizeof(kDefaultOrder[0]));
}

JceEditorComponentSlot jce_editor_component_default_order_at(int index)
{
    if (index < 0 || index >= jce_editor_component_default_order_count())
        return 0;
    return kDefaultOrder[index];
}

bool jce_editor_component_slot_is_legacy_flag(JceEditorComponentSlot slot)
{
    const JceEditorComponentDescriptor *d = jce_editor_component_find(slot);
    return d && d->legacy_flag != 0 && d->legacy_flag == slot;
}

bool jce_editor_component_slot_is_light_group(JceEditorComponentSlot slot)
{
    return slot == JCE_EDITOR_COMP_SLOT_LIGHT_GROUP;
}

bool jce_editor_component_slot_is_compound_collider(
    JceEditorComponentSlot slot)
{
    return slot == JCE_EDITOR_COMP_SLOT_COMPOUND_COLLIDER;
}

bool jce_editor_component_slot_is_video_player(
    JceEditorComponentSlot slot)
{
    return slot == JCE_EDITOR_COMP_SLOT_VIDEO_PLAYER;
}

bool jce_editor_component_slot_is_nav_agent(
    JceEditorComponentSlot slot)
{
    return slot == JCE_EDITOR_COMP_SLOT_NAV_AGENT;
}

bool jce_editor_component_slot_is_ik_constraints(
    JceEditorComponentSlot slot)
{
    return slot == JCE_EDITOR_COMP_SLOT_IK_CONSTRAINTS;
}

bool jce_editor_component_slot_is_sequence_player(
    JceEditorComponentSlot slot)
{
    return slot == JCE_EDITOR_COMP_SLOT_SEQUENCE_PLAYER;
}

const char *jce_editor_component_display_name(JceEditorComponentSlot slot)
{
    const JceEditorComponentDescriptor *d = jce_editor_component_find(slot);
    return d ? d->display_name : "Unknown";
}

const char *jce_editor_component_i18n_key(JceEditorComponentSlot slot)
{
    const JceEditorComponentDescriptor *d = jce_editor_component_find(slot);
    return d ? d->i18n_key : NULL;
}

bool jce_editor_component_slot_present(JceScene *scene,
                                       JceEntity entity,
                                       uint64_t legacy_flags,
                                       JceEditorComponentSlot slot)
{
    if (jce_editor_component_slot_is_light_group(slot))
        return (legacy_flags & JCE_EDITOR_COMPONENT_LIGHT_MASK) != 0;
    if (jce_editor_component_slot_is_compound_collider(slot))
        return scene && jce_scene_has_compound_collider(scene, entity);
    if (jce_editor_component_slot_is_video_player(slot))
        return scene && jce_scene_has_video_player(scene, entity);
    if (jce_editor_component_slot_is_nav_agent(slot))
        return scene && jce_scene_has_nav_agent(scene, entity);
    if (jce_editor_component_slot_is_ik_constraints(slot))
        return scene && jce_scene_has_ik_constraints(scene, entity);
    if (jce_editor_component_slot_is_sequence_player(slot))
        return scene && jce_scene_has_sequence_player(scene, entity);
    const JceEditorComponentDescriptor *d = jce_editor_component_find(slot);
    return d && d->legacy_flag != 0 && (legacy_flags & d->legacy_flag) != 0;
}

void jce_editor_component_compound_default(
    JceCompoundColliderComponent *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    out->mode          = 0;
    out->split         = 0;
    out->is_static     = true;
    out->detect_naming = true;
    out->friction      = 0.5f;
}
