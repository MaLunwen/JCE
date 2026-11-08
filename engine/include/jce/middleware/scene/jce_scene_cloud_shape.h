#ifndef JCE_SCENE_CLOUD_SHAPE_H
#define JCE_SCENE_CLOUD_SHAPE_H

#include <jce/middleware/scene/jce_scene.h>
JCE_EXTERN_C_BEGIN

#define JCE_SCENE_CLOUD_SHAPE_VERSION 1u

/* Optional shared density shaping for the visible cloud layer and its shadow. */
typedef struct JceSceneCloudShapeSettings {
    uint32_t struct_size;
    uint32_t version;
    float detail_erosion; /* [0,0.95], default 0.45 */
    float curl_metres;    /* horizontal displacement [0,256], default 0 */
} JceSceneCloudShapeSettings;

JCE_API JceSceneCloudShapeSettings JCE_CALL jce_scene_cloud_shape_default(void);

/* NULL clears the block. Invalid input leaves the scene unchanged. */
JCE_API bool JCE_CALL jce_scene_set_cloud_shape_settings(JceScene *scene,
    const JceSceneCloudShapeSettings *value);

/* Borrowed authored settings, or NULL. Valid until settings change or clear. */
JCE_API const JceSceneCloudShapeSettings *JCE_CALL
jce_scene_get_cloud_shape_settings(const JceScene *scene);

JCE_EXTERN_C_END
#endif
