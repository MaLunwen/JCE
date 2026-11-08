$input v_texcoord0, v_worldpos, v_normal, v_tangent, v_bitangent, v_viewdepth, v_localpos
/* Classic gamma-space Blinn-Phong material. Uses the public material binding:
 * metallic = specular reflectance; roughness = 1 - shininess/128.
 * mrTex red channel = specular mask. Scene hemisphere and CSM stay shared. */
#include "fs_pbr_decl.sh"
#define JCE_CSM_HELPERS_PROVIDED
#include "csm_shadow.sh"
#undef JCE_CSM_HELPERS_PROVIDED

/* A zero shininess is a constant lobe, including at a zero dot product.
 * Bare pow(0, 0) is undefined across shader backends and can return NaN.
 * Keep the original positive-base/exponent power without an epsilon bias. */
float classic_lobe_power(float base, float exponent)
{
    if (exponent == 0.0)
        return 1.0;
    if (base <= 0.0)
        return 0.0;
    return pow(base, exponent);
}

void main()
{
    vec2 uv = v_texcoord0 * u_uvTransform.xy + u_uvTransform.zw;
    vec4 texel = texture2D(s_albedo, uv) * u_baseColorFactor;
    if (u_emissiveFactor.w == 1.0 && texel.a < u_pbrParams.w) discard;
    vec3 N = normalize(v_normal);
    vec3 V = normalize(u_cameraPos.xyz - v_worldpos);
    float h = clamp(N.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 sky = u_ambientColor.xyz;
    vec3 ambient = mix(sky, mix(u_lookHemiGround.xyz, sky, h), u_lookHemiGround.w) * u_ambientColor.w;
    vec3 diffuse = ambient;
    vec3 specular = vec3_splat(0.0);
    float shine = max(0.0, (1.0-u_pbrParams.y)*128.0);
    vec3 hemiL = vec3(0.0,1.0,0.0);
    vec3 hemiH = normalize(hemiL+V);
    vec3 groundH = normalize(-hemiL+V);
    float norm = (shine+2.0)/8.0;
    float skyF = u_pbrParams.x + (1.0-u_pbrParams.x)*pow(max(1.0-dot(hemiL,hemiH),0.0),5.0);
    float groundF = u_pbrParams.x + (1.0-u_pbrParams.x)*pow(max(1.0-dot(-hemiL,groundH),0.0),5.0);
    float specMask = texture2D(s_metalRough, uv).r;
    if(u_pbrParams.x>0.0) specular += ambient*specMask*norm*
        (skyF*classic_lobe_power(max(0.5*dot(N,hemiH)+0.5,0.0),shine)*max(N.y,0.0)
        +groundF*classic_lobe_power(max(0.5*dot(N,groundH)+0.5,0.0),shine)*max(-N.y,0.0));
    for (int i=0; i<2; ++i) {
        if (float(i) < u_lightCounts.x) {
            vec3 L = normalize(-u_dirLights[i*2].xyz);
            vec3 H = normalize(L+V);
            vec3 radiance = u_dirLights[i*2+1].xyz * u_dirLights[i*2].w;
            float ndl = max(dot(N,L),0.0);
            float shadow = 1.0;
            if (float(i)+1.0 == u_lightCounts.w && u_normalScale.w < 0.5)
                shadow = csm_shadow_factor(v_worldpos,N,L,v_viewdepth);
            diffuse += radiance*ndl*shadow;
            float fresnel = u_pbrParams.x + (1.0-u_pbrParams.x)*pow(max(1.0-dot(L,H),0.0),5.0);
            float highlight = classic_lobe_power(max(dot(N,H),0.0),shine)*(shine+2.0001)/8.0;
            if(u_pbrParams.x>0.0) specular += radiance*ndl*highlight*fresnel*specMask*shadow;
        }
    }
    vec3 color = texel.rgb*(diffuse+u_emissiveFactor.xyz)+specular;
    gl_FragColor = vec4(color,texel.a);
}
