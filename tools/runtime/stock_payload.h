/* Versioned, non-extracting stock application envelope. Private consumer API. */
#ifndef JCE_STOCK_PAYLOAD_H
#define JCE_STOCK_PAYLOAD_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define JCE_STOCK_FOOTER_SIZE 64u
#define JCE_STOCK_HOST_LIMIT (128u * 1024u * 1024u)
#define JCE_STOCK_BUNDLE_LIMIT (512u * 1024u * 1024u)
#define JCE_STOCK_ARCHIVE_LIMIT (JCE_STOCK_BUNDLE_LIMIT + 1024u * 1024u)
#define JCE_STOCK_IMAGE_LIMIT ((uint64_t)JCE_STOCK_HOST_LIMIT + JCE_STOCK_ARCHIVE_LIMIT + JCE_STOCK_FOOTER_SIZE)
#define JCE_STOCK_BUNDLE_PATH "stock/project.jbundle"
typedef struct JceStockPayload {
    const uint8_t *archive;
    size_t archive_size;
} JceStockPayload;
/* Version 2 borrows one secure outer PAK; version 1 raw Bundles are refused.
 * Caller retains the mapped image until this archive has closed.
 * Every failure clears output. FNV is corruption detection, not authentication. */
bool jce_stock_payload_read(const void *image, size_t size, JceStockPayload *out);
/* Authenticate before allocating/decrypting the one original Bundle.
 * The startup process key is installed by the ordinary engine boot. On success
 * caller owns *out (jce_free), retaining it until every Bundle borrower closes.
 * Every failure clears both outputs. No content is written to the filesystem. */
bool jce_stock_payload_decrypt(const JceStockPayload *payload,
                               uint8_t **out, size_t *out_size);
#endif
