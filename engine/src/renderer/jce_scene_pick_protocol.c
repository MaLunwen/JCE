/*
 * jce_scene_pick_protocol.c  Object-ID picking pixel encoding.
 */

#include <jce/renderer/jce_pick_id.h>

bool jce_scene_pick_encode_rgba(uint64_t key, uint8_t out_rgba[4])
{
    if (!out_rgba || key == 0 || key > 0x00FFFFFFu)
        return false;

    out_rgba[0] = (uint8_t)(key & 0xFFu);
    out_rgba[1] = (uint8_t)((key >> 8) & 0xFFu);
    out_rgba[2] = (uint8_t)((key >> 16) & 0xFFu);
    out_rgba[3] = 255u;
    return true;
}

uint64_t jce_scene_pick_decode_rgba(const uint8_t rgba[4])
{
    if (!rgba)
        return 0;
    return (uint64_t)rgba[0]
         | ((uint64_t)rgba[1] << 8)
         | ((uint64_t)rgba[2] << 16);
}
