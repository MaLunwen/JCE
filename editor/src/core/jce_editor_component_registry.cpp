/*
 * jce_editor_component_registry.cpp
 *
 * Static identity table (engine_name / display metadata, plus the
 * deprecated flag-compat slot) and the runtime-resolved engine comp_id
 * with runtime-registered behavior hooks
 * (draw / add_default / dup_fixup / pre_remove).
 */

#include "jce_editor_component_registry.h"

#include <stddef.h>
#include <string.h>

namespace {

/* Rows are mutable: comp_id is resolved at first use and the behavior
 * hooks are installed at runtime (the omitted trailing aggregate members
 * value-initialize to 0/NULL; ensure_comp_ids() turns the zero comp_id
 * into JCE_COMP_ID_INVALID before anything can read it). */
/* clang-format off */
static JceEditorComponentDescriptor kDescriptors[] = {
    { JCE_COMP_FLAG_TRANSFORM,            JCE_COMP_FLAG_TRANSFORM,            "Transform",         "Transform",             "comp.transform",            false, false, true  },
    { 0,                                  0,                                  "Pivot",             "Pivot",                 "comp.pivot",                true,  true,  true  },
    { 0,                                  0,                                  "Light",             "Light",                 NULL,                        false, true,  false },
    { JCE_COMP_FLAG_DIR_LIGHT,            JCE_COMP_FLAG_DIR_LIGHT,            "DirectionalLight",  "Directional Light",     "comp.dirLight",             true,  true,  false },
    { JCE_COMP_FLAG_POINT_LIGHT,          JCE_COMP_FLAG_POINT_LIGHT,          "PointLight",        "Point Light",           "comp.pointLight",           true,  true,  false },
    { JCE_COMP_FLAG_SPOT_LIGHT,           JCE_COMP_FLAG_SPOT_LIGHT,           "SpotLight",         "Spot Light",            "comp.spotLight",            true,  true,  false },
    { JCE_COMP_FLAG_CAMERA,               JCE_COMP_FLAG_CAMERA,               "Camera",            "Camera",                "comp.camera",               true,  true,  true  },
    { JCE_COMP_FLAG_MESH_RENDERER,        JCE_COMP_FLAG_MESH_RENDERER,        "MeshRenderer",      "Mesh Renderer",         "comp.meshRenderer",         true,  true,  true  },
    { JCE_COMP_FLAG_SPRITE_RENDERER,      JCE_COMP_FLAG_SPRITE_RENDERER,      "SpriteRenderer",    "Sprite Renderer",       "comp.spriteRenderer",       true,  true,  true  },
    { JCE_COMP_FLAG_ANIMATOR,             JCE_COMP_FLAG_ANIMATOR,             "Animator",          "Animator",              "comp.animator",             true,  true,  false },
    { JCE_COMP_FLAG_SKELETAL_ANIMATOR,    JCE_COMP_FLAG_SKELETAL_ANIMATOR,    "SkeletalAnimator",  "Skeletal Animator",     "comp.skeletalAnimator",     true,  true,  true  },
    { 0,                                  0,                                  "IkConstraints",     "IK Constraints",        "comp.ikConstraints",        true,  true,  false },
    { 0,                                  0,                                  "FootIk",            "Foot IK",               "comp.footIk",               true,  true,  false },
    { 0,                                  0,                                  "FullBodyIk",        "Full-Body IK",          "comp.fullBodyIk",           true,  true,  false },
    { JCE_COMP_FLAG_RIGIDBODY,            JCE_COMP_FLAG_RIGIDBODY,            "Rigidbody",         "Rigidbody",             "comp.rigidbody",            true,  true,  true  },
    { JCE_COMP_FLAG_BOX_COLLIDER,         JCE_COMP_FLAG_BOX_COLLIDER,         "BoxCollider",       "Box Collider",          "comp.boxCollider",          true,  true,  true  },
    { JCE_COMP_FLAG_SPHERE_COLLIDER,      JCE_COMP_FLAG_SPHERE_COLLIDER,      "SphereCollider",    "Sphere Collider",       "comp.sphereCollider",       true,  true,  true  },
    { JCE_COMP_FLAG_CHARACTER_CONTROLLER, JCE_COMP_FLAG_CHARACTER_CONTROLLER, "CharacterController", "Character Controller", "comp.characterController", true,  true,  false },
    { JCE_COMP_FLAG_AUDIO_SOURCE,         JCE_COMP_FLAG_AUDIO_SOURCE,         "AudioSource",       "Audio Source",          "comp.audioSource",          true,  true,  true  },
    { 0,                                  0,                                  "MusicTrack",        "Music Track",           "comp.musicTrack",           true,  true,  true  },
    { 0,                                  0,                                  "VideoPlayer",       "Video Player",          "comp.videoPlayer",          true,  true,  false },
    { JCE_COMP_FLAG_SCRIPT,               JCE_COMP_FLAG_SCRIPT,               "Script",            "Script",                "comp.script",               true,  true,  false },
    { JCE_COMP_FLAG_SKYBOX,               JCE_COMP_FLAG_SKYBOX,               "Skybox",            "Skybox",                "comp.skybox",               true,  true,  false },
    { JCE_COMP_FLAG_SPRITE_ANIMATOR,      JCE_COMP_FLAG_SPRITE_ANIMATOR,      "SpriteAnimator",    "Sprite Animator",       "comp.spriteAnimator",       true,  true,  false },
    { JCE_COMP_FLAG_CONSTRAINT,           JCE_COMP_FLAG_CONSTRAINT,           "Constraint",        "Constraint",            "comp.constraint",           true,  true,  true  },
    { JCE_COMP_FLAG_TERRAIN,              JCE_COMP_FLAG_TERRAIN,              "Terrain",           "Terrain",               "comp.terrain",              true,  true,  false },
    { 0,                                  0,                                  "VegetationScatter", "Vegetation Scatter",    "comp.vegetationScatter",    true,  true,  false },
    { 0,                                  0,                                  "GrassField",        "Grass Field",           "comp.grassField",           true,  true,  false },
    { 0,                                  0,                                  "FoliageCluster",    "Foliage Cluster",       "comp.foliageCluster",       true,  true,  false },
    { 0,                                  0,                                  "Water",             "Water",                 "comp.water",                true,  true,  false },
    { 0,                                  0,                                  "Buoyancy",          "Buoyancy",              "comp.buoyancy",             true,  true,  false },
    { JCE_COMP_FLAG_RIGIDBODY_2D,         JCE_COMP_FLAG_RIGIDBODY_2D,         "Rigidbody2D",       "Rigidbody 2D",          "comp.rigidbody2d",          true,  true,  false },
    { JCE_COMP_FLAG_PARTICLE_EMITTER,     JCE_COMP_FLAG_PARTICLE_EMITTER,     "ParticleEmitter",   "Particle Emitter",      "comp.particleEmitter",      true,  true,  false },
    { JCE_COMP_FLAG_BEHAVIOR_TREE,        JCE_COMP_FLAG_BEHAVIOR_TREE,        "BehaviorTree",      "Behavior Tree",         "comp.behaviorTree",         true,  true,  false },
    { JCE_COMP_FLAG_EDITOR_META,          JCE_COMP_FLAG_EDITOR_META,          "EditorMeta",        "Editor Meta",           "comp.editorMeta",           false, true,  false },
    { JCE_COMP_FLAG_LOD_GROUP,            JCE_COMP_FLAG_LOD_GROUP,            "LODGroup",          "LOD Group",             "comp.lodGroup",             true,  true,  false },
    { JCE_COMP_FLAG_VIRTUAL_CAMERA,       JCE_COMP_FLAG_VIRTUAL_CAMERA,       "VirtualCamera",     "Virtual Camera",        "comp.virtualCamera",        true,  true,  false },
    { JCE_COMP_FLAG_TRIGGER_VOLUME,       JCE_COMP_FLAG_TRIGGER_VOLUME,       "TriggerVolume",     "Trigger Volume",        "comp.triggerVolume",        true,  true,  false },
    { JCE_COMP_FLAG_CAPSULE_COLLIDER,     JCE_COMP_FLAG_CAPSULE_COLLIDER,     "CapsuleCollider",   "Capsule Collider",      "comp.capsuleCollider",      true,  true,  true  },
    { JCE_COMP_FLAG_MESH_COLLIDER,        JCE_COMP_FLAG_MESH_COLLIDER,        "MeshCollider",      "Mesh Collider",         "comp.meshCollider",         true,  true,  true  },
    { 0,                                  0,                                  "CompoundCollider",  "Compound Collider",     "comp.compoundCollider",     true,  true,  true  },
    { JCE_COMP_FLAG_COLLIDER_2D,          JCE_COMP_FLAG_COLLIDER_2D,          "Collider2D",        "Collider 2D",           "comp.collider2d",           true,  true,  false },
    { JCE_COMP_FLAG_TRAIL_RENDERER,       JCE_COMP_FLAG_TRAIL_RENDERER,       "TrailRenderer",     "Trail Renderer",        "comp.trailRenderer",        true,  true,  false },
    { JCE_COMP_FLAG_LINE_RENDERER,        JCE_COMP_FLAG_LINE_RENDERER,        "LineRenderer",      "Line Renderer",         "comp.lineRenderer",         true,  true,  false },
    { JCE_COMP_FLAG_REFLECTION_PROBE,     JCE_COMP_FLAG_REFLECTION_PROBE,     "ReflectionProbe",   "Reflection Probe",      "comp.reflectionProbe",      true,  true,  false },
    { JCE_COMP_FLAG_DECAL,                JCE_COMP_FLAG_DECAL,                "Decal",             "Decal Projector",       "comp.decal",                true,  true,  false },
    { JCE_COMP_FLAG_LIGHT_PROBE_GROUP,    JCE_COMP_FLAG_LIGHT_PROBE_GROUP,    "LightProbeGroup",   "Light Probe Group",     "comp.lightProbeGroup",      true,  true,  false },
    { JCE_COMP_FLAG_AUDIO_LISTENER,       JCE_COMP_FLAG_AUDIO_LISTENER,       "AudioListener",     "Audio Listener",        "comp.audioListener",        true,  true,  false },
    { JCE_COMP_FLAG_AUDIO_REVERB_ZONE,    JCE_COMP_FLAG_AUDIO_REVERB_ZONE,    "AudioReverbZone",   "Audio Reverb Zone",     "comp.audioReverbZone",      true,  true,  false },
    { JCE_COMP_FLAG_AUDIO_OCCLUSION,      JCE_COMP_FLAG_AUDIO_OCCLUSION,      "AudioOcclusion",    "Audio Occlusion",       "comp.audioOcclusion",       true,  true,  false },
    { JCE_COMP_FLAG_SPAWN_MANAGER,        JCE_COMP_FLAG_SPAWN_MANAGER,        "SpawnManager",      "Spawn Manager",         "comp.spawnManager",         true,  true,  false },
    { JCE_COMP_FLAG_WEAPON,               JCE_COMP_FLAG_WEAPON,               "Weapon",            "Weapon",                "comp.weapon",               true,  true,  false },
    { JCE_COMP_FLAG_SAVE_POINT,           JCE_COMP_FLAG_SAVE_POINT,           "SavePoint",         "Save Point",            "comp.savePoint",            true,  true,  false },
    { JCE_COMP_FLAG_WHEEL_COLLIDER,       JCE_COMP_FLAG_WHEEL_COLLIDER,       "WheelCollider",     "Wheel Collider",        "comp.wheelCollider",        true,  true,  false },
    { JCE_COMP_FLAG_CONSTANT_FORCE,       JCE_COMP_FLAG_CONSTANT_FORCE,       "ConstantForce",     "Constant Force",        "comp.constantForce",        true,  true,  false },
    { JCE_COMP_FLAG_CONFIGURABLE_JOINT,   JCE_COMP_FLAG_CONFIGURABLE_JOINT,   "ConfigurableJoint", "Configurable Joint",    "comp.configurableJoint",    true,  true,  false },
    { JCE_COMP_FLAG_JOINT_2D,             JCE_COMP_FLAG_JOINT_2D,             "Joint2D",           "Joint 2D",              "comp.joint2d",              true,  true,  false },
    { JCE_COMP_FLAG_BILLBOARD_RENDERER,   JCE_COMP_FLAG_BILLBOARD_RENDERER,   "BillboardRenderer", "Billboard Renderer",    "comp.billboardRenderer",    true,  true,  false },
    { JCE_COMP_FLAG_CANVAS,               JCE_COMP_FLAG_CANVAS,               "Canvas",            "Canvas",                "comp.canvas",               true,  true,  false },
    { JCE_COMP_FLAG_CANVAS_GROUP,         JCE_COMP_FLAG_CANVAS_GROUP,         "CanvasGroup",       "Canvas Group",          "comp.canvasGroup",          true,  true,  false },
    { JCE_COMP_FLAG_LAYOUT_GROUP,         JCE_COMP_FLAG_LAYOUT_GROUP,         "LayoutGroup",       "Layout Group",          "comp.layoutGroup",          true,  true,  false },
    { JCE_COMP_FLAG_UI_IMAGE,             JCE_COMP_FLAG_UI_IMAGE,             "UIImage",           "UI Image",              "comp.uiImage",              true,  true,  false },
    { JCE_COMP_FLAG_UI_TEXT,              JCE_COMP_FLAG_UI_TEXT,              "UIText",            "UI Text",               "comp.uiText",               true,  true,  false },
    { JCE_COMP_FLAG_UI_BUTTON,            JCE_COMP_FLAG_UI_BUTTON,            "UIButton",          "UI Button",             "comp.uiButton",             true,  true,  false },
    { 0,                                  0,                                  "UISlider",          "UI Slider",             "comp.uiSlider",             true,  true,  false },
    { 0,                                  0,                                  "UIToggle",          "UI Toggle",             "comp.uiToggle",             true,  true,  false },
    { 0,                                  0,                                  "UIInputField",      "UI Input Field",        "comp.uiInputField",         true,  true,  false },
    { 0,                                  0,                                  "UIScrollView",      "UI Scroll View",        "comp.uiScrollView",         true,  true,  false },
    { 0,                                  0,                                  "UIProgressBar",     "UI Progress Bar",       "comp.uiProgressBar",        true,  true,  false },
    { 0,                                  0,                                  "UIDropdown",        "UI Dropdown",           "comp.uiDropdown",           true,  true,  false },
    { JCE_COMP_FLAG_CLOTH,                JCE_COMP_FLAG_CLOTH,                "Cloth",             "Cloth",                 "comp.cloth",                true,  true,  false },
    { JCE_COMP_FLAG_NETWORK_OBJECT,       JCE_COMP_FLAG_NETWORK_OBJECT,       "NetworkObject",     "Network Object",        "comp.networkObject",        true,  true,  false },
    { JCE_COMP_FLAG_NET_TRANSFORM,        JCE_COMP_FLAG_NET_TRANSFORM,        "NetworkTransform",  "Network Transform",     "comp.netTransform",         true,  true,  false },
    { JCE_COMP_FLAG_NET_ANIMATOR,         JCE_COMP_FLAG_NET_ANIMATOR,         "NetworkAnimator",   "Network Animator",      "comp.netAnimator",          true,  true,  false },
    { JCE_COMP_FLAG_NET_RIGIDBODY,        JCE_COMP_FLAG_NET_RIGIDBODY,        "NetworkRigidbody",  "Network Rigidbody",     "comp.netRigidbody",         true,  true,  false },
    /* JCE_COMP_FLAG_VFX_GRAPH retired (v0.9.9): scene loads migrate it onto
     * Particle Emitter; the VFX Graph panel now exports .particles.json. */
    { JCE_COMP_FLAG_TILEMAP,              JCE_COMP_FLAG_TILEMAP,              "Tilemap",           "Tilemap",               "comp.tilemap",              true,  true,  false },
    { JCE_COMP_FLAG_TILEMAP_COLLIDER_2D,  JCE_COMP_FLAG_TILEMAP_COLLIDER_2D,  "TilemapCollider2D", "Tilemap Collider 2D",   "comp.tilemapCollider2d",    true,  true,  false },
    { JCE_COMP_FLAG_AVATAR,               JCE_COMP_FLAG_AVATAR,               "Avatar",            "Avatar",                "comp.avatar",               true,  true,  false },
    { JCE_COMP_FLAG_VOLUME,               JCE_COMP_FLAG_VOLUME,               "Volume",            "Volume",                "comp.volume",               true,  true,  false },
    { JCE_COMP_FLAG_OCCLUSION_PORTAL,     JCE_COMP_FLAG_OCCLUSION_PORTAL,     "OcclusionPortal",   "Occlusion Portal",      "comp.occlusionPortal",      true,  true,  false },
    { 0,                                  0,                                  "NavAgent",          "Nav Agent",             "comp.navAgent",             true,  true,  false },
    { 0,                                  0,                                  "SimLod",            "Simulation LOD",        "comp.simLod",               true,  true,  false },
    { 0,                                  0,                                  "SequencePlayer",    "Sequence Player",       "comp.sequencePlayer",       true,  true,  false },
    { 0,                                  0,                                  "MorphWeights",      "Morph Weights",         "comp.morphWeights",         true,  true,  false },
    { 0,                                  0,                                  "NetworkVariable",   "Network Variable",      "comp.networkVariable",      true,  true,  false },
    { 0,                                  0,                                  "GameplayAbilitySystem", "Gameplay Ability System", "comp.gas",            true,  true,  false },
    { 0,                                  0,                                  "Vehicle",           "Vehicle",               "comp.vehicle",              true,  true,  false },
    { 0,                                  0,                                  "SoftBody",          "Soft Body",             "comp.softBody",             true,  true,  false },
    { 0,                                  0,                                  "Ragdoll",           "Ragdoll",               "comp.ragdoll",              true,  true,  false },
    { 0,                                  0,                                  "Fracture",          "Fracture",              "comp.fracture",             true,  true,  false },
};

/* Default inspector section order, by canonical engine name ("Light" is
 * the unified light-group row). */
static const char *const kDefaultOrderNames[] = {
    "Transform",
    "Pivot",
    "Light",
    "Camera",
    "MeshRenderer",
    "SpriteRenderer",
    "Animator",
    "SkeletalAnimator",
    "IkConstraints",
    "FootIk",
    "FullBodyIk",
    "Rigidbody",
    "BoxCollider",
    "SphereCollider",
    "CharacterController",
    "AudioSource",
    "MusicTrack",
    "VideoPlayer",
    "Script",
    "Skybox",
    "SpriteAnimator",
    "Constraint",
    "Terrain",
    "VegetationScatter",
    "GrassField",
    "FoliageCluster",
    "Water",
    "Buoyancy",
    "Rigidbody2D",
    "ParticleEmitter",
    "BehaviorTree",
    "LODGroup",
    "VirtualCamera",
    "TriggerVolume",
    "CapsuleCollider",
    "MeshCollider",
    "CompoundCollider",
    "Collider2D",
    "TrailRenderer",
    "LineRenderer",
    "ReflectionProbe",
    "Decal",
    "LightProbeGroup",
    "AudioListener",
    "AudioReverbZone",
    "AudioOcclusion",
    "SpawnManager",
    "Weapon",
    "SavePoint",
    "WheelCollider",
    "ConstantForce",
    "ConfigurableJoint",
    "Joint2D",
    "BillboardRenderer",
    "Canvas",
    "CanvasGroup",
    "LayoutGroup",
    "UIImage",
    "UIText",
    "UIButton",
    "UISlider",
    "UIToggle",
    "UIInputField",
    "UIScrollView",
    "UIProgressBar",
    "UIDropdown",
    "Cloth",
    "NetworkObject",
    "NetworkTransform",
    "NetworkAnimator",
    "NetworkRigidbody",
    "Tilemap",
    "TilemapCollider2D",
    "Avatar",
    "Volume",
    "OcclusionPortal",
    "NavAgent",
    "SimLod",
    "SequencePlayer",
    "MorphWeights",
    "NetworkVariable",
    "GameplayAbilitySystem",
    "Vehicle",
    "SoftBody",
    "Ragdoll",
    "Fracture",
};
/* clang-format on */

/* Resolve every row's engine comp_id once the engine registry exists
 * (populated at first jce_scene_create).  Until then every comp_id reads
 * JCE_COMP_ID_INVALID, never the value-initialized 0 (which would alias
 * the Transform row). */
static void ensure_comp_ids(void)
{
    static int state = 0; /* 0 = untouched, 1 = invalidated, 2 = resolved */
    if (state == 2)
        return;
    if (state == 0) {
        for (JceEditorComponentDescriptor &d : kDescriptors)
            d.comp_id = JCE_COMP_ID_INVALID;
        state = 1;
    }
    if (jce_component_count() == 0)
        return; /* engine registry not populated yet — stay invalid */
    for (JceEditorComponentDescriptor &d : kDescriptors)
        d.comp_id = jce_component_find(d.engine_name);
    state = 2;
}

static JceEditorComponentDescriptor *find_mut(JceEditorComponentSlot slot)
{
    /* Rows without a legacy flag carry slot 0 — never slot-addressable. */
    if (slot == 0)
        return NULL;
    ensure_comp_ids();
    for (JceEditorComponentDescriptor &d : kDescriptors) {
        if (d.slot == slot)
            return &d;
    }
    return NULL;
}

static JceEditorComponentDescriptor *find_mut_by_name(const char *engine_name)
{
    if (!engine_name || !*engine_name)
        return NULL;
    ensure_comp_ids();
    for (JceEditorComponentDescriptor &d : kDescriptors) {
        if (strcmp(d.engine_name, engine_name) == 0)
            return &d;
    }
    return NULL;
}

static JceEditorComponentDescriptor *find_mut_by_id(int comp_id)
{
    if (comp_id == JCE_COMP_ID_INVALID)
        return NULL;
    ensure_comp_ids();
    for (JceEditorComponentDescriptor &d : kDescriptors) {
        if (d.comp_id == comp_id)
            return &d;
    }
    return NULL;
}

} /* namespace */

