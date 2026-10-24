// Universal Mod: sun shadow map pass. The game sends PRE-TRANSFORMED vertices (x, y in pixels, z in 0..1, rhw): the vertex shader
// turns them back into world space with one 4x4 matrix (light matrix * screen-to-world), so every depth-writing draw of the world
// can be drawn into the shadow map from the very same buffers.
layout(push_constant)
uniform um_shadow_depth_info_t {
  mat4  m_screen_to_light;
  uint  sampler_index;
  float alpha_ref;
  uint  alpha_mode;      // 0 none, 1 vertex alpha, 2 vertex alpha * texture alpha
  uint  pad;
};
