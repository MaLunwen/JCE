/* SDK-only scene bundle producer. Extra argv paths are explicit dependency roots. */
#include <jce/resource/jce_bundle_pack.h>
#include <stdio.h>
static void output(JceBundlePackLogLevel level, const char *message, void *user)
{
    (void)level; (void)user; puts(message);
}
int main(int argc, char **argv)
{
    JceBundlePackOptions options = {0};
    const char *scene;
    if (argc < 4) { fputs("Usage: td_pack_scene scene resource_root output_dir [asset...]\n", stderr); return 2; }
    scene = argv[1]; options.scene_files = &scene; options.scene_file_count = 1;
    options.resource_root = argv[2]; options.out_dir = argv[3];
    options.single_file_mode = true; options.single_bundle_id = "tower-defense";
    options.always_included = (const char *const *)(argv + 4);
    options.always_included_count = argc - 4;
    return jce_bundle_pack_run(&options, output, NULL);
}
