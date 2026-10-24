#version 450

#extension GL_GOOGLE_include_directive : enable

#include "dxvk_um_shadow_common.glsl"

layout(location = 0) in vec4 i_pos;       // x, y, z, rhw
layout(location = 1) in vec4 i_diffuse;   // D3DCOLOR (b, g, r, a bytes): only .a is used (byte 3)
layout(location = 2) in vec2 i_uv;

layout(location = 0) out vec2 o_uv;
layout(location = 1) out float o_alpha;

void main() {
  float cw = 1.0f / i_pos.w;
  vec4 p = vec4(i_pos.xy * cw, i_pos.z * cw, cw);
  gl_Position = m_screen_to_light * p;
  o_uv = i_uv;
  o_alpha = i_diffuse.a;
}
