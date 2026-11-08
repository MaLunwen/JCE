/* Private WebGL sample-query admission. No public SDK/backend types. */
#ifndef JCE_WEBGL_QUERY_INTERNAL_H
#define JCE_WEBGL_QUERY_INTERNAL_H
#include <stdbool.h>
#include <stdint.h>

#define JCE_WEBGL_RBO_HALF_FLOAT 1u
#define JCE_WEBGL_RBO_FLOAT 2u
#define JCE_WEBGL_RBO_NORM16 4u
#define JCE_WEBGL_INVALID_ENUM 0x0500u
#define JCE_WEBGL_INVALID_VALUE 0x0501u
#define JCE_WEBGL_INVALID_OPERATION 0x0502u

typedef struct JceWebGlQueryProvider {
    bool (*context)(void *user,uint32_t *extensions);
    void (*query)(void *user,uint32_t target,uint32_t format,uint32_t pname,
                  int32_t count,int32_t *samples);
    void (*error)(void *user,uint32_t error);
    void *user;
} JceWebGlQueryProvider;
typedef enum JceWebGlQueryOutcome {
    JCE_WEBGL_QUERY_DELEGATED=0,JCE_WEBGL_QUERY_BAD_VALUE,
    JCE_WEBGL_QUERY_BAD_ENUM,JCE_WEBGL_QUERY_UNAVAILABLE
} JceWebGlQueryOutcome;

JceWebGlQueryOutcome jce_webgl_sample_query_internal(
    const JceWebGlQueryProvider *provider,uint32_t target,uint32_t format,
    uint32_t pname,int32_t count,int32_t *samples);
#endif
