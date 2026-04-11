/*
 * jce_render_queue.c  Sorted draw-call queue — stub implementation.
 *
 * STATUS: Architecture stub.  All functions return safe defaults
 *         (NULL / 0) until the render-queue system is built.
 */

#include <jce/render/jce_render_queue.h>
#include <stddef.h>

JceRenderQueue *jce_rq_create(uint32_t initial_capacity)               { (void)initial_capacity; return NULL; }
void            jce_rq_destroy(JceRenderQueue *rq)                     { (void)rq; }

void     jce_rq_push(JceRenderQueue *rq, const JceDrawCmd *cmd)        { (void)rq; (void)cmd; }
void     jce_rq_sort(JceRenderQueue *rq, JceSortMode mode)             { (void)rq; (void)mode; }
void     jce_rq_flush(JceRenderQueue *rq, const JceRenderer *renderer) { (void)rq; (void)renderer; }
void     jce_rq_clear(JceRenderQueue *rq)                              { (void)rq; }
uint32_t jce_rq_count(const JceRenderQueue *rq)                        { (void)rq; return 0; }
