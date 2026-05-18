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


#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceFileSystem JceFileSystem;
typedef struct JcePakArchive    JcePakArchive;

/* Opaque file handle for streaming reads. */
typedef struct JceFile JceFile;

/* -- Lifecycle ------------------------------------------------------ */

JCE_API JceFileSystem *jce_fs_create(void);
JCE_API void JCE_CALL jce_fs_destroy(JceFileSystem *fs);

/* -- Mount points --------------------------------------------------- */

/* Register a PAK archive as a read source.
   Assets stored in PAK are resolved by their embedded path.
   This is the legacy single-pak entry point: the archive occupies the
   "unnamed" slot in the priority list and is replaced (not stacked)
   by subsequent calls to this function. */
JCE_API void JCE_CALL jce_fs_mount_pak(JceFileSystem *fs, JcePakArchive *pak);

/* Mount an additional named PAK at the FRONT of the priority list so
 * its assets shadow earlier mounts (Unity AssetBundle semantics).
 * `name` is copied internally — typically the bundle id.  Returns false
 * if the name is already mounted, the table is full, or out of memory.
 * Pair with jce_fs_unmount_pak_named() for symmetry. */
JCE_API bool JCE_CALL jce_fs_mount_pak_named(JceFileSystem *fs,
                                             const char *name,
                                             JcePakArchive *pak);

/* Remove a previously named-mounted PAK.  Returns true if found.  The
 * caller owns the JcePakArchive lifetime — this only detaches it from
 * the VFS lookup chain. */
JCE_API bool JCE_CALL jce_fs_unmount_pak_named(JceFileSystem *fs,
                                               const char *name);

/* Number of mounted PAK archives (named + legacy combined). */
JCE_API uint32_t JCE_CALL jce_fs_mounted_pak_count(const JceFileSystem *fs);

/* Register a loose-file directory as a read source.
   prefix: virtual path prefix (e.g. "assets/").
   directory: actual filesystem path (e.g. "C:/project/assets/").
   Loose-file mounts are checked BEFORE PAK (developer override). */
JCE_API void JCE_CALL jce_fs_mount_dir(JceFileSystem *fs, const char *prefix,
                      const char *directory);

/* -- File operations ------------------------------------------------ */

/* Open a virtual file for reading.  Returns NULL if not found. */
JCE_API JceFile *jce_fs_open(const JceFileSystem *fs, const char *virtual_path);

/* Close a file. */
JCE_API void JCE_CALL jce_fs_close(JceFile *file);

/* Read up to buf_size bytes into buf.  Returns bytes actually read. */
JCE_API uint64_t JCE_CALL jce_fs_read(JceFile *file, void *buf, uint64_t buf_size);

/* Get total file size in bytes (0 if unknown). */
JCE_API uint64_t JCE_CALL jce_fs_size(const JceFile *file);

/* -- Convenience: load entire file into a newly allocated buffer ---- */

/* Allocate and read the entire file.  Caller must JCE_FREE(buf).
   Sets *out_size to the number of bytes read.
   Returns NULL if the file cannot be opened. */
JCE_API void *JCE_CALL jce_fs_read_all(const JceFileSystem *fs, const char *virtual_path,
                                       uint64_t *out_size);

/* -- Query ---------------------------------------------------------- */

/* Returns true if the virtual path exists in any mounted source. */
JCE_API bool JCE_CALL jce_fs_exists(const JceFileSystem *fs, const char *virtual_path);

/* -- Write support -------------------------------------------------- */

/* Set the directory where VFS write operations go.
   Subsequent jce_fs_open_write / jce_fs_write_all are relative to this dir.
   Returns false if the directory does not exist or cannot be set.
   Pass NULL or "" to clear the write directory. */
JCE_API bool JCE_CALL jce_fs_set_write_dir(JceFileSystem *fs, const char *directory);

