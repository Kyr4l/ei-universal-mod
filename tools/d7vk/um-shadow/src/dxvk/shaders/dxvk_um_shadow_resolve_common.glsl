layout(set = 0, binding = 0) uniform sampler s_samplers[];
#if defined(UM_MS) || defined(UM_MS_EDGE)
layout(set = 1, binding = 0) uniform texture2DMS s_depth_ms;
#else
layout(set = 1, binding = 0) uniform texture2D s_depth;
#endif
layout(set = 1, binding = 1) uniform texture2D s_map;

layout(push_constant)
uniform um_shadow_resolve_info_t {
  mat4  m_screen_to_world;   // (x*cw, y*cw, z*cw, cw) -> world (homogeneous)
  mat4  m_world_to_light;    // world -> light clip (x, y in -1..1, z in 0..1)
  vec4  light_dir_strength;  // xyz: direction the light TRAVELS, w: shadow strength 0..1
  vec4  sun_color_bias;      // rgb: sun colour, w: depth bias (light depth units)
  vec4  ambient_slope;       // rgb: ambient colour, w: slope-scaled bias
  vec4  depth_map;           // x: K = -m14 * m11, y: m10, z: m14 (cw = K / (m10 - z * m14)), w: extra darkness in shadows
  vec4  cam_map_size;        // xyz: camera position (world), w: shadow map size in texels
  uint  sampler_cmp;
  uint  debug_mode;
  uint  pad0;
  uint  pad1;
};

layout(constant_id = 0) const int samples_count = 1;

layout(location = 0) in vec2 i_coord;
layout(location = 0) out vec4 o_color;

#ifdef UM_MS_EDGE
#define SAMPLE_INDEX gl_SampleID
#else
#define SAMPLE_INDEX 0
#endif

float fetch_depth(ivec2 p) {
#if defined(UM_MS) || defined(UM_MS_EDGE)
  return texelFetch(s_depth_ms, p, SAMPLE_INDEX).r;
#else
  return texelFetch(s_depth, p, 0).r;
#endif
}

vec3 world_at(ivec2 p, float zs) {
  float cw = depth_map.x / (depth_map.y - zs * depth_map.z);
  vec4 w4 = m_screen_to_world * vec4((vec2(p) + 0.5f + vec2(uintBitsToFloat(pad0), uintBitsToFloat(pad1))) * cw, zs * cw, cw);
  return w4.xyz / w4.w;
}

