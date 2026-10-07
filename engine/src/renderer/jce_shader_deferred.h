/* Private prepared program state, owned by the singleton bgfx device. */
#ifndef JCE_SHADER_DEFERRED_H
#define JCE_SHADER_DEFERRED_H
#include <stdbool.h>
#include <stdint.h>
#define JCE_DEFERRED_FIRST 65000u
#define JCE_DEFERRED_COUNT 256u
typedef struct {
    void *data;
    uint32_t size;
    bool host;
} JceDeferredSource;
typedef struct {
    bool used, attempted;
    JceDeferredSource vs, fs;
    uint16_t program;
} JceDeferredProgram;
typedef struct {
    JceDeferredProgram programs[JCE_DEFERRED_COUNT];
} JceShaderDeferredState;
JceShaderDeferredState *jce_renderer_shader_deferred_state(void);
uint16_t jce_shader_deferred_resolve(uint16_t idx);
void jce_shader_deferred_shutdown(void);
#endif
