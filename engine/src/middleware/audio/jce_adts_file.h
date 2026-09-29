#ifndef JCE_ADTS_FILE_H
#define JCE_ADTS_FILE_H
#include <stdbool.h>
#include <stdint.h>
typedef struct JceAdtsFile JceAdtsFile;
JceAdtsFile *jce_adts_file_open(const char *path);
void jce_adts_file_close(JceAdtsFile *file);
uint32_t jce_adts_file_read(JceAdtsFile *file,int16_t *out,uint32_t frames);
bool jce_adts_file_seek(JceAdtsFile *file,uint64_t frame);
void jce_adts_file_format(const JceAdtsFile *file,uint32_t *channels,
                          uint32_t *rate,uint64_t *frames);
#endif
