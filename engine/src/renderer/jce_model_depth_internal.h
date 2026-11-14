/* Scene batching must not erase a model's alpha-cutout material identity. */
#ifndef JCE_MODEL_DEPTH_INTERNAL_H
#define JCE_MODEL_DEPTH_INTERNAL_H
#include <jce/renderer/jce_model.h>
bool jce_model_depth_masked(const JceModel *model);
#endif
