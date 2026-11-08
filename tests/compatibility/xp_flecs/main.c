/* Real XP entropy and the isolated typed C99 control share one executable. */
#include <jce/os/platform/jce_entropy.h>
#include <stdio.h>
int jce_xp_flecs_control_main(int argc, char **argv);
int main(int argc, char **argv)
{
    unsigned char bytes[16];
    if (!jce_host_random_bytes(bytes, sizeof bytes)) {
        puts("JCE_XP_FLECS_ENTROPY_FAILED");
        return 90;
    }
    return jce_xp_flecs_control_main(argc, argv);
}
