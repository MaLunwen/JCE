/* Pak owner only: immutable inspection admission and the canonical VFS bridge. */
#ifndef JCE_PAK_MOUNT_INTERNAL_H
#define JCE_PAK_MOUNT_INTERNAL_H
#include <jce/resource/jce_pak_loader.h>
/* Applied before publication of an inspected Bundle; never cleared. */
void jce_pak_mark_inspection_only_internal(JcePakArchive *pak);
/* Both Engine bootstrap and catalog-less Bundle loading use this one tuple. */
void jce_pak_register_filesystem_provider_internal(void);
#endif
