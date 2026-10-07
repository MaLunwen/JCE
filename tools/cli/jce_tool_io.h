/* Private UTF-8 host stream bridge; no SDL types in tool code. */
#ifndef JCE_TOOL_IO_H
#define JCE_TOOL_IO_H
#include <stddef.h>
#include <stdint.h>
typedef struct JceToolFile JceToolFile;
JceToolFile *jce_tool_fopen(const char *path, const char *mode);
size_t jce_tool_fread(void *data, size_t size, size_t count, JceToolFile *file);
size_t jce_tool_fwrite(const void *data, size_t size, size_t count, JceToolFile *file);
int jce_tool_fseek(JceToolFile *file, int64_t offset, int origin);
int64_t jce_tool_ftell(JceToolFile *file);
int jce_tool_fclose(JceToolFile *file);
#endif
