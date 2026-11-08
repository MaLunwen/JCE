/* Counted allocator port for the exact resource loader TU in CPU tests. */
#ifndef JCE_RESOURCE_LOADER_TEST_MEMORY_H
#define JCE_RESOURCE_LOADER_TEST_MEMORY_H
#include <stddef.h>
void *jce_test_resource_malloc(size_t size);
void jce_test_resource_free(void *memory);
#define JCE_MALLOC(size) jce_test_resource_malloc(size)
#define JCE_FREE(memory) jce_test_resource_free(memory)
#endif
