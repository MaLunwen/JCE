/* Density of the cloud field at a world point, in the atlas's own terms.
 *
 * Lifted out of the view march so the LIGHT march can ask the same question.
 * Two copies of this addressing is exactly how the axis swap survived as long
 * as it did -- it was wrong in the march and right in the CPU bake, and there
 * was nothing to compare. */
/* One trilinear-in-XY, nearest-in-Z fetch of the folded atlas.
 *
 * Split out of cloud_density_at so the same addressing can be reused at a
 * second, higher frequency for the detail term below.  Two copies of this
 * arithmetic is exactly how the axis swap documented above survived. */
vec2 cloud_atlas_fetch_rg(float u01, float v01, float w01)
{
	float dimx = u_cloud_atlas.x, dimy = u_cloud_atlas.y;
	float dimz = u_cloud_atlas.z, tilesx = u_cloud_atlas.w;
	float slice = floor(clamp(w01, 0.0, 0.999) * dimz);
	float tcol  = mod(slice, tilesx);
	float trow  = floor(slice / tilesx);
	float aw    = dimx * tilesx;
	float ah    = dimy * ceil(dimz / tilesx);
	float px    = tcol * dimx + clamp(u01, 0.0, 1.0) * (dimx - 1.0) + 0.5;
	float py    = trow * dimy + clamp(v01, 0.0, 1.0) * (dimy - 1.0) + 0.5;
    // One mip is uploaded. Explicit level 0 also permits divergent ray steps
    // without relying on derivatives or unrolling the entire light march.
#if BGFX_SHADER_LANGUAGE_HLSL || BGFX_SHADER_LANGUAGE_SPIRV || BGFX_SHADER_LANGUAGE_METAL
    return texture2DLod(s_sky_clouds, vec2(px / aw, py / ah), 0.0).rg;
#else
    return texture2D(s_sky_clouds, vec2(px / aw, py / ah)).rg;
#endif
}

float cloud_atlas_fetch(float u01, float v01, float w01)
{
    return cloud_atlas_fetch_rg(u01, v01, w01).x;
}
