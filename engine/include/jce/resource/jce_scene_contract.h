/*
 * jce_scene_contract.h  Shared scene JSON contract metadata.
 *
 * This header defines the stable wrapper keys/version used by both
 * runtime scene serialization and editor-side scene persistence.
 */

#ifndef JCE_SCENE_CONTRACT_H
#define JCE_SCENE_CONTRACT_H


#include <jce/os/core/jce_defs.h>
#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_SCENE_CONTRACT_NAME      "jce.scene"
#define JCE_SCENE_CONTRACT_MAJOR     1u
#define JCE_SCENE_CONTRACT_MINOR     0u

#define JCE_SCENE_CONTRACT_KEY       "contract"
#define JCE_SCENE_CONTRACT_NAME_KEY  "name"
#define JCE_SCENE_CONTRACT_MAJOR_KEY "major"
#define JCE_SCENE_CONTRACT_MINOR_KEY "minor"

#define JCE_SCENE_ROOT_KEY           "scene"
#define JCE_SCENE_VERSION_KEY        "version"
#define JCE_SCENE_ENTITIES_KEY       "entities"

static inline bool jce_scene_contract_major_compatible(uint32_t major)
{
    return major == JCE_SCENE_CONTRACT_MAJOR;
}

JCE_EXTERN_C_END

#endif /* JCE_SCENE_CONTRACT_H */
