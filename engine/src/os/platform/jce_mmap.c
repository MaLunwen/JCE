/*
 * jce_mmap.c  Cross-platform read-only whole-file memory mapping (spec §8).
 */

#include <jce/os/platform/jce_mmap.h>

#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_log.h>

#include <stdint.h>
#include <string.h>

#define LOG_TAG "mmap"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

struct JceMmap {
    const void *data;
    size_t      size;
    bool        is_mapped;   /* true: OS mapping; false: read fallback buffer */
#if defined(_WIN32)
    HANDLE      file;
    HANDLE      mapping;
#endif
};

/* Read the whole file into a jce_malloc'd buffer (mapping fallback / tiny
 * files).  Returns 1 on success, filling *out / *out_size. */
static int read_whole_file(const char *path, void **out, size_t *out_size) {
#if defined(_WIN32)
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER li;
    if (!GetFileSizeEx(f, &li) || li.QuadPart < 0) { CloseHandle(f); return 0; }
    size_t sz = (size_t)li.QuadPart;
    uint8_t *buf = (uint8_t *)jce_malloc(sz ? sz : 1);
    if (!buf) { CloseHandle(f); return 0; }
    size_t done = 0;
    while (done < sz) {
        DWORD chunk = (DWORD)((sz - done > 0x40000000u) ? 0x40000000u : (sz - done));
        DWORD got = 0;
        if (!ReadFile(f, buf + done, chunk, &got, NULL) || got == 0) {
            jce_free(buf); CloseHandle(f); return 0;
        }
        done += got;
    }
    CloseHandle(f);
    *out = buf; *out_size = sz;
    return 1;
#else
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 0) { close(fd); return 0; }
    size_t sz = (size_t)st.st_size;
    uint8_t *buf = (uint8_t *)jce_malloc(sz ? sz : 1);
    if (!buf) { close(fd); return 0; }
    size_t done = 0;
    while (done < sz) {
        ssize_t got = read(fd, buf + done, sz - done);
        if (got <= 0) { jce_free(buf); close(fd); return 0; }
        done += (size_t)got;
    }
    close(fd);
    *out = buf; *out_size = sz;
    return 1;
#endif
}

JceMmap *jce_mmap_open(const char *path) {
    if (!path || !path[0]) return NULL;

    JceMmap *m = (JceMmap *)jce_malloc(sizeof(JceMmap));
    if (!m) return NULL;
    memset(m, 0, sizeof(*m));

#if defined(_WIN32)
    m->file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (m->file != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER li;
        if (GetFileSizeEx(m->file, &li) && li.QuadPart > 0) {
            m->mapping = CreateFileMappingA(m->file, NULL, PAGE_READONLY, 0, 0, NULL);
            if (m->mapping) {
                void *view = MapViewOfFile(m->mapping, FILE_MAP_READ, 0, 0, 0);
                if (view) {
                    m->data = view;
                    m->size = (size_t)li.QuadPart;
                    m->is_mapped = true;
                    return m;
                }
                CloseHandle(m->mapping); m->mapping = NULL;
            }
        }
        CloseHandle(m->file); m->file = INVALID_HANDLE_VALUE;
    }
    m->file = INVALID_HANDLE_VALUE;
#else
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        struct stat st;
        if (fstat(fd, &st) == 0 && st.st_size > 0) {
            void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
            if (p != MAP_FAILED) {
                close(fd);
                m->data = p;
                m->size = (size_t)st.st_size;
                m->is_mapped = true;
                return m;
            }
        }
        close(fd);
    }
#endif

    /* Fallback: read the whole file into a heap buffer. */
    void  *buf = NULL;
    size_t sz  = 0;
    if (!read_whole_file(path, &buf, &sz)) {
        LOG_WARN(LOG_TAG, "open failed: %s", path);
        jce_free(m);
        return NULL;
    }
    m->data = buf;
    m->size = sz;
    m->is_mapped = false;
    return m;
}

const void *jce_mmap_data(const JceMmap *m) { return m ? m->data : NULL; }
size_t      jce_mmap_size(const JceMmap *m) { return m ? m->size : 0; }
bool        jce_mmap_is_mapped(const JceMmap *m) { return m ? m->is_mapped : false; }

void jce_mmap_close(JceMmap *m) {
    if (!m) return;
    if (m->is_mapped) {
#if defined(_WIN32)
        if (m->data)    UnmapViewOfFile((LPCVOID)m->data);
        if (m->mapping) CloseHandle(m->mapping);
        if (m->file != INVALID_HANDLE_VALUE) CloseHandle(m->file);
#else
        if (m->data) munmap((void *)m->data, m->size);
#endif
    } else {
        jce_free((void *)m->data);
    }
    jce_free(m);
}
