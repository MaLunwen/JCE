#ifndef JCE_MODEL_TOON_HAIR_INTERNAL_H
#define JCE_MODEL_TOON_HAIR_INTERNAL_H

#include <jce/renderer/jce_model.h>

typedef struct {
    uint16_t view, program, texture;
} JceModelToonHairPass;

bool jce_model_has_toon_hair(const JceModel *model);
/* Scope to a single direct skinned toon draw on the render thread. */
void jce_model_set_toon_hair_pass(const JceModelToonHairPass *pass);
bool jce_pbr_material_bind_toon_hair_depth(uint16_t texture);

#endif
