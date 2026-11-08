/* Private dependency-inversion bridge. Registration/mount mutation is serial
 * on the filesystem owner thread, exactly as the existing public provider API.
 * The public three-callback provider ABI and opaque custom providers are unchanged. */
#ifndef JCE_FILESYSTEM_PAK_INTERNAL_H
#define JCE_FILESYSTEM_PAK_INTERNAL_H
#include <jce/os/core/jce_filesystem.h>
typedef bool (JCE_CALL *JceFsPakMountAllowedInternal)(const JcePakArchive *pak);
/* Resource registers its one canonical callback tuple and matching predicate.
 * The public setter recognizes that tuple, including a borrowed tuple copy,
 * when restored after a custom provider. Unknown callbacks never reach the
 * native-Pak predicate. This is a same-handle SDK admission contract, not a
 * security boundary against custom decoding or independently reopened bytes. */
void JCE_CALL jce_fs_register_pak_provider_internal(
    const JceFsPakProvider *provider, JceFsPakMountAllowedInternal mount_allowed);
#endif
