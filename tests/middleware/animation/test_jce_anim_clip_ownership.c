/* Only public SDK headers: a parsed clip's entire lifetime is available. */
#include <jce/api_animation.h>
#include "unity.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_owned_clip_lifetime(void)
{
    static const char document[] =
        "{\"name\":\"Owned\",\"duration\":1,\"channels\":[{\"joint\":0,"
        "\"target\":\"translation\",\"interp\":\"linear\",\"times\":[0,1],"
        "\"values\":[[0,0,0],[2,0,0]]}]}";
    JceJoint joint;
    memset(&joint, 0, sizeof(joint));
    joint.parent = -1;
    joint.rest_rotation = jce_q_identity();
    joint.rest_scale = jce_v3(1, 1, 1);
    joint.local_transform = jce_m4_identity();
    joint.inverse_bind_matrix = jce_m4_identity();
    JceSkeleton *skeleton = jce_skeleton_create(&joint, 1);
    TEST_ASSERT_NOT_NULL(skeleton);
    for (unsigned i = 0; i < 32u; ++i) {
        JceAnimClip *clip = jce_anim_clip_parse(document, sizeof(document) - 1u);
        TEST_ASSERT_NOT_NULL(clip);
        JceAnimPlayer *player = jce_anim_player_create(skeleton);
        TEST_ASSERT_NOT_NULL(player);
        jce_anim_player_play(player, clip, false, 1.0f);
        jce_mat4 pose;
        TEST_ASSERT_EQUAL_UINT32(1, jce_anim_player_update(player, 0.25f, &pose, 1));
        TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.5f, JCE_M4_PTR(pose)[12]);
        jce_anim_player_destroy(player);
        jce_anim_clip_destroy(clip);
    }
    jce_skeleton_destroy(skeleton);
    jce_anim_clip_destroy(NULL);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_owned_clip_lifetime);
    return UNITY_END();
}