int jce_editor_component_descriptor_count(void)
{
    return (int)(sizeof(kDescriptors) / sizeof(kDescriptors[0]));
}

const JceEditorComponentDescriptor *jce_editor_component_descriptor_at(
    int index)
{
    ensure_comp_ids();
    if (index < 0 || index >= jce_editor_component_descriptor_count())
        return NULL;
    return &kDescriptors[index];
}

const JceEditorComponentDescriptor *jce_editor_component_find(
    JceEditorComponentSlot slot)
{
    return find_mut(slot);
}

const JceEditorComponentDescriptor *jce_editor_component_find_by_id(int comp_id)
{
    return find_mut_by_id(comp_id);
}

const JceEditorComponentDescriptor *jce_editor_component_find_by_name(
    const char *engine_name)
{
    return find_mut_by_name(engine_name);
}

int jce_editor_component_comp_id(JceEditorComponentSlot slot)
{
    const JceEditorComponentDescriptor *d = find_mut(slot);
    return d ? d->comp_id : JCE_COMP_ID_INVALID;
}

int jce_editor_component_default_order_count(void)
{
    return (int)(sizeof(kDefaultOrderNames) / sizeof(kDefaultOrderNames[0]));
}

int jce_editor_component_default_order_comp_id(int index)
{
    if (index < 0 || index >= jce_editor_component_default_order_count())
        return JCE_COMP_ID_INVALID;
    const JceEditorComponentDescriptor *d =
        find_mut_by_name(kDefaultOrderNames[index]);
    return d ? d->comp_id : JCE_COMP_ID_INVALID;
}

