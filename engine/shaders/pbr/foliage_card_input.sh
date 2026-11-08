/* Expanded mesh input matches the instanced leaf data and the shared quad. */
#if defined(JCE_FOLIAGE_EXPANDED_CARDS)
#define JCE_FOLIAGE_CORNER (vec3(a_texcoord0.x - 0.5, 0.5 - a_texcoord0.y, 0.0))
#define JCE_FOLIAGE_CENTER a_position
#define JCE_FOLIAGE_SIZE a_tangent.x
#define JCE_FOLIAGE_NORMAL a_normal
#define JCE_FOLIAGE_PHASE a_tangent.y
#else
#define JCE_FOLIAGE_CORNER a_position
#define JCE_FOLIAGE_CENTER i_data0.xyz
#define JCE_FOLIAGE_SIZE i_data0.w
#define JCE_FOLIAGE_NORMAL i_data1.xyz
#define JCE_FOLIAGE_PHASE i_data1.w
#endif
