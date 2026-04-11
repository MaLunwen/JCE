/*
 * jce_filesystem.h  Virtual file system abstraction.
 *
 * Unifies PAK archive reads and loose-file reads behind a single API.
 * Supports mount points: each prefix maps to a backend (PAK or directory).
 *
 * Layer: Utilities (Layer 0 — depends on pak_loader).
 */

#ifndef JCE_FILESYSTEM_H
#define JCE_FILESYSTEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JceFileSystem JceFileSystem;
typedef struct PakArchive    PakArchive;

/* Opaque file handle for streaming reads. */
typedef struct JceFile JceFile;

/* -- Lifecycle ------------------------------------------------------ */

JceFileSystem *jce_fs_create(void);
void           jce_fs_destroy(JceFileSystem *fs);

/* -- Mount points --------------------------------------------------- */

/* Register a PAK archive as a read source.
   Assets stored in PAK are resolved by their embedded path. */
void jce_fs_mount_pak(JceFileSystem *fs, PakArchive *pak);

/* Register a loose-file directory as a read source.
   prefix: virtual path prefix (e.g. "assets/").
   directory: actual filesystem path (e.g. "C:/project/assets/").
   Loose-file mounts are checked BEFORE PAK (developer override). */
void jce_fs_mount_dir(JceFileSystem *fs, const char *prefix,
                      const char *directory);

/* -- File operations ------------------------------------------------ */

/* Open a virtual file for reading.  Returns NULL if not found. */
JceFile *jce_fs_open(const JceFileSystem *fs, const char *virtual_path);

/* Close a file. */
void jce_fs_close(JceFile *file);

/* Read up to buf_size bytes into buf.  Returns bytes actually read. */
size_t jce_fs_read(JceFile *file, void *buf, size_t buf_size);

/* Get total file size in bytes (0 if unknown). */
size_t jce_fs_size(const JceFile *file);

/* -- Convenience: load entire file into a newly allocated buffer ---- */

/* Allocate and read the entire file.  Caller must JCE_FREE(buf).
   Sets *out_size to the number of bytes read.
   Returns NULL if the file cannot be opened. */
void *jce_fs_read_all(const JceFileSystem *fs, const char *virtual_path,
                      size_t *out_size);

/* -- Query ---------------------------------------------------------- */

/* Returns true if the virtual path exists in any mounted source. */
bool jce_fs_exists(const JceFileSystem *fs, const char *virtual_path);

#ifdef __cplusplus
}
#endif

#endif /* JCE_FILESYSTEM_H */
