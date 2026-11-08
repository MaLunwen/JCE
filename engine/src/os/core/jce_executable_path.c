/* The private adapter must select native feature declarations first. It is
 * compiled by Core, like the existing filesystem-presence adapter; no
 * Core-to-Platform target dependency or public native types are introduced. */
#include "os/platform/jce_executable_path_native_internal.h"
#include <jce/os/core/jce_filesystem.h>

bool JCE_CALL jce_fs_host_get_executable_path(char *out, uint32_t out_size)
{
    return jce_fs_native_get_executable_path_internal(out, out_size);
}