void main() {
  ivec2 ip = ivec2(gl_FragCoord.xy);

#if defined(UM_MS) || defined(UM_MS_EDGE)
  // Multisampled colour / depth: pixels whose samples hit different surfaces (object edges) are shaded per sample, the rest once per pixel
  float zmin = 2.0f, zmax = -1.0f;
  for (int i = 0; i < samples_count; i++) {
    float z = texelFetch(s_depth_ms, ip, i).r;
    zmin = min(zmin, z);
    zmax = max(zmax, z);
  }
  bool edge = (zmax - zmin) > 3e-4f;
#ifdef UM_MS_EDGE
  if (!edge) discard;
#else
  if (edge) discard;
#endif
#endif

  float zs = fetch_depth(ip);

  // Sky / cleared depth: nothing to shade (no early exit: keeps the shader simple for the quad)
  bool is_sky = zs >= 0.99999f;

  vec3 wp = world_at(ip, zs);

  // Surface normal from the neighbouring depth samples, two pixels away (the game's depth buffer is only 16 bits: one pixel away the steps are as
  // small as the quantisation and the normal flickers with the camera). The smaller step on each axis: no smearing over depth edges. Facing the camera.
#if defined(UM_MS) || defined(UM_MS_EDGE)
  ivec2 dsize = textureSize(s_depth_ms) - ivec2(1);
#else
  ivec2 dsize = textureSize(s_depth, 0) - ivec2(1);
#endif
  ivec2 ql = clamp(ip + ivec2(-2, 0), ivec2(0), dsize), qr = clamp(ip + ivec2(2, 0), ivec2(0), dsize);
  ivec2 qu = clamp(ip + ivec2(0, -2), ivec2(0), dsize), qd = clamp(ip + ivec2(0, 2), ivec2(0), dsize);
  float zl = fetch_depth(ql), zr = fetch_depth(qr);
  float zu = fetch_depth(qu), zd = fetch_depth(qd);
  vec3 pl = world_at(ql, zl), pr = world_at(qr, zr);
  vec3 pu = world_at(qu, zu), pd = world_at(qd, zd);
  vec3 dx = (abs(zr - zs) < abs(zs - zl)) ? (pr - wp) : (wp - pl);
  vec3 dy = (abs(zd - zs) < abs(zs - zu)) ? (pd - wp) : (wp - pu);
  vec3 n = cross(dx, dy);
  float nl = length(n);
  n = nl > 1e-12f ? n / nl : vec3(0.0f, 0.0f, 1.0f);

  if (dot(n, cam_map_size.xyz - wp) < 0.0f)
    n = -n;

  vec3 to_sun = -light_dir_strength.xyz;
  float ndl = max(dot(n, to_sun), 0.0f);

  // Normal offset: the receiving point is moved along its normal by one to three shadow texels (more where the surface is edge-on to the sun) before the lookup: no acne on slopes
  // (river banks, cliffs, terrain facing away) and so nothing to flicker as the camera or the sun moves a little
  float world_texel = 2.0f / (length(vec3(m_world_to_light[0][0], m_world_to_light[1][0], m_world_to_light[2][0])) * cam_map_size.w);
  vec3 wp_offset = wp + n * world_texel * (1.0f + 2.0f * (1.0f - ndl));

  vec4 lp = m_world_to_light * vec4(wp_offset, 1.0f);
  vec2 uv = lp.xy * 0.5f + 0.5f;
  float bias = sun_color_bias.w + ambient_slope.w * (1.0f - ndl);
  float depth = lp.z - bias;

  float texel = 1.0f / cam_map_size.w;
  float lit = 0.0f;

  // 7x7 taps of the hardware 2x2 compare, weighted by distance (a soft penumbra of about four texels: swaying leaves and moving units do not make the edges flicker)
  float wsum = 0.0f;
  for (int y = -3; y <= 3; y++) {
    for (int x = -3; x <= 3; x++) {
      vec2 o = vec2(x, y);
      float w = max(0.0f, 1.0f - length(o) / 4.2f);
      lit += w * texture(sampler2DShadow(s_map, s_samplers[sampler_cmp]), vec3(uv + o * texel, depth));
      wsum += w;
    }
  }

  lit /= wsum;

  // Outside the shadow map: no shadow
  if (any(lessThan(uv, vec2(0.0f))) || any(greaterThan(uv, vec2(1.0f))) || lp.z < 0.0f || lp.z > 1.0f)
    lit = 1.0f;

  float s = clamp((1.0f - lit) * light_dir_strength.w, 0.0f, 1.0f);

  // The game lights with colour = max(ambient, N.L * sun) (computed per vertex, smooth): a surface the sun does not reach keeps the ambient part only.
  // The same ratio for every surface that faces the sun (the estimated normals are faceted: using N.L here would show the facets inside the shadows),
  // measured on a surface facing straight up; surfaces turned away from the sun get no extra darkening (they are lit by the ambient light already).
  vec3 sun_up = sun_color_bias.rgb * max(-light_dir_strength.z, 0.15f);
  vec3 k = ambient_slope.rgb / max(max(ambient_slope.rgb, sun_up), vec3(0.004f));
  float facing = smoothstep(0.05f, 0.35f, ndl);
  vec3 factor = mix(vec3(1.0f), k, s * facing) * (1.0f - clamp(depth_map.w, 0.0f, 1.0f) * s * facing);

  if (debug_mode == 1u)
    factor = vec3(1.0f - s);
  else if (debug_mode == 2u)
    factor = vec3(lit, ndl, 0.0f);
  else if (debug_mode == 3u)
    factor = fract(wp * 0.1f);
  else if (debug_mode == 5u)
    factor = vec3(wp.x * 0.01f, wp.y * 0.01f, wp.z * 0.1f);
  else if (debug_mode == 10u)
    factor = vec3(s);
  else if (debug_mode == 4u)
    factor = n * 0.5f + 0.5f;
  else if (debug_mode == 6u)
    factor = normalize(dx) * 0.5f + 0.5f;
  else if (debug_mode == 7u)
    factor = normalize(dy) * 0.5f + 0.5f;
  else if (debug_mode == 8u)
    factor = vec3(abs(dx) * 20.0f);
  else if (debug_mode == 9u)
    factor = vec3(abs(dy) * 20.0f);

  if (is_sky)
    factor = vec3(1.0f);

  o_color = vec4(factor, 1.0f);
}
