#ifndef JCE_FULLSCREEN_EFFECT_SH
#define JCE_FULLSCREEN_EFFECT_SH

/* Public shader ABI for project-authored full-screen effects (version 1). */

SAMPLER2D(s_jceSceneColor, 0);
SAMPLER2D(s_jceSceneDepth, 1);
SAMPLER2D(s_jceHistory, 2);
/* User stages are 3, 4, 6, 7 -- NOT 3..6.  Stage 5 is the engine-wide shadow
 * map: sr_bind_frame_shadow_state() binds s_shadowMap there for whatever draw
 * is in flight, so a program declaring its own sampler at 5 gets either the
 * shadow or its own texture depending on which bind ran last.  Neither errors
 * and neither reads as a slot conflict -- it reads as a surface shaded oddly.
 * The gap is why the C side binds through a stage table instead of `3 + i`;
 * the two must agree, so they are changed together or not at all. */
SAMPLER2D(s_jceUser0, 3);
SAMPLER2D(s_jceUser1, 4);
SAMPLER2D(s_jceUser2, 6);
SAMPLER2D(s_jceUser3, 7);

uniform mat4 u_jceViewProj;
uniform mat4 u_jceInvViewProj;
uniform mat4 u_jcePrevViewProj;
uniform mat4 u_jceEffectWorld;
uniform mat4 u_jceEffectWorldInv;
uniform vec4 u_jceCameraPosition;
uniform vec4 u_jceCameraBasis[3];
uniform vec4 u_jceCameraProjection;
uniform vec4 u_jceViewport;
uniform vec4 u_jceOutputViewport;
uniform vec4 u_jceTimeFrame;
uniform vec4 u_jceParams[16];

/* v_texcoord0 is always top-left screen UV. Render targets use the native
 * backend texture origin, so only render-target samples need this conversion.
 * bgfx defines BGFX_SHADER_LANGUAGE_GLSL for both OpenGL and OpenGL ES. */
vec2 jce_fullscreen_render_target_uv(vec2 screen_uv)
{
#if BGFX_SHADER_LANGUAGE_GLSL
    return vec2(screen_uv.x, 1.0 - screen_uv.y);
#else
    return screen_uv;
#endif
}

#endif /* JCE_FULLSCREEN_EFFECT_SH */