/* Open a virtual file for writing.  Returns NULL on failure.
   The path is relative to the write directory set by jce_fs_set_write_dir.
   Creates the file if it does not exist; truncates if it does. */
JCE_API JceFile *jce_fs_open_write(JceFileSystem *fs, const char *virtual_path);

/* Write bytes to an opened-for-write file.  Returns bytes written. */
JCE_API uint64_t JCE_CALL jce_fs_write(JceFile *file, const void *buf, uint64_t size);

/* Convenience: write an entire buffer to a file in one call.
   Equivalent to open_write + write + close.
   Returns false on any error. */
JCE_API bool JCE_CALL jce_fs_write_all(JceFileSystem *fs, const char *virtual_path,
                      const void *data, uint64_t size);

/* ================================================================== */
/* Host filesystem helpers                                             */
/* ================================================================== */
/*
 * The functions below operate on REAL OS paths, not VFS virtual
 * paths.  They wrap SDL3 (SDL_GetPathInfo, SDL_CreateDirectory,
 * SDL_EnumerateDirectory, SDL_RemovePath) so that callers (editors,
 * tools, asset pipelines) never pull in <windows.h> / <direct.h> /
 * <sys/stat.h> directly.
 */

/* Returns true iff `path` exists and is a regular file. */
JCE_API bool JCE_CALL jce_fs_host_exists_file(const char *path);

/* Returns true iff `path` exists and is a directory. */
JCE_API bool JCE_CALL jce_fs_host_exists_dir(const char *path);

/* Create a directory (and any missing intermediate parents — mkdir -p
   semantics).  Returns true if the directory exists after the call. */
JCE_API bool JCE_CALL jce_fs_host_create_directory(const char *path);

/* Delete a regular file.  Returns true on success or if the file was
   already absent. */
JCE_API bool JCE_CALL jce_fs_host_remove_file(const char *path);

/* Recursive directory walk.  `cb` receives the absolute path of each
   entry plus a bool indicating whether it's a directory.  Return false
   from `cb` to stop early.  Returns false if `root` cannot be walked. */
typedef bool (*JceFsHostWalkFn)(const char *path, bool is_dir, void *user);
JCE_API bool JCE_CALL jce_fs_host_walk(const char *root, JceFsHostWalkFn cb, void *user);

/* Single-level directory enumeration (non-recursive).  `cb` receives
   the entry's leaf name (no path prefix) and a bool flag for dirs.
   Return false from `cb` to stop early.  Returns false on error. */
typedef bool (*JceFsHostListFn)(const char *name, bool is_dir, void *user);
JCE_API bool JCE_CALL jce_fs_host_list_dir(const char *dir, JceFsHostListFn cb, void *user);

/* Get last-modified time as Unix epoch seconds.  Returns false if
   the path does not exist. */
JCE_API bool JCE_CALL jce_fs_host_get_mtime(const char *path, int64_t *out_epoch_sec);

/* Get file size in bytes.  Returns false if the path is not a file. */
JCE_API bool JCE_CALL jce_fs_host_get_size(const char *path, uint64_t *out_size);

/* Copy a single regular file.  Overwrites `dst` if it exists.
   Returns false if `src` cannot be read or `dst` cannot be written. */
JCE_API bool JCE_CALL jce_fs_host_copy_file(const char *src, const char *dst);

/* Copy a file or a directory tree.  When `src` is a directory, every
   entry under it is copied recursively.  `dst` is the destination
   path (the new location, not its parent).  Existing files are
   overwritten; missing intermediate directories are created. */
JCE_API bool JCE_CALL jce_fs_host_copy_recursive(const char *src, const char *dst);

/* Rename / move a file or directory.  Equivalent to POSIX rename().
   Returns false if `src` does not exist or `dst` is on a different
   volume that the host cannot atomically rename across. */
JCE_API bool JCE_CALL jce_fs_host_rename(const char *src, const char *dst);

