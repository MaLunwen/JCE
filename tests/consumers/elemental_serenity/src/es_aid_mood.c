/* Legacy mood compatibility; active scene policy uses es_scene_orchestrator.
 * Retired internal transport code is preserved in the ignored local archive.
 */
#include "es_aid_mood.h"


struct EsAidMood { int unused; };

EsAidMood *es_aid_mood_create(JceRuntime *runtime, JceScene *scene)
{
    (void)runtime; (void)scene;
    return NULL;
}
void es_aid_mood_destroy(EsAidMood *m) { (void)m; }
void es_aid_mood_update(EsAidMood *m, float dt, const struct JceInput *input)
{
    (void)m; (void)dt; (void)input;
}
void es_aid_mood_ui(EsAidMood *m, struct JceUICanvas *uc, float dt)
{
    (void)m; (void)uc; (void)dt;
}
void es_aid_mood_debug_draw(EsAidMood *m, struct JceRenderer *renderer,
                            struct JceWindow *window, const struct JceCamera *cam)
{
    (void)m; (void)renderer; (void)window; (void)cam;
}
bool es_aid_mood_is_generating(const EsAidMood *m) { (void)m; return false; }
