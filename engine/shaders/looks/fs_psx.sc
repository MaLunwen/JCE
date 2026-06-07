/*
 * fs_psx.sc — PSX / retro low-fi look (screen-space approximation).
 *
 * CLIENT / SHOWCASE CONTENT via the GENERIC custom post-pass contract.
 * True PS1 character comes from VERTEX snapping + affine (perspective-incorrect)
 * texture warping, which are vertex/material concerns; this post pass evokes the
 * LOOK: low resolution, 15-bit-ish colour banding with ordered dither, a little
 * positional jitter, and faint scanlines.  No depth needed.
 *
 * Contract: s_texColor(0), s_texDepth(1, unused), u_texelSize, u_postfxTime(x=time),
 *   u_postfxParams[0] = (downscale_px, color_levels, dither_strength, jitter)  [<=0 => defaults]
 */

$input v_texcoord0
#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);
SAMPLER2D(s_texDepth, 1);
uniform vec4 u_texelSize;
uniform vec4 u_postfxTime;
uniform vec4 u_postfxParams[8];

float bayer4x4(vec2 p)
{
	float m[16];
	m[0]=0.0;  m[1]=8.0;  m[2]=2.0;  m[3]=10.0;
	m[4]=12.0; m[5]=4.0;  m[6]=14.0; m[7]=6.0;
	m[8]=3.0;  m[9]=11.0; m[10]=1.0; m[11]=9.0;
	m[12]=15.0;m[13]=7.0; m[14]=13.0;m[15]=5.0;
	int ix = int(mod(p.x, 4.0));
	int iy = int(mod(p.y, 4.0));
	return m[ix + iy * 4] / 16.0;
}

void main()
{
	vec2 res = u_texelSize.zw;
	float t = u_postfxTime.x;

	float downscale = u_postfxParams[0].x; if (downscale < 1.0) downscale = 3.0;
	float levels    = u_postfxParams[0].y; if (levels   < 2.0) levels    = 32.0;  /* ~15-bit */
	float dither    = u_postfxParams[0].z; if (dither  <= 0.0) dither    = 0.7;
	float jitter    = u_postfxParams[0].w; if (jitter  <= 0.0) jitter    = 0.5;

	/* low-freq positional wobble (fakes vertex snap / affine warp) */
	vec2 wob = vec2(sin(v_texcoord0.y * 60.0 + t * 6.0),
	                sin(v_texcoord0.x * 60.0 + t * 5.0)) * jitter / res;

	vec2 block = vec2(downscale, downscale) / res;
	vec2 puv = (floor((v_texcoord0 + wob) / block) + 0.5) * block;
	vec3 col = texture2D(s_texColor, puv).rgb;

	/* colour-depth reduction + ordered dither */
	float thr = bayer4x4(floor(v_texcoord0 * res / downscale));
	float off = mix(0.5, thr, clamp(dither, 0.0, 1.0));
	col = floor(col * levels + off) / levels;

	/* faint scanlines */
	col *= 0.92 + 0.08 * abs(sin(v_texcoord0.y * res.y * 1.5708));

	gl_FragColor = vec4(clamp(col, 0.0, 1.0), 1.0);
}
