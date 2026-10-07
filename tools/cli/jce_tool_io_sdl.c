/* Sanctioned stream bridge. SDL owns native path encoding and stream I/O. */
#include "jce_tool_io.h"
#include <SDL3/SDL_iostream.h>
#include <stdio.h>

JceToolFile *jce_tool_fopen(const char *path, const char *mode)
{
    return (JceToolFile *)SDL_IOFromFile(path, mode);
}

size_t jce_tool_fread(void *data, size_t size, size_t count, JceToolFile *file)
{
    if (!file || !size || count > SIZE_MAX / size) return 0;
    return SDL_ReadIO((SDL_IOStream *)file, data, size * count) / size;
}

size_t jce_tool_fwrite(const void *data, size_t size, size_t count, JceToolFile *file)
{
    if (!file || !size || count > SIZE_MAX / size) return 0;
    return SDL_WriteIO((SDL_IOStream *)file, data, size * count) / size;
}

int jce_tool_fseek(JceToolFile *file, int64_t offset, int origin)
{
    SDL_IOWhence whence;
    if (origin == SEEK_SET) whence = SDL_IO_SEEK_SET;
    else if (origin == SEEK_CUR) whence = SDL_IO_SEEK_CUR;
    else if (origin == SEEK_END) whence = SDL_IO_SEEK_END;
    else return -1;
    return SDL_SeekIO((SDL_IOStream *)file, offset, whence) < 0 ? -1 : 0;
}

int64_t jce_tool_ftell(JceToolFile *file)
{
    return SDL_TellIO((SDL_IOStream *)file);
}

int jce_tool_fclose(JceToolFile *file)
{
    return SDL_CloseIO((SDL_IOStream *)file) ? 0 : -1;
}
