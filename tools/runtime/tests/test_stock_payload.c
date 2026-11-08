/* Executes the complete production envelope/decryption owner with the public
 * archive writer/reader. CPU-only; no renderer, application or extracted files. */
#include "../stock_payload.h"
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_hash.h>
#include <jce/resource/jce_archive.h>
#include <jce/resource/jce_archive_writer.h>
#include <stdio.h>
#include <string.h>
#ifndef JCE_STOCK_PAYLOAD_OWNER
#define JCE_STOCK_PAYLOAD_OWNER "../stock_payload.c"
#endif
#include JCE_STOCK_PAYLOAD_OWNER
static int checks, failures;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; \
    fprintf(stderr, "stock payload line %d: %s\n", __LINE__, #value); } } while (0)
static const uint8_t fixture_key[32] = {
    1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23, 25, 27, 29, 31,
    2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30, 32
};
/* Deliberately binary bytes: this owner unwraps, while stock_bundle.c owns
 * subsequent typed Bundle admission. No synthetic Bundle claim is made. */
static const uint8_t bundle_bytes[] = "JPAK\0stock payload\xff\x00\x7f\x01";
typedef struct FixtureArchive {
    uint8_t *bytes;
    size_t size;
} FixtureArchive;
static void put16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
}
static void put32(uint8_t *out, uint32_t value)
{
    unsigned i;
    for (i = 0; i < 4; ++i) out[i] = (uint8_t)(value >> (i * 8));
}
static void put64(uint8_t *out, uint64_t value)
{
    unsigned i;
    for (i = 0; i < 8; ++i) out[i] = (uint8_t)(value >> (i * 8));
}
static void make_host(uint8_t *image)
{
    memset(image, 0, 512);
    memcpy(image, "MZ", 2);
    put32(image + 60, 64);
    memcpy(image + 64, "PE\0\0", 4);
    put16(image + 68, 0x8664);
    put16(image + 84, 240);
    put16(image + 88, 0x20b);
    put16(image + 156, 2);
}
static void finish_image(uint8_t *image, size_t payload_size)
{
    uint8_t *footer = image + 512 + payload_size;
    memset(footer, 0, 64);
    memcpy(footer, "JCE_STOCK_APP_V2", 16);
    put32(footer + 16, 2);
    put32(footer + 20, 64);
    put64(footer + 24, 512);
    put64(footer + 32, payload_size);
    put64(footer + 40, jce_fnv1a64(image, 512));
    put64(footer + 48, jce_fnv1a64(image + 512, payload_size));
    put64(footer + 56, jce_fnv1a64(footer, 56));
}
static void make_image(uint8_t image[584])
{
    make_host(image);
    memcpy(image + 512, "payload!", 8);
    finish_image(image, 8);
}
static void rejected(const void *bytes, size_t size)
{
    JceStockPayload output;
    output.archive = (const uint8_t *)"sentinel";
    output.archive_size = 123;
    CHECK(!jce_stock_payload_read(bytes, size, &output));
    CHECK(output.archive == NULL);
    CHECK(output.archive_size == 0);
}
static void envelope_controls(void)
{
    uint8_t image[585], copy[584];
    JceStockPayload output;
    const unsigned positions[] = {0, 10, 512, 520, 536, 544, 552, 560, 568, 576};
    const unsigned host_fields[] = {60, 68, 84, 88, 156};
    size_t i;
    make_image(image);
    CHECK(jce_stock_payload_read(image, 584, &output));
    CHECK(output.archive == image + 512);
    CHECK(output.archive_size == 8);
    CHECK(memcmp(output.archive, "payload!", 8) == 0);
    CHECK(!jce_stock_payload_read(image, 584, NULL));
    rejected(NULL, 0);
    rejected(image, 0);
    rejected(image, 63);
    rejected(image, 583);
    /* The image cap must reject before touching the deliberately small buffer. */
    rejected(image, (size_t)JCE_STOCK_IMAGE_LIMIT + 1);
    image[584] = 0;
    rejected(image, 585);
    for (i = 0; i < sizeof(positions) / sizeof(positions[0]); ++i) {
        memcpy(copy, image, sizeof(copy));
        copy[positions[i]] ^= 1;
        rejected(copy, sizeof(copy));
    }
    memcpy(copy, image, sizeof(copy));
    put64(copy + 544, UINT64_MAX);
    put64(copy + 576, jce_fnv1a64(copy + 520, 56));
    rejected(copy, sizeof(copy));
    for (i = 0; i < 4; ++i) {
        memcpy(copy, image, sizeof(copy));
        put64(copy + (i < 2 ? 544 : 552),
            i == 0 || i == 2 ? 0 :
            (i == 1 ? (uint64_t)JCE_STOCK_HOST_LIMIT + 1 :
                          (uint64_t)JCE_STOCK_ARCHIVE_LIMIT + 1));
        put64(copy + 576, jce_fnv1a64(copy + 520, 56));
        rejected(copy, sizeof(copy));
    }
    /* A checksum-consistent v1 raw payload is no longer admissible. */
    memcpy(copy, image, sizeof(copy));
    memcpy(copy + 520, "JCE_STOCK_APP_V1", 16);
    put32(copy + 536, 1);
    put64(copy + 576, jce_fnv1a64(copy + 520, 56));
    rejected(copy, sizeof(copy));
    /* A v1 version with v2 magic cannot bypass explicit version admission. */
    memcpy(copy, image, sizeof(copy));
    put32(copy + 536, 1);
    put64(copy + 576, jce_fnv1a64(copy + 520, 56));
    rejected(copy, sizeof(copy));
    for (i = 0; i < 2; ++i) {
        memcpy(copy, image, sizeof(copy));
        memcpy(copy + 448, i ? "JCE_STOCK_APP_V1" : "JCE_STOCK_APP_V2", 16);
        put64(copy + 560, jce_fnv1a64(copy, 512));
        put64(copy + 576, jce_fnv1a64(copy + 520, 56));
        rejected(copy, sizeof(copy));
    }
    /* Consistent bad PE offsets/architecture/optional header/subsystem remain
     * rejected after rehashing, without invalid pointers or huge reads. */
    for (i = 0; i < sizeof(host_fields) / sizeof(host_fields[0]); ++i) {
        memcpy(copy, image, sizeof(copy));
        if (host_fields[i] == 60) put32(copy + 60, UINT32_MAX);
        else put16(copy + host_fields[i],
            host_fields[i] == 68 ? 0x14c :
            host_fields[i] == 84 ? 69 : host_fields[i] == 88 ? 0x10b : 3);
        put64(copy + 560, jce_fnv1a64(copy, 512));
        put64(copy + 576, jce_fnv1a64(copy + 520, 56));
        rejected(copy, sizeof(copy));
    }
}
static FixtureArchive make_archive(bool secure, const char *path, bool extra)
{
    FixtureArchive result = {NULL, 0};
    JceArchiveWriterConfig config;
    JceArchiveWriter *writer;
    void *bytes = NULL;
    bool added;
    memset(&config, 0, sizeof(config));
    config.zstd_level = 1;
    config.encryption_salt = jce_archive_salt_from_label("stock-payload-cpu-fixture");
    writer = jce_archive_writer_create(&config);
    CHECK(writer != NULL);
    if (!writer) return result;
    if (secure) jce_archive_writer_set_encryption_key(writer, fixture_key);
    added = secure ?
        jce_archive_writer_add_encrypted(writer, path, bundle_bytes, sizeof(bundle_bytes), -1) :
        jce_archive_writer_add(writer, path, bundle_bytes, sizeof(bundle_bytes));
    CHECK(added);
    if (added && extra) {
        added = secure ?
            jce_archive_writer_add_encrypted(writer, "stock/extra.bin", "other", 5, -1) :
            jce_archive_writer_add(writer, "stock/extra.bin", "other", 5);
        CHECK(added);
    }
    if (added) CHECK(jce_archive_writer_finish(writer, &bytes, &result.size));
    result.bytes = (uint8_t *)bytes;
    jce_archive_writer_destroy(writer);
    return result;
}
static void public_archive_controls(const FixtureArchive *fixture, bool secure,
                                    const char *path, uint32_t count)
{
    JceArchive *archive = jce_archive_open(fixture->bytes, fixture->size);
    const JceArchiveEntry *entry;
    uint8_t decoded[sizeof(bundle_bytes)] = {0};
    CHECK(archive != NULL);
    if (!archive) return;
    CHECK(jce_archive_verify_header(archive));
    CHECK((jce_archive_is_secure(archive) != 0) == secure);
    CHECK((jce_archive_is_authenticated(archive) != 0) == secure);
    CHECK(jce_archive_auth_status(archive) ==
        (secure ? JCE_ARCHIVE_AUTH_VALID : JCE_ARCHIVE_AUTH_UNAVAILABLE));
    CHECK(jce_archive_count(archive) == count);
    entry = jce_archive_find(archive, path);
    CHECK(entry != NULL);
    if (entry) {
        CHECK(entry->original_size == sizeof(bundle_bytes));
        CHECK(((entry->entry_flags &
            (JCE_ARCHIVE_ENTRY_ENCRYPTED | JCE_ARCHIVE_ENTRY_AUTHENTICATED)) ==
            (JCE_ARCHIVE_ENTRY_ENCRYPTED | JCE_ARCHIVE_ENTRY_AUTHENTICATED)) == secure);
        CHECK(jce_archive_read(archive, entry, decoded, sizeof(decoded)) == sizeof(decoded));
        CHECK(memcmp(decoded, bundle_bytes, sizeof(decoded)) == 0);
        CHECK(jce_archive_verify_entry(entry, decoded, sizeof(decoded)));
    }
    jce_archive_close(archive);
}
static void decrypt_rejected(const JceStockPayload *payload)
{
    uint8_t *output = (uint8_t *)"sentinel";
    size_t size = 123;
    bool accepted = jce_stock_payload_decrypt(payload, &output, &size);
    CHECK(!accepted);
    CHECK(output == NULL);
    CHECK(size == 0);
    /* Ordinary old controls may successfully decode a forbidden fixture. */
    if (accepted) jce_free(output);
}
static void archive_controls(void)
{
    FixtureArchive secure, plain, wrong_path, two_entries;
    JceStockPayload payload, bad;
    uint8_t *output, *image, *corrupt;
    uint8_t wrong_key[32];
    size_t size, image_size, i;
    secure = make_archive(true, JCE_STOCK_BUNDLE_PATH, false);
    plain = make_archive(false, JCE_STOCK_BUNDLE_PATH, false);
    wrong_path = make_archive(true, "stock/wrong.jbundle", false);
    two_entries = make_archive(true, JCE_STOCK_BUNDLE_PATH, true);
    if (!secure.bytes || !plain.bytes || !wrong_path.bytes || !two_entries.bytes) goto done;
    jce_archive_set_process_key(fixture_key);
    /* All refusal inputs first pass the real public parser/deep read. The
     * plain/two-entry assertions therefore exercise stock admission itself. */
    public_archive_controls(&secure, true, JCE_STOCK_BUNDLE_PATH, 1);
    public_archive_controls(&plain, false, JCE_STOCK_BUNDLE_PATH, 1);
    public_archive_controls(&wrong_path, true, "stock/wrong.jbundle", 1);
    public_archive_controls(&two_entries, true, JCE_STOCK_BUNDLE_PATH, 2);
    payload.archive = secure.bytes;
    payload.archive_size = secure.size;
    output = NULL;
    size = 0;
    CHECK(jce_stock_payload_decrypt(&payload, &output, &size));
    CHECK(output != NULL && size == sizeof(bundle_bytes));
    if (output && size == sizeof(bundle_bytes))
        CHECK(memcmp(output, bundle_bytes, sizeof(bundle_bytes)) == 0);
    jce_free(output);
    image_size = 512 + secure.size + JCE_STOCK_FOOTER_SIZE;
    image = (uint8_t *)jce_malloc(image_size);
    CHECK(image != NULL);
    if (image) {
        make_host(image);
        memcpy(image + 512, secure.bytes, secure.size);
        finish_image(image, secure.size);
        CHECK(jce_stock_payload_read(image, image_size, &bad));
        CHECK(bad.archive == image + 512 && bad.archive_size == secure.size);
        output = NULL;
        size = 0;
        CHECK(jce_stock_payload_decrypt(&bad, &output, &size));
        CHECK(output != NULL && size == sizeof(bundle_bytes));
        if (output && size == sizeof(bundle_bytes))
            CHECK(memcmp(output, bundle_bytes, sizeof(bundle_bytes)) == 0);
        jce_free(output);
        jce_free(image);
    }
    jce_archive_set_process_key(NULL);
    decrypt_rejected(&payload);
    memcpy(wrong_key, fixture_key, sizeof(wrong_key));
    wrong_key[0] ^= 0x80;
    jce_archive_set_process_key(wrong_key);
    decrypt_rejected(&payload);
    jce_archive_set_process_key(fixture_key);
    corrupt = (uint8_t *)jce_malloc(secure.size);
    CHECK(corrupt != NULL);
    if (corrupt) {
        for (i = 0; i < 2; ++i) {
            memcpy(corrupt, secure.bytes, secure.size);
            corrupt[i ? secure.size - 1 : 64] ^= 1;
            bad.archive = corrupt;
            bad.archive_size = secure.size;
            decrypt_rejected(&bad);
        }
        jce_free(corrupt);
    }
    bad.archive = plain.bytes;
    bad.archive_size = plain.size;
    decrypt_rejected(&bad);
    bad.archive = wrong_path.bytes;
    bad.archive_size = wrong_path.size;
    decrypt_rejected(&bad);
    bad.archive = two_entries.bytes;
    bad.archive_size = two_entries.size;
    decrypt_rejected(&bad);
    decrypt_rejected(NULL);
    bad = payload;
    bad.archive = NULL;
    decrypt_rejected(&bad);
    bad = payload;
    bad.archive_size = 63;
    decrypt_rejected(&bad);
    /* The small secure buffer intentionally advertises a huge size. This must
     * fail at the declared archive budget before parsing or allocation. */
    bad = payload;
    bad.archive_size = (size_t)JCE_STOCK_ARCHIVE_LIMIT + 1;
    decrypt_rejected(&bad);
    output = (uint8_t *)"sentinel";
    CHECK(!jce_stock_payload_decrypt(&payload, &output, NULL));
    CHECK(output == NULL);
    size = 123;
    CHECK(!jce_stock_payload_decrypt(&payload, NULL, &size));
    CHECK(size == 0);
done:
    jce_archive_set_process_key(NULL);
    jce_free(two_entries.bytes);
    jce_free(wrong_path.bytes);
    jce_free(plain.bytes);
    jce_free(secure.bytes);
}
int main(void)
{
    envelope_controls();
    archive_controls();
    printf("stock payload: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
