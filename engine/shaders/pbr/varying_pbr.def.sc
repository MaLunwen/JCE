vec2 v_texcoord0 : TEXCOORD0 = vec2(0.0, 0.0);
vec3 v_worldpos  : TEXCOORD1 = vec3(0.0, 0.0, 0.0);
vec3 v_normal    : TEXCOORD2 = vec3(0.0, 1.0, 0.0);
vec3 v_tangent   : TEXCOORD3 = vec3(1.0, 0.0, 0.0);
vec3 v_bitangent : TEXCOORD4 = vec3(0.0, 0.0, 1.0);
float v_viewdepth : TEXCOORD5 = 0.0;

vec3 a_position  : POSITION;
vec3 a_normal    : NORMAL;
vec4 a_tangent   : TANGENT;
vec2 a_texcoord0 : TEXCOORD0;
vec4 a_indices   : BLENDINDICES;
vec4 a_weight    : BLENDWEIGHT;
