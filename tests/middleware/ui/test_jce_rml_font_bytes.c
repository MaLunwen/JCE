/* Actual C retention owner; only registry alloc/free are counted ports. */
#include "unity.h"
#include <jce/os/core/jce_alloc.h>
#include <stdint.h>
#include <string.h>
#include "jce_rml_font_bytes.h"
static JceRmlFontBytes owner;
static int allocations, record_frees, payload_frees, admitted;
static bool fail_record, accept;
static void *record_alloc(size_t size)
{
    ++allocations; return fail_record ? NULL : jce_malloc(size);
}
static void record_free(void *ptr)
{
    if (ptr) ++record_frees; jce_free(ptr);
}
static void payload_free(void *ptr)
{
    if (ptr) ++payload_frees; jce_free(ptr);
}
#define jce_malloc record_alloc
#define jce_free record_free
#include "jce_rml_font_bytes.c"
#undef jce_malloc
#undef jce_free
static bool admit(const void *bytes, size_t size, void *user)
{
    (void)user; ++admitted;
    TEST_ASSERT_NOT_NULL(bytes); TEST_ASSERT_TRUE(size > 0);
    TEST_ASSERT_NOT_NULL(owner.head); TEST_ASSERT_TRUE(owner.count > 0);
    TEST_ASSERT_TRUE(owner.bytes >= size); /* retention precedes borrowing */
    return accept;
}
static void *payload(void)
{
    void *p = jce_malloc(8); TEST_ASSERT_NOT_NULL(p); memset(p, 42, 8); return p;
}
void setUp(void)
{
    memset(&owner, 0, sizeof owner);
    allocations = record_frees = payload_frees = admitted = 0;
    fail_record = false; accept = true;
}
void tearDown(void) { jce_rml_font_bytes_clear(&owner); }
static void test_registration_precedes_real_admission_and_clear_is_idempotent(void)
{
    TEST_ASSERT_EQUAL_INT(JCE_RML_FONT_BYTES_OK,
        jce_rml_font_bytes_admit(&owner, payload(), 8, payload_free, admit, NULL));
    TEST_ASSERT_EQUAL_INT(1, admitted); TEST_ASSERT_EQUAL_UINT32(1, owner.count);
    TEST_ASSERT_EQUAL_INT(0, payload_frees); TEST_ASSERT_EQUAL_INT(0, record_frees);
    jce_rml_font_bytes_clear(&owner); jce_rml_font_bytes_clear(&owner);
    TEST_ASSERT_EQUAL_INT(1, payload_frees); TEST_ASSERT_EQUAL_INT(1, record_frees);
    TEST_ASSERT_EQUAL_UINT32(0, owner.bytes);
}
static void test_accepted_sources_remain_owned_until_shutdown_clear(void)
{
    for (int i = 0; i < 2; ++i)
        TEST_ASSERT_EQUAL_INT(JCE_RML_FONT_BYTES_OK,
            jce_rml_font_bytes_admit(&owner, payload(), 8, payload_free, admit, NULL));
    TEST_ASSERT_EQUAL_UINT32(2, owner.count); TEST_ASSERT_EQUAL_UINT32(16, owner.bytes);
    TEST_ASSERT_EQUAL_INT(0, payload_frees);
    jce_rml_font_bytes_clear(&owner);
    TEST_ASSERT_EQUAL_INT(2, payload_frees); TEST_ASSERT_EQUAL_INT(2, record_frees);
}
static void test_face_rejection_retires_once_and_retry_can_be_admitted(void)
{
    accept = false;
    TEST_ASSERT_EQUAL_INT(JCE_RML_FONT_BYTES_REJECTED,
        jce_rml_font_bytes_admit(&owner, payload(), 8, payload_free, admit, NULL));
    TEST_ASSERT_EQUAL_UINT32(0, owner.count);
    TEST_ASSERT_EQUAL_INT(1, payload_frees); TEST_ASSERT_EQUAL_INT(1, record_frees);
    accept = true;
    TEST_ASSERT_EQUAL_INT(JCE_RML_FONT_BYTES_OK,
        jce_rml_font_bytes_admit(&owner, payload(), 8, payload_free, admit, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, owner.count); jce_rml_font_bytes_clear(&owner);
    TEST_ASSERT_EQUAL_INT(2, payload_frees); TEST_ASSERT_EQUAL_INT(2, record_frees);
}
static void test_registry_oom_never_calls_borrower_and_retry_works(void)
{
    fail_record = true;
    TEST_ASSERT_EQUAL_INT(JCE_RML_FONT_BYTES_REGISTRY_OOM,
        jce_rml_font_bytes_admit(&owner, payload(), 8, payload_free, admit, NULL));
    TEST_ASSERT_EQUAL_INT(0, admitted); TEST_ASSERT_EQUAL_INT(1, payload_frees);
    TEST_ASSERT_EQUAL_INT(0, record_frees); TEST_ASSERT_NULL(owner.head);
    fail_record = false;
    TEST_ASSERT_EQUAL_INT(JCE_RML_FONT_BYTES_OK,
        jce_rml_font_bytes_admit(&owner, payload(), 8, payload_free, admit, NULL));
    TEST_ASSERT_EQUAL_INT(1, admitted);
}
static void test_invalid_and_overflow_inputs_do_not_mutate_retained_state(void)
{
    TEST_ASSERT_EQUAL_INT(JCE_RML_FONT_BYTES_INVALID,
        jce_rml_font_bytes_admit(NULL, payload(), 8, payload_free, admit, NULL));
    owner.bytes = SIZE_MAX - 1;
    TEST_ASSERT_EQUAL_INT(JCE_RML_FONT_BYTES_INVALID,
        jce_rml_font_bytes_admit(&owner, payload(), 8, payload_free, admit, NULL));
    TEST_ASSERT_EQUAL_INT(2, payload_frees); TEST_ASSERT_EQUAL_INT(0, allocations);
    TEST_ASSERT_EQUAL_INT(0, admitted); TEST_ASSERT_EQUAL_UINT64(SIZE_MAX - 1, owner.bytes);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_registration_precedes_real_admission_and_clear_is_idempotent);
    RUN_TEST(test_accepted_sources_remain_owned_until_shutdown_clear);
    RUN_TEST(test_face_rejection_retires_once_and_retry_can_be_admitted);
    RUN_TEST(test_registry_oom_never_calls_borrower_and_retry_works);
    RUN_TEST(test_invalid_and_overflow_inputs_do_not_mutate_retained_state);
    return UNITY_END();
}
