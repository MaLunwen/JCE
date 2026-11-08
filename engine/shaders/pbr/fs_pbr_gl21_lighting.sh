    // Keep the light budget bounded for GL 2.1 fragment translators.
    for (int i = 0; i < 2; ++i)
    {
        if (float(i) >= u_lightCounts.x)
            break;
        vec4 direction = u_dirLights[i * 2];
        vec3 L = safe_normalize_vec3(-direction.xyz, N);
        vec3 radiance = u_dirLights[i * 2 + 1].rgb * direction.w;
        Lo += cookTorranceBRDFWrap(N, V, L, F0, albedo, metallic,
                                   roughness, u_lookWrap.x) * radiance;
    }

    for (int i = 0; i < 4; ++i)
    {
        if (float(i) >= u_lightCounts.y)
            break;
        vec4 position = u_pointLights[i * 2];
        vec4 light = u_pointLights[i * 2 + 1];
        vec3 offset = position.xyz - v_worldpos;
        float radius2 = max(position.w * position.w, 1e-8);
        float attenuation = clamp(1.0 - dot(offset, offset) / radius2, 0.0, 1.0);
        vec3 radiance = light.rgb * (light.w * attenuation * attenuation);
        vec3 L = safe_normalize_vec3(offset, N);
        Lo += cookTorranceBRDFWrap(N, V, L, F0, albedo, metallic,
                                   roughness, u_lookWrap.x) * radiance;
    }

    vec3 skyAmbient = u_ambientColor.rgb * u_ambientColor.w;
    float h = clamp(N.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 hemiColor = mix(u_lookHemiGround.rgb, skyAmbient, h);
    vec3 ambient = mix(skyAmbient, hemiColor, u_lookHemiGround.w) * albedo * ao;
