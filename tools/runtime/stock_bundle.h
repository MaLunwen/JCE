/* Public SDK Bundle inspection plus stock Lua/JS capability admission. */
#ifndef JCE_STOCK_BUNDLE_H
#define JCE_STOCK_BUNDLE_H
#include <stdbool.h>
#include <stddef.h>
typedef struct JceStockBundle JceStockBundle;
/* Borrows bytes until close. Never mounts a filesystem or submits rendering. */
JceStockBundle *jce_stock_bundle_open(const void *bytes, size_t size);
void jce_stock_bundle_close(JceStockBundle *bundle);
bool jce_stock_bundle_scene_allowed(const JceStockBundle *bundle, const char *path);
#endif
