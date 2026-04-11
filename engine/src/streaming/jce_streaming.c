/*
 * jce_streaming.c  Resource streaming — stub implementation.
 *
 * STATUS: Architecture stub.  All functions return safe defaults
 *         (NULL / 0 / false) until the streaming system is built.
 */

#include <jce/streaming/jce_streaming.h>
#include <stddef.h>

JceStreamingSystem *jce_streaming_create(const JceStreamingConfig *config) { (void)config; return NULL; }
void                jce_streaming_destroy(JceStreamingSystem *sys)        { (void)sys; }

void jce_streaming_register_chunk(JceStreamingSystem *sys,
                                   const JceStreamChunk *chunk)           { (void)sys; (void)chunk; }
void jce_streaming_unregister_chunk(JceStreamingSystem *sys,
                                     uint32_t chunk_id)                   { (void)sys; (void)chunk_id; }

void jce_streaming_update(JceStreamingSystem *sys, jce_vec3 camera_pos)   { (void)sys; (void)camera_pos; }

uint32_t jce_streaming_loaded_count(const JceStreamingSystem *sys)        { (void)sys; return 0; }
uint32_t jce_streaming_pending_count(const JceStreamingSystem *sys)       { (void)sys; return 0; }
uint64_t jce_streaming_memory_used(const JceStreamingSystem *sys)         { (void)sys; return 0; }
bool     jce_streaming_chunk_loaded(const JceStreamingSystem *sys,
                                     uint32_t chunk_id)                   { (void)sys; (void)chunk_id; return false; }
