/* Actual Emscripten proc-address resolver, no context/window/GPU. The counted
 * raw C symbol prevents a missing-wrap control from touching browser state. */
#include <stdint.h>
#include <stdio.h>

typedef void (*InternalFormatFn)(unsigned,unsigned,unsigned,int,int *);
extern void *emscripten_webgl2_get_proc_address(const char *name);
static unsigned direct_calls,wrapped_calls;

void emscripten_glGetInternalformativ(unsigned target,unsigned format,
    unsigned pname,int count,int *samples)
{
    (void)target;(void)format;(void)pname;
    ++direct_calls;if(count>0 && samples)*samples=7;
}
void __wrap_emscripten_glGetInternalformativ(unsigned target,unsigned format,
    unsigned pname,int count,int *samples)
{
    (void)target;(void)format;(void)pname;
    ++wrapped_calls;if(count>0 && samples)*samples=11;
}
int main(void)
{
    InternalFormatFn fn=(InternalFormatFn)emscripten_webgl2_get_proc_address("glGetInternalformativ");
    int samples=-1;
    if(!fn) {puts("pointer-wrap refusal: actual proc address unavailable");return 4;}
    fn(0x8d41u,0x8058u,0x80a9u,1,&samples);
    printf("pointer-wrap samples=%d wrapped=%u direct=%u\n",samples,wrapped_calls,direct_calls);
    if(samples!=11 || wrapped_calls!=1 || direct_calls!=0) {
        puts("pointer-wrap assertion failed: actual resolver bypassed the wrapper");return 4;
    }
    puts("pointer-wrap PASS: actual Emscripten resolver entered the counted wrapper");return 0;
}