bool jce_editor_component_id_is_light_group(int comp_id)
{
    if (comp_id == JCE_COMP_ID_INVALID)
        return false;
    const JceEditorComponentDescriptor *d = find_mut_by_name("Light");
    return d && d->comp_id == comp_id;
}

void jce_editor_component_set_draw_fn(const char *engine_name,
                                      JceEditorCompDrawFn fn)
{
    JceEditorComponentDescriptor *d = find_mut_by_name(engine_name);
    if (d)
        d->draw = fn;
}

void jce_editor_component_set_add_default_fn(const char *engine_name,
                                             JceEditorCompAddDefaultFn fn)
{
    JceEditorComponentDescriptor *d = find_mut_by_name(engine_name);
    if (d)
        d->add_default = fn;
}

void jce_editor_component_set_dup_fixup_fn(const char *engine_name,
                                           JceEditorCompDupFixupFn fn)
{
    JceEditorComponentDescriptor *d = find_mut_by_name(engine_name);
    if (d)
        d->dup_fixup = fn;
}

void jce_editor_component_set_pre_remove_fn(const char *engine_name,
                                            JceEditorCompPreRemoveFn fn)
{
    JceEditorComponentDescriptor *d = find_mut_by_name(engine_name);
    if (d)
        d->pre_remove = fn;
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
