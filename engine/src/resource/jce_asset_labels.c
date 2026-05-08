/*
 * jce_asset_labels.c  Asset label name registry.
 *
 * Wraps a flat array of label names; bit position == registration
 * order.  Linear search is fine — projects rarely have more than 10–20
 * labels.
 */

#include <jce/resource/jce_asset_labels.h>

#include <string.h>

#define LABEL_NAME_MAX 32

typedef struct {
    char name[LABEL_NAME_MAX];
    bool valid;
} LabelSlot;

static LabelSlot s_labels[JCE_ASSET_LABEL_MAX];
static uint32_t  s_count = 0;

uint64_t jce_asset_label_bit(const char *name)
{
    if (!name || !name[0]) return 0;
    for (uint32_t i = 0; i < s_count; ++i) {
        if (s_labels[i].valid &&
            strncmp(s_labels[i].name, name, LABEL_NAME_MAX) == 0)
            return ((uint64_t)1u << i);
    }
    if (s_count >= JCE_ASSET_LABEL_MAX) return 0;
    uint32_t id = s_count++;
    strncpy(s_labels[id].name, name, LABEL_NAME_MAX - 1);
    s_labels[id].name[LABEL_NAME_MAX - 1] = '\0';
    s_labels[id].valid = true;
    return ((uint64_t)1u << id);
}

const char *jce_asset_label_name(uint32_t bit_index)
{
    if (bit_index >= s_count || !s_labels[bit_index].valid) return NULL;
    return s_labels[bit_index].name;
}

uint32_t jce_asset_label_count(void) { return s_count; }

uint64_t jce_asset_label_mask_from_names(const char *const *names)
{
    if (!names) return 0;
    uint64_t mask = 0;
    for (const char *const *p = names; *p; ++p)
        mask |= jce_asset_label_bit(*p);
    return mask;
}
