/*
 * JCE hosted-runner toolchain probe (C99, no external headers).
 * Compiling this file checks object generation and pointer width only.
 * It does NOT validate the JCE SDK, its dependencies, or runtime behavior.
 */
#ifndef JCE_CI_EXPECT_POINTER_BYTES
# error JCE_CI_EXPECT_POINTER_BYTES must be supplied by the probe driver
#endif

typedef char jce_ci_pointer_size[(sizeof(void *) == JCE_CI_EXPECT_POINTER_BYTES) ? 1 : -1];
typedef char jce_ci_byte_size[(sizeof(char) == 1) ? 1 : -1];
typedef char jce_ci_short_min[(sizeof(short) >= 2) ? 1 : -1];
typedef char jce_ci_int_min[(sizeof(int) >= 2) ? 1 : -1];

int jce_ci_toolchain_probe(void)
{
    return (int)(sizeof(void *) + sizeof(long) + sizeof(int));
}
