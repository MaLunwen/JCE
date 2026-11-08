/* WebGL2 SAMPLES contract. This owns the only format gate; the browser still
 * owns actual sample counts. Values are WebGL/GLES tokens, not bgfx formats. */
#include "jce_webgl_query_internal.h"

static bool renderbuffer_query_format(uint32_t format,uint32_t extensions)
{
    switch(format) {
    /* RGB/RGBA are legal sample queries with empty results, despite not being
     * WebGL2 renderbufferStorage sized formats. No DEPTH_STENCIL alias here. */
    case 0x1907u: case 0x1908u: /* RGB, RGBA */
    case 0x8229u: case 0x822bu: /* R8, RG8 */
    case 0x8051u: case 0x8d62u: /* RGB8, RGB565 */
    case 0x8058u: case 0x8c43u: /* RGBA8, SRGB8_ALPHA8 */
    case 0x8057u: case 0x8056u: case 0x8059u: /* RGB5_A1, RGBA4, RGB10_A2 */
    case 0x81a5u: case 0x81a6u: case 0x8cacu: /* DEPTH_COMPONENT16/24/32F */
    case 0x88f0u: case 0x8cadu: case 0x8d48u: /* depth/stencil sized formats */
    case 0x8231u: case 0x8232u: case 0x8233u: case 0x8234u: /* R 8/16 I/UI */
    case 0x8235u: case 0x8236u: /* R32 I/UI */
    case 0x8237u: case 0x8238u: case 0x8239u: case 0x823au: /* RG 8/16 I/UI */
    case 0x823bu: case 0x823cu: /* RG32 I/UI */
    case 0x8d8eu: case 0x8d7cu: case 0x8d88u: case 0x8d76u: /* RGBA 8/16 I/UI */
    case 0x8d82u: case 0x8d70u: case 0x906fu: /* RGBA32 I/UI, RGB10_A2UI */
        return true;
    case 0x822du: case 0x822fu: case 0x881au: /* R16F, RG16F, RGBA16F */
        return (extensions&(JCE_WEBGL_RBO_HALF_FLOAT|JCE_WEBGL_RBO_FLOAT))!=0;
    case 0x822eu: case 0x8230u: case 0x8814u: case 0x8c3au: /* 32F, R11F_G11F_B10F */
        return (extensions&JCE_WEBGL_RBO_FLOAT)!=0;
    case 0x822au: case 0x822cu: case 0x805bu: /* EXT_texture_norm16 R/RG/RGBA16 */
        return (extensions&JCE_WEBGL_RBO_NORM16)!=0;
    default:
        return false;
    }
}
JceWebGlQueryOutcome jce_webgl_sample_query_internal(
    const JceWebGlQueryProvider *provider,uint32_t target,uint32_t format,
    uint32_t pname,int32_t count,int32_t *samples)
{
    if(!provider || !provider->context || !provider->query || !provider->error)
        return JCE_WEBGL_QUERY_UNAVAILABLE;
    if(count<0 || !samples) {
        provider->error(provider->user,JCE_WEBGL_INVALID_VALUE);
        return JCE_WEBGL_QUERY_BAD_VALUE;
    }
    /* The existing Emscripten bridge copies only returned entries. Legal
     * empty sample arrays must not leave bgfx's first sample uninitialized. */
    if(count>0) samples[0]=0;
    uint32_t extensions=0;
    if(!provider->context(provider->user,&extensions)) {
        provider->error(provider->user,JCE_WEBGL_INVALID_OPERATION);
        return JCE_WEBGL_QUERY_UNAVAILABLE;
    }
    if(target!=0x8d41u || pname!=0x80a9u || !renderbuffer_query_format(format,extensions)) {
        provider->error(provider->user,JCE_WEBGL_INVALID_ENUM);
        return JCE_WEBGL_QUERY_BAD_ENUM;
    }
    provider->query(provider->user,target,format,pname,count,samples);
    return JCE_WEBGL_QUERY_DELEGATED;
}
