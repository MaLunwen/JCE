        /* ── Preetham analytic daylight (mode 2) ──────────────────── */
        /* theta = view-zenith angle; clamp cos just above 0 (sky hemi). */
        float cos_theta = clamp(dir.y, 0.01, 1.0);

        float cos_gamma = clamp(dot(dir, u_sky_sun_dir.xyz), -1.0, 1.0);
        float gamma     = acos(cos_gamma);

        float cos_ts  = clamp(u_sky_sun_dir.y, 0.01, 1.0);
        float theta_s = acos(cos_ts);

        /* F(0, theta_s): cos(0)=1, gamma=theta_s. */
        float fY0 = sky_perez(u_sky_perez[0], u_sky_perez[3].x, 1.0, theta_s);
        float fx0 = sky_perez(u_sky_perez[1], u_sky_perez[3].y, 1.0, theta_s);
        float fy0 = sky_perez(u_sky_perez[2], u_sky_perez[3].z, 1.0, theta_s);
        fY0 = (abs(fY0) < 1e-6) ? 1e-6 : fY0;
        fx0 = (abs(fx0) < 1e-6) ? 1e-6 : fx0;
        fy0 = (abs(fy0) < 1e-6) ? 1e-6 : fy0;

        float Y = u_sky_zenith.x * sky_perez(u_sky_perez[0], u_sky_perez[3].x, cos_theta, gamma) / fY0;
        float x = u_sky_zenith.y * sky_perez(u_sky_perez[1], u_sky_perez[3].y, cos_theta, gamma) / fx0;
        float y = u_sky_zenith.z * sky_perez(u_sky_perez[2], u_sky_perez[3].z, cos_theta, gamma) / fy0;

        Y = max(Y, 0.0);
        if (u_sky_zenith.w > 0.5) {
            float Yz = max(u_sky_zenith.x, 1e-6);
            Y /= Yz;
        }

        /* xyY -> XYZ (guard y). */
        y = max(y, 1e-4);
        float X = (x / y) * Y;
        float Z = ((1.0 - x - y) / y) * Y;

        /* XYZ -> linear sRGB (D65) — mirrors jce_sky_radiance(). */
        vec3 col;
        col.r =  3.2404542 * X - 1.5371385 * Y - 0.4985314 * Z;
        col.g = -0.9692660 * X + 1.8760108 * Y + 0.0415560 * Z;
        col.b =  0.0556434 * X - 0.2040259 * Y + 1.0572252 * Z;

        col *= u_sky_params.y;          /* exposure */
        col  = max(col, vec3_splat(0.0));
