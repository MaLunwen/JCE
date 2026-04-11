/*
 * jce_render_graph.c  Declarative frame graph — stub implementation.
 *
 * STATUS: Architecture stub.  All functions return safe defaults
 *         (NULL / false / 0) until the render-graph system is built.
 */

#include <jce/render/jce_render_graph.h>
#include <stddef.h>

JceRenderGraph *jce_rg_create(void)                                    { return NULL; }
void            jce_rg_destroy(JceRenderGraph *rg)                     { (void)rg; }

JceRGResource jce_rg_create_resource(JceRenderGraph *rg,
                                      const JceRGResourceDesc *desc)   { (void)rg; (void)desc; return (JceRGResource){0}; }
JceRGResource jce_rg_import_resource(JceRenderGraph *rg,
                                      uint16_t bgfx_texture_handle,
                                      const char *debug_name)          { (void)rg; (void)bgfx_texture_handle; (void)debug_name; return (JceRGResource){0}; }

JceRGPass jce_rg_add_pass(JceRenderGraph *rg, const char *name,
                           JceRGPassExecuteFn fn, void *userdata)      { (void)rg; (void)name; (void)fn; (void)userdata; return (JceRGPass){0}; }
void jce_rg_pass_read(JceRenderGraph *rg, JceRGPass pass,
                       JceRGResource resource)                         { (void)rg; (void)pass; (void)resource; }
void jce_rg_pass_write(JceRenderGraph *rg, JceRGPass pass,
                        JceRGResource resource)                        { (void)rg; (void)pass; (void)resource; }

bool jce_rg_compile(JceRenderGraph *rg)                                { (void)rg; return false; }
void jce_rg_execute(JceRenderGraph *rg)                                { (void)rg; }
void jce_rg_reset(JceRenderGraph *rg)                                  { (void)rg; }
