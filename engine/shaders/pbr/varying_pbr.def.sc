vec2 v_texcoord0 : TEXCOORD0 = vec2(0.0, 0.0);
vec3 v_worldpos  : TEXCOORD1 = vec3(0.0, 0.0, 0.0);
vec3 v_normal    : TEXCOORD2 = vec3(0.0, 1.0, 0.0);
vec3 v_tangent   : TEXCOORD3 = vec3(1.0, 0.0, 0.0);
vec3 v_bitangent : TEXCOORD4 = vec3(0.0, 0.0, 1.0);
float v_viewdepth : TEXCOORD5 = 0.0;
vec3 v_localpos  : TEXCOORD6 = vec3(0.0, 0.0, 0.0);
vec4 v_tint      : COLOR0    = vec4(1.0, 1.0, 1.0, 1.0);
// TAA motion-vector pass: current/previous clip-space position (un-jittered).
// Only used by vs/fs_gbuffer_vel[_skinned]; ignored by every other pbr shader.
// (bgfx 1.146 shaderc rejects /* */ block comments in varying.def.sc.)
vec4 v_curClip   : TEXCOORD8 = vec4(0.0, 0.0, 0.0, 1.0);
vec4 v_prevClip  : TEXCOORD9 = vec4(0.0, 0.0, 0.0, 1.0);

vec3 a_position  : POSITION;
vec3 a_normal    : NORMAL;
vec4 a_tangent   : TANGENT;
vec2 a_texcoord0 : TEXCOORD0;
vec4 a_indices   : BLENDINDICES;
vec4 a_weight    : BLENDWEIGHT;

vec4 i_data0     : TEXCOORD7;
vec4 i_data1     : TEXCOORD6;
vec4 i_data2     : TEXCOORD5;
vec4 i_data3     : TEXCOORD4;
vec4 i_data4     : TEXCOORD3;
