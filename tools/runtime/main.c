/* Stock SDK consumer: same default runtime and traced application lifecycle. */
#include "stock_payload.h"
#include "stock_bundle.h"
#include <jce/api_app.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_alloc.h>
#include <jce/os/platform/jce_mmap.h>
#include <jce/middleware/script/jce_script_vm.h>
#include <jce_script_register_linked.h>
#include <string.h>
static JceMmap *stock_image;
static size_t stock_bundle_size;
static uint8_t *stock_bundle_bytes;
static JceStockBundle *stock_bundle;
static bool stock_entered;
static bool stock_scene_allowed(const char *path)
{
    return jce_stock_bundle_scene_allowed(stock_bundle, path);
}
#define JCE_DEFAULT_MAIN_PACKAGED_ONLY 1
#define JCE_DEFAULT_MAIN_PACKAGED_SCENE_ALLOWED(path) stock_scene_allowed(path)
#define JCE_PROJECT_EMBEDDED_BUNDLE_COUNT 1
typedef struct { const char *name; const void *data; const size_t *size; }
    JceProjectEmbeddedBundle;
static JceProjectEmbeddedBundle jce_project_embedded_bundles[2] = {
    {"stock_payload", NULL, &stock_bundle_size}, {NULL, NULL, NULL}
};
#include <jce/application/jce_default_main.inc.h>
static bool stock_init(const JceServices *services, void *user_data)
{
    char path[32768];
    uint64_t observed_size = 0;
    JceStockPayload payload;
    if (!jce_fs_host_get_executable_path(path, sizeof(path))) {
        LOG_ERROR("stock", "%s", "own executable path is unavailable");
        return false;
    }
    /* Reject an observed oversized image before mapping/fallback allocation.
     * The actual mapped length is checked again by the envelope reader. */
    if (!jce_fs_host_get_size(path, &observed_size) ||
        observed_size < JCE_STOCK_FOOTER_SIZE || observed_size > JCE_STOCK_IMAGE_LIMIT)
        return false;
    stock_image = jce_mmap_open(path);
    if (!stock_image || jce_mmap_size(stock_image) != observed_size ||
        !jce_stock_payload_read(jce_mmap_data(stock_image),
                                jce_mmap_size(stock_image), &payload)) {
        LOG_ERROR("stock", "%s", "invalid or missing stock application envelope");
        return false;
    }
    if (!jce_stock_payload_decrypt(&payload, &stock_bundle_bytes, &stock_bundle_size)) {
        LOG_ERROR("stock", "%s", "project archive authentication or budget failed");
        return false;
    }
    stock_bundle = jce_stock_bundle_open(stock_bundle_bytes, stock_bundle_size);
    if (!stock_bundle) {
        LOG_ERROR("stock", "%s", "Bundle is not a verified self-contained Lua/JS scene");
        return false;
    }
    /* Exactly one non-Lua backend (JS) is linked by the declared CMake set.
     * Register before default runtime creates any script instance. */
    if (jce_script_register_linked_languages(NULL) != 1 ||
        !jce_script_vm_find("lua") || !jce_script_vm_find("js")) {
        LOG_ERROR("stock", "%s", "required embedded Lua/JS backends are unavailable");
        return false;
    }
    jce_project_embedded_bundles[0].data = stock_bundle_bytes;
    /* Engine destruction calls exit even if init fails; enter once, release
     * Bundle/VFS/runtime borrowers before releasing the envelope buffer. */
    stock_entered = true;
    return app_init(services, user_data) && jce_default_main_get_scene() != NULL;
}
static void stock_exit(void *user_data)
{
    if (stock_entered) app_exit(user_data);
    stock_entered = false;
    jce_stock_bundle_close(stock_bundle);
    stock_bundle = NULL;
    jce_project_embedded_bundles[0].data = NULL;
    stock_bundle_size = 0;
    jce_free(stock_bundle_bytes);
    stock_bundle_bytes = NULL;
    jce_mmap_close(stock_image);
    stock_image = NULL;
}
JceAppDesc stock_get_desc(void)
{
    JceAppDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.name = "JCE Application";
    desc.init = stock_init;
    desc.exit = stock_exit;
    desc.update = app_update;
    desc.draw = app_draw;
    desc.window_width = 1280;
    desc.window_height = 720;
    return desc;
}
JCE_MAIN(stock_get_desc)
