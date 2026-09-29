/*
 * area_light.sh -- rectangular area lights, shared by every lit surface.
 *
 * ONE COPY, included by fs_pbr_body.sh (which is every PBR variant: default,
 * Forward+, toon, tint, fade, texture-array) and by fs_terrain.sc.  Those two
 * are the only shaders that consume u_pointLights / u_spotLights today, so
 * they are the whole set an area light has to reach -- a light that lit meshes
 * and not the ground they stand on would be the shape of feature this audit
 * exists to remove.
 *
 * NO LUT TEXTURES, deliberately.  The reference technique is LTC (Linearly
 * Transformed Cosines), which Unity and UE both use and which needs two more
 * sampler stages.  fs_pbr_body's sixteen stages are FULL -- that ceiling is
 * already recorded in this tree -- so an LTC path could not be bound at all.
 * What is here instead is the standard analytic pair, and it is an
 * approximation with named error rather than a stand-in:
 *
 *   DIFFUSE   the exact solid angle the rectangle subtends, times the average
 *             of five cosines (the four corners and the centre).  Frostbite's
 *             formulation.  Exact for a receiver facing the light; the
 *             five-point average is what carries the grazing cases.
 *   SPECULAR  the representative point: the closest point ON the rectangle to
 *             the reflection ray, lit as a punctual light with the roughness
 *             widened by the angle the light subtends, and the energy
 *             renormalised by (alpha/alpha')^2 so widening does not brighten.
 *             Karis's MRP, which is what the cheap path of every engine that
 *             ships area lights uses.
 *
 * The visible difference from a point light -- the reason the feature exists
 * -- is the ELONGATED specular highlight and the soft, shape-aware terminator.
 * Both come out of the two functions below and neither is expressible with a
 * point or a spot.
 *
 * PACKING, four vec4 per light (jce_lighting_system.c writes it):
 *   [0] xyz = centre position      w = half width
 *   [1] xyz = light normal (unit)  w = half height
 *   [2] xyz = colour * intensity   w = attenuation radius
 *   [3] xyz = right/tangent (unit) w = two-sided ? 1 : 0
 * up = cross(normal, right), which is why right must be unit and orthogonal.
 */
#ifndef JCE_AREA_LIGHT_SH
#define JCE_AREA_LIGHT_SH

#define JCE_MAX_AREA_LIGHTS 4

uniform vec4 u_areaLights[JCE_MAX_AREA_LIGHTS * 4];
uniform vec4 u_areaParams;   /* x = count, yzw reserved */

/* Solid angle of the quad (p0,p1,p2,p3) as seen from `pos`, in steradians.
 *
 * The spherical excess of the spherical quadrilateral: sum the four dihedral
 * angles and subtract 2*PI.  Exact, not fitted.  Returns 0 when the quad is
 * edge-on or degenerate, which is what the clamps below are for -- acos of a
 * value a hair outside [-1,1] is NaN, and a NaN here propagates through the
 * whole lighting sum and blackens the pixel. */
float jce_rect_solid_angle(vec3 pos, vec3 p0, vec3 p1, vec3 p2, vec3 p3)
{
    vec3 v0 = p0 - pos;
    vec3 v1 = p1 - pos;
    vec3 v2 = p2 - pos;
    vec3 v3 = p3 - pos;

    vec3 n0 = cross(v0, v1);
    vec3 n1 = cross(v1, v2);
    vec3 n2 = cross(v2, v3);
    vec3 n3 = cross(v3, v0);
    float l0 = length(n0), l1 = length(n1), l2 = length(n2), l3 = length(n3);
    if (l0 < 1e-8 || l1 < 1e-8 || l2 < 1e-8 || l3 < 1e-8) return 0.0;
    n0 /= l0; n1 /= l1; n2 /= l2; n3 /= l3;

    float g0 = acos(clamp(dot(-n0, n1), -1.0, 1.0));
    float g1 = acos(clamp(dot(-n1, n2), -1.0, 1.0));
    float g2 = acos(clamp(dot(-n2, n3), -1.0, 1.0));
    float g3 = acos(clamp(dot(-n3, n0), -1.0, 1.0));
    return max(g0 + g1 + g2 + g3 - 2.0 * PI, 0.0);
}

/* The closest point ON the rectangle to the reflection ray -- the
 * "representative point" the specular lobe is evaluated at.
 *
 * Intersect the reflection ray with the light's plane, then clamp the hit into
 * the rectangle's extents.  A ray parallel to the plane, or pointing away from
 * it, has no forward hit: fall back to the centre, which is the same answer a
 * point light would give and is what the surrounding falloff already handles.
 */
