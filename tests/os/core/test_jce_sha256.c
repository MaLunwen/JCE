/* Private Core SHA owner; fixed known answers, chunking and erased state. */
#include "unity.h"
#include "os/core/jce_sha256_internal.h"
#include <string.h>
void setUp(void) {}
void tearDown(void) {}
static void expect_digest(const char *hex, const uint8_t actual[32])
{
    static const char digits[] = "0123456789abcdef";
    char actual_hex[65];
    for (unsigned i = 0; i < 32; ++i) {
        actual_hex[2*i] = digits[actual[i] >> 4];
        actual_hex[2*i+1] = digits[actual[i] & 15u];
    }
    actual_hex[64] = '\0';
    TEST_ASSERT_EQUAL_STRING(hex, actual_hex);
}
static void test_known_abc_and_fips_multiblock(void)
{
    uint8_t digest[32];
    static const char text[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    jce_sha256_internal("abc", 3, digest);
    expect_digest("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", digest);
    jce_sha256_internal(text, sizeof(text)-1, digest);
    expect_digest("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", digest);
}
static void test_padding_boundaries_and_chunked_updates(void)
{
    static const struct { size_t size; const char *hex; } vectors[] = {
    {0u, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
    {55u, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
    {56u, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
    {63u, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"},
    {64u, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
    {65u, "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0"},
    {1000u, "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3"},
    };
    uint8_t bytes[1000], digest[32];
    memset(bytes, 'a', sizeof bytes);
    for (unsigned i = 0; i < sizeof vectors / sizeof vectors[0]; ++i) {
        jce_sha256_internal(bytes, vectors[i].size, digest);
        expect_digest(vectors[i].hex, digest);
        JceSha256Internal ctx;
        jce_sha256_init_internal(&ctx);
        for (size_t offset = 0; offset < vectors[i].size;) {
            size_t take = vectors[i].size - offset;
            if (take > 7) take = 7;
            jce_sha256_update_internal(&ctx, bytes+offset, take);
            offset += take;
        }
        jce_sha256_final_internal(&ctx, digest);
        expect_digest(vectors[i].hex, digest);
        uint8_t zero[sizeof ctx]; memset(zero, 0, sizeof zero);
        TEST_ASSERT_EQUAL_MEMORY(zero, &ctx, sizeof ctx);
    }
    jce_sha256_internal(NULL, 0, digest);
    expect_digest(vectors[0].hex, digest);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_known_abc_and_fips_multiblock);
    RUN_TEST(test_padding_boundaries_and_chunked_updates);
    return UNITY_END();
}
