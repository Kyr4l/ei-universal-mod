#version 450

#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_nonuniform_qualifier : require

#include "dxvk_um_shadow_common.glsl"

layout(set = 0, binding = 0) uniform sampler s_samplers[];
layout(set = 1, binding = 0) uniform texture2D s_tex;

layout(location = 0) in vec2 i_uv;
layout(location = 1) in float i_alpha;

void main() {
  float a = i_alpha;

  if (alpha_mode == 2u)
    a *= texture(sampler2D(s_tex, s_samplers[sampler_index]), i_uv).a;

  // a lower threshold than the game's: leaves and grass cast fuller shadows, so the cut-out pattern does not speckle as the wind moves them
  if (a < alpha_ref * 0.5)
    discard;
}