vec3 jce_rect_closest_point(vec3 pos, vec3 R, vec3 centre, vec3 nrm,
                            vec3 right, vec3 up, float halfW, float halfH)
{
    float denom = dot(nrm, R);
    vec3 hit = centre;
    if (abs(denom) > 1e-5) {
        float t = dot(nrm, centre - pos) / denom;
        if (t > 0.0) hit = pos + R * t;
    }
    vec3 d = hit - centre;
    float dx = clamp(dot(d, right), -halfW, halfW);
    float dy = clamp(dot(d, up),    -halfH, halfH);
    return centre + right * dx + up * dy;
}

/* One rectangular light's contribution.  Returns radiance to ADD to Lo. */
vec3 jce_area_light_contrib(int i, vec3 P, vec3 N, vec3 V,
                            vec3 F0, vec3 albedo,
                            float metallic, float roughness, float wrap)
{
    vec3  centre = u_areaLights[i * 4 + 0].xyz;
    float halfW  = u_areaLights[i * 4 + 0].w;
    vec3  nrm    = u_areaLights[i * 4 + 1].xyz;
    float halfH  = u_areaLights[i * 4 + 1].w;
    vec3  colour = u_areaLights[i * 4 + 2].xyz;
    float radius = u_areaLights[i * 4 + 2].w;
    vec3  right  = u_areaLights[i * 4 + 3].xyz;
    float twoSided = u_areaLights[i * 4 + 3].w;

    vec3 toC  = centre - P;
    float distC = length(toC);
    /* Same radius falloff the point lights use, so an area light and a point
     * light of the same radius fade over the same distance. */
    float atten = clamp(1.0 - (distC * distC) / max(radius * radius, 1e-6),
                        0.0, 1.0);
    atten *= atten;
    if (atten <= 0.0) return vec3_splat(0.0);

    /* A one-sided rectangle emits from its front face only.  `nrm` points the
     * way it emits; a receiver behind the plane gets nothing. */
    float facing = dot(nrm, -toC);   /* >0 when P is in front of the plane */
    if (twoSided < 0.5 && facing <= 0.0) return vec3_splat(0.0);
    /* Two-sided: flip the normal toward the receiver so the corner winding
     * below stays consistent and the solid angle stays positive. */
    if (facing < 0.0) nrm = -nrm;

    vec3 up = cross(nrm, right);
    vec3 ex = right * halfW;
    vec3 ey = up    * halfH;
    vec3 p0 = centre - ex - ey;
    vec3 p1 = centre + ex - ey;
    vec3 p2 = centre + ex + ey;
    vec3 p3 = centre - ex + ey;

    /* ── Diffuse: solid angle x the five-cosine average ── */
    float sa = jce_rect_solid_angle(P, p0, p1, p2, p3);
    vec3 diffuse = vec3_splat(0.0);
    if (sa > 0.0) {
        float c = 0.0;
        c += clamp((dot(N, normalize(p0 - P)) + wrap) / (1.0 + wrap), 0.0, 1.0);
        c += clamp((dot(N, normalize(p1 - P)) + wrap) / (1.0 + wrap), 0.0, 1.0);
        c += clamp((dot(N, normalize(p2 - P)) + wrap) / (1.0 + wrap), 0.0, 1.0);
        c += clamp((dot(N, normalize(p3 - P)) + wrap) / (1.0 + wrap), 0.0, 1.0);
        c += clamp((dot(N, normalize(centre - P)) + wrap) / (1.0 + wrap), 0.0, 1.0);
        float illum = sa * 0.2 * c;
        /* kD, matching cookTorranceBRDFWrap's split so a metal gets no
         * diffuse from an area light either. */
        vec3 kD = (vec3_splat(1.0) - F0) * (1.0 - metallic);
        diffuse = kD * albedo * (illum / PI);
    }

    /* ── Specular: representative point + widened roughness ── */
    vec3 R = reflect(-V, N);
    vec3 rp = jce_rect_closest_point(P, R, centre, nrm, right, up, halfW, halfH);
    vec3 toRP = rp - P;
    float distRP = max(length(toRP), 1e-4);
    vec3 L = toRP / distRP;

    float alpha = roughness * roughness;
    /* The angle the light subtends, as a sphere of the rectangle's larger
     * half-extent.  Karis's widening; the (alpha/alpha')^2 below is his
     * energy renormalisation, without which a big light near a smooth surface
     * gains brightness it should not have. */
    float lightRadius = max(halfW, halfH);
    float alphaP = clamp(alpha + lightRadius / (2.0 * distRP), 0.0, 1.0);
    float norm = (alpha / max(alphaP, 1e-4));
    norm = norm * norm;

    float roughP = sqrt(alphaP);
    vec3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);
    float D = distributionGGX(N, H, roughP);
    float G = geometrySmith(N, V, L, roughP);
    vec3  F = fresnelSchlick(max(dot(H, V), 0.0), F0);
    vec3 specular = (D * G * F) / max(4.0 * NdotV * NdotL, 0.001);
    specular *= norm * NdotL;

    return (diffuse + specular) * colour * atten;
}

#endif /* JCE_AREA_LIGHT_SH */
