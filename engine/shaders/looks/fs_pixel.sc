/*
 * fs_pixel.sc — pixel-art look: low-res snap + palette quantize + ordered (Bayer) dither.
 *
 * CLIENT / SHOWCASE CONTENT via the GENERIC custom post-pass contract.
 * Cheap and bold. No depth needed.
 *
 * Contract: s_texColor(0), s_texDepth(1, unused), u_texelSize, u_postfxTime,
 *   u_postfxParams[0] = (pixel_size_px, color_levels, dither_strength, _)  [<=0 => defaults]
 */

$input v_texcoord0
#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);
SAMPLER2D(s_texDepth, 1);
uniform vec4 u_texelSize;
uniform vec4 u_postfxTime;
uniform vec4 u_postfxParams[8];

/* 4x4 ordered Bayer threshold in [0,1). */
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

	float psize  = u_postfxParams[0].x; if (psize  < 1.0) psize  = 4.0;
	float levels = u_postfxParams[0].y; if (levels < 1.0) levels = 6.0;
	float dither = u_postfxParams[0].z; if (dither <= 0.0) dither = 0.6;

	/* snap to a low-res grid (nearest-neighbour downsample) */
	vec2 block = vec2(psize, psize) / res;
	vec2 puv = (floor(v_texcoord0 / block) + 0.5) * block;
	vec3 col = texture2D(s_texColor, puv).rgb;

	/* ordered-dithered palette quantization */
	float thr = bayer4x4(floor(v_texcoord0 * res / psize));
	float off = mix(0.5, thr, clamp(dither, 0.0, 1.0));
	col = floor(col * levels + off) / levels;

	gl_FragColor = vec4(clamp(col, 0.0, 1.0), 1.0);
}
