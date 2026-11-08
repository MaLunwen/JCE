/* Private foliage roots-buffer policy and ownership. No backend types. */
#ifndef JCE_SR_FOLIAGE_BUFFER_H
#define JCE_SR_FOLIAGE_BUFFER_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint16_t (*create)(uint32_t count, bool compute_read, void *user);
    bool (*upload)(uint16_t handle, const void *roots, uint32_t count, void *user);
    void (*destroy)(uint16_t handle, void *user);
    void *user;
} JceSrFoliageBufferProvider;

/* A leading '0' disables the ordinary resident path. Compute culling needs
 * resident roots regardless of that diagnostic override. */
bool sr_foliage_persist_enabled(const char *value, bool compute_cull);

/* Only a true result makes the buffer current for this count/hash/capability.
 * Upload success means the provider accepted CPU data, not GPU completion. */
bool sr_foliage_buffer_ensure(uint16_t *handle, uint32_t *resident_hash,
    uint32_t *resident_count, bool *resident_compute_read, const void *roots,
    uint32_t count, uint32_t hash, bool compute_read,
    const JceSrFoliageBufferProvider *provider);
void sr_foliage_buffer_destroy(uint16_t *handle, uint32_t *resident_hash,
    uint32_t *resident_count, bool *resident_compute_read,
    const JceSrFoliageBufferProvider *provider);

#endif
