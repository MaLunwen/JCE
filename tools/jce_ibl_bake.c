/* Offline stock IBL generation; exact production kernel, canonical LE16. */
#include "cli/jce_tool_main.h"
#include "cli/jce_tool_io.h"
#include "renderer/jce_ibl_brdf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int jce_tool_main(int argc, char **argv)
{
    if (argc != 3 || strcmp(argv[1], "--output") != 0) return 2;
    JceToolFile *file = jce_tool_fopen(argv[2], "wb");
    if (!file) return 3;
    uint16_t row[256 * 4];
    for (uint32_t y = 0; y < 256; ++y) {
        jce_ibl_brdf_row(256, y, row);
        for (size_t i = 0; i < 256 * 4; ++i) {
            unsigned char bytes[2] = {(unsigned char)row[i], (unsigned char)(row[i] >> 8)};
            if (jce_tool_fwrite(bytes, 1, 2, file) != 2) { jce_tool_fclose(file); return 4; }
        }
    }
    return jce_tool_fclose(file) == 0 ? 0 : 5;
}
