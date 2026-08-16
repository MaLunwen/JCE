$input v_texcoord0

#include <bgfx_shader.sh>
#include <jce_fullscreen_effect.sh>

#define CK_KERR_MAX_STEPS 1024
#include "include/ck_kerr_lensing_solver.sh"

void main()
{
    gl_FragColor = ck_kerr_render(v_texcoord0);
}
