/* Absolute scale of the PHYSICAL sky, see the derivation at its use site.
 * A single named constant rather than a number buried in an expression,
 * because it is the one figure in this shader that is calibrated against a
 * measurement instead of derived from the atmosphere. */
#define JCE_SKY_SCALE 0.21

/* Perez F(theta,gamma): abcde = (A,B,C,D,E). Identical to sky_perez(). */
float sky_perez(vec4 abcd, float E, float cos_theta, float gamma)
{
    float cg = cos(gamma);
    return (1.0 + abcd.x * exp(abcd.y / cos_theta))
         * (1.0 + abcd.z * exp(abcd.w * gamma) + E * cg * cg);
}
