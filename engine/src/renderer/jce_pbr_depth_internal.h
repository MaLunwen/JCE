/* Private material-aware depth programs. Opaque programs remain untextured. */
#ifndef JCE_PBR_DEPTH_INTERNAL_H
#define JCE_PBR_DEPTH_INTERNAL_H
#include <jce/renderer/jce_pbr_material.h>

typedef enum JcePbrDepthKind {
    JCE_PBR_DEPTH_STATIC,
    JCE_PBR_DEPTH_INSTANCED,
    JCE_PBR_DEPTH_SKINNED,
    JCE_PBR_DEPTH_SKINNED_INSTANCED,
    JCE_PBR_DEPTH_GBUFFER_STATIC,
    JCE_PBR_DEPTH_GBUFFER_SKINNED,
    JCE_PBR_DEPTH_VELOCITY_STATIC,
    JCE_PBR_DEPTH_VELOCITY_SKINNED,
    JCE_PBR_DEPTH_VELOCITY_SKINNED_INSTANCED,
    JCE_PBR_DEPTH_COUNT
} JcePbrDepthKind;

/* Borrowed until the existing material shutdown; missing variants stay invalid. */
JceShaderHandle jce_pbr_depth_program(JcePbrDepthKind kind);
/* Bind only MASK materials; false means no valid texture/uniform resources. */
bool jce_pbr_material_bind_depth(const JcePbrMaterial *material);
#endif