/* Recursively delete a file or directory.  Returns true if the path
   does not exist after the call. */
JCE_API bool JCE_CALL jce_fs_host_remove_recursive(const char *path);

/* Build a unique destination path next to `desired` by appending
   " (2)", " (3)", etc. to the stem until no file/directory exists at
   the candidate.  Writes up to `out_size` bytes into `out`.  Returns
   true on success, false on overflow or invalid input. */
JCE_API bool JCE_CALL jce_fs_host_make_unique_path(const char *desired,
                                  char *out, uint32_t out_size);

/* Read an entire host-path file into a freshly allocated buffer.
   On success returns a buffer of *out_size bytes (caller frees with
   jce_fs_buffer_free) and writes the byte count to *out_size.
   Returns NULL on any error (missing file, read truncation, OOM). */
JCE_API void *jce_fs_host_read_all(const char *path, uint64_t *out_size);

/* Write an entire buffer to a host-path file (truncate-and-replace).
   Creates parent directories that do not exist.  Returns true on
   success. */
JCE_API bool JCE_CALL jce_fs_host_write_all(const char *path,
                           const void *data, uint64_t size);

/* Append a buffer to the end of a host-path file (creating it if missing).
   Returns true on success.  Suitable for log/KPI streams; not safe for
   concurrent multi-process appends. */
JCE_API bool JCE_CALL jce_fs_host_append(const char *path,
                           const void *data, uint64_t size);

/* Read up to `max_bytes` of a host-path file into a freshly allocated
   buffer.  On success returns a buffer of *out_read bytes (always
   NUL-terminated past the end so it's safe to cast to char*; caller
   frees with jce_fs_buffer_free), and writes the full file size to
   *out_total (caller may pass NULL for either).  Returns NULL on failure. */
JCE_API void *jce_fs_host_read_capped(const char *path, uint64_t max_bytes,
                                      uint64_t *out_read, uint64_t *out_total);

/* Release a buffer returned by jce_fs_host_read_all / read_capped.
   NULL is OK.  Required because the engine internal allocator is not
   exposed in the public API. */
JCE_API void JCE_CALL jce_fs_buffer_free(void *buf);

/* Active VFS override.
 *
 * Function-pointer hook to avoid a hard link dependency on the VFS
 * read API (jce_fs_read_all lives in jce_filesystem.c, which not every
 * jce_filesystem_host.c consumer links — e.g. the cooker tool).
 *
 * Editor wiring:
 *   jce_fs_set_active(g_bm.fs);   // installs jce_fs_read_all as reader
 *
 * Asset cache loaders calling jce_fs_host_read_all will then transparently
 * see bundle-resident files first, with host-file fallback on miss. */
typedef void *(*JceFsReadFn)(const JceFileSystem *fs, const char *path,
                             uint64_t *out_size);
JCE_API void              JCE_CALL jce_fs_set_active(JceFileSystem *fs);
JCE_API JceFileSystem *   JCE_CALL jce_fs_get_active(void);
/* Low-level: install a custom reader paired with the active fs handle.
 * Default reader is set to jce_fs_read_all when you call
 * jce_fs_set_active(); use this only for tests or alternate backends. */
JCE_API void              JCE_CALL jce_fs_set_active_reader(JceFsReadFn fn);

/* Get the executable's directory (with trailing path separator) into
   `out`.  Returns true on success.  Equivalent to SDL_GetBasePath. */
JCE_API bool JCE_CALL jce_fs_host_get_base_path(char *out, uint32_t out_size);

/* Get the process's current working directory (no trailing separator)
   into `out`.  Returns true on success.  Use this rather than the
   exe directory when the editor or tooling needs the user's launch
   context (e.g. project root resolution, "open file" defaults). */
JCE_API bool JCE_CALL jce_fs_host_get_current_dir(char *out, uint32_t out_size);

JCE_EXTERN_C_END

#endif /* JCE_FILESYSTEM_H */
