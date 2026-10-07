/* Canonical little-endian baked stock payload, linked into the renderer. */
#ifndef JCE_IBL_STOCK_H
#define JCE_IBL_STOCK_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
extern const unsigned char jce_ibl_stock_256[];
extern const size_t jce_ibl_stock_256_size;
static bool jce_ibl_stock_decode(uint32_t size, uint16_t *out, size_t bytes,
                                  const unsigned char *stock, size_t stock_bytes)
{
    if (size != 256u || !out || !stock || bytes != 524288u || stock_bytes != bytes)
        return false;
    for (size_t i = 0; i < bytes / 2; ++i)
        out[i] = (uint16_t)((uint16_t)stock[i * 2] | ((uint16_t)stock[i * 2 + 1] << 8));
    return true;
}
#endif
