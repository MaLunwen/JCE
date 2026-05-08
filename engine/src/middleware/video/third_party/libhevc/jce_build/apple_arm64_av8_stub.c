/*
 * apple_arm64_av8_stub.c
 *
 * On Apple arm64 (macOS / iOS) we deliberately skip the libhevc NEON .s files
 * because Apple's assembler rejects the ELF directives they use. The decoder
 * function selector still references ihevcd_init_function_ptr_av8 — provide a
 * stub that routes to the noneon C implementation, mirroring what
 * msvc_compat.h does for MSVC ARM64.
 */

struct _codec_t;
void ihevcd_init_function_ptr_noneon(struct _codec_t *ps_codec);

void ihevcd_init_function_ptr_av8(struct _codec_t *ps_codec)
{
    ihevcd_init_function_ptr_noneon(ps_codec);
}
