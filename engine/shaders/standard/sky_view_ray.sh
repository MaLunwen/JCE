    /* Reconstruct world-space view ray direction.  The NDC near-plane z is
     * backend-dependent: OpenGL clip space is [-1,1] (near = -1) while
     * D3D/Vulkan/Metal are [0,1] (near = 0).  Unprojecting z = -1 on a [0,1]
     * backend yields a point OUTSIDE the frustum (negative w after the
     * homogeneous divide) → a garbage ray → the dome gradient samples the
     * wrong band → dark sky on D3D/VK (OpenGL stayed correct). */
#if BGFX_SHADER_LANGUAGE_GLSL
    float ndcNearZ = -1.0;
#else
    float ndcNearZ =  0.0;
#endif
    vec4 nearH = mul(u_invViewProj, vec4(v_texcoord0, ndcNearZ, 1.0));
    vec4 farH  = mul(u_invViewProj, vec4(v_texcoord0,      1.0, 1.0));
    vec3 dir   = normalize(farH.xyz / farH.w - nearH.xyz / nearH.w);
