// Universal Mod: the water's border. A full-screen pass over the finished world (after the water layer): where a pixel's ground lies just below the water's surface (or a little above it) the
// shore gets a band of foam. The water's depth over the ground is a map sampled at the pixel's world position (one value per terrain vertex, filled by um-engine from the game's
// own terrain records: water surface height minus ground height), so the band is as wide as asked in world units on any slope.

#ifdef UM_MS
layout(set = 1, binding = 0) uniform texture2DMS s_depth;
#else
layout(set = 1, binding = 0) uniform texture2D s_depth;
#endif
layout(set = 1, binding = 1) uniform texture2D s_thick;

layout(push_constant)
uniform um_water_info_t {
  mat4  m_screen_to_world;   // (x*cw, y*cw, z*cw, cw) -> world (homogeneous)
  vec4  depth_map;           // x: K, y: m10 (far), z: m10 - near, w: unused
  vec4  tint_strength;       // rgb: light colour of the foam, w: strength 0..1
  vec4  shape;               // x: foam width (world units), y: wet strip outside the waterline, z: speed, w: time (s)
  vec4  guard_size;          // xy: guard band shift (pixels), zw: thickness map size
};

layout(location = 0) in vec2 i_coord;
layout(location = 0) out vec4 o_color;

vec3 world_at(ivec2 p, float zs) {
  float cw = depth_map.x / (depth_map.y - zs * depth_map.z);
  vec4 w4 = m_screen_to_world * vec4((vec2(p) + 0.5f + guard_size.xy) * cw, zs * cw, cw);
  return w4.xyz / w4.w;
}

vec2 thick_at(ivec2 c) {   // x: the water's depth over the ground (-100: no water), y: the water's level
  c = clamp(c, ivec2(0), ivec2(guard_size.zw) - ivec2(1));
  return texelFetch(s_thick, c, 0).rg;
}

float hash21(vec2 p) {
  p = fract(p * vec2(123.34f, 456.21f));
  p += dot(p, p + 45.32f);
  return fract(p.x * p.y);
}

float vnoise(vec2 p) {
  vec2 i = floor(p), f = fract(p);
  f = f * f * (3.0f - 2.0f * f);
  return mix(mix(hash21(i), hash21(i + vec2(1, 0)), f.x), mix(hash21(i + vec2(0, 1)), hash21(i + vec2(1, 1)), f.x), f.y);
}

void main() {
  ivec2 ip = ivec2(gl_FragCoord.xy);
  float zs = texelFetch(s_depth, ip, 0).r;   // (multisampled: the first sample)
  if (zs >= 0.99999f)
    discard;

  vec3 wp = world_at(ip, zs);

  // the water's depth over the ground at this point: bilinear in the vertex map; a corner without water (-100) means no shore here
  vec2 f = wp.xy;
  ivec2 i0 = ivec2(floor(f));
  vec2 w = f - vec2(i0);
  vec2 v00 = thick_at(i0), v10 = thick_at(i0 + ivec2(1, 0)), v01 = thick_at(i0 + ivec2(0, 1)), v11 = thick_at(i0 + ivec2(1, 1));
  if (min(min(v00.x, v10.x), min(v01.x, v11.x)) < -50.0f)
    discard;

  float t = mix(mix(v00.x, v10.x, w.x), mix(v01.x, v11.x, w.x), w.y);
  float level = mix(mix(v00.y, v10.y, w.x), mix(v01.y, v11.y, w.x), w.y);

  // only the ground (and the water over it) gets a shore: whatever stands well above the water's level (a bridge, a unit, a tree) does not
  if (wp.z > level + 0.3f)
    discard;

  vec2 grad = vec2(mix(v10.x - v00.x, v11.x - v01.x, w.y), mix(v01.x - v00.x, v11.x - v10.x, w.x));
  float raw_slope = length(grad);
  float slope = max(raw_slope, 0.04f);
  float flat_fade = smoothstep(0.04f, 0.14f, raw_slope);   // water as deep as the ground is low all over (a marsh): no sharp shoreline there, nothing to draw

  // distance to the waterline along the ground (world units): positive inside the water, negative on the dry side
  float dist = t / slope;
  float width = shape.x, wet = shape.y, speed = shape.z, time = shape.w;

  if (dist > width * 3.2f || dist < -wet)
    discard;

  float n = vnoise(wp.xy * 1.9f + vec2(time * 0.30f, -time * 0.18f) * speed) * 0.65f + vnoise(wp.xy * 4.7f - vec2(time * 0.22f, time * 0.35f) * speed) * 0.35f;
  float lap = 0.5f + 0.5f * sin(time * speed * 1.7f + n * 7.0f + wp.x * 0.35f + wp.y * 0.21f);   // the foam laps back and forth

  float strength = tint_strength.w;
  vec3 light = tint_strength.rgb;

  if (dist < 0.0f) {
    // the dry side: the ground is wet just above the waterline (darker, fading out over `wet`)
    float wetness = smoothstep(-wet, 0.0f, dist) * (0.65f + 0.35f * n);
    float a = wetness * 0.30f * strength * flat_fade;
    if (a < 0.003f) discard;
    o_color = vec4(vec3(0.04f, 0.035f, 0.03f), a);
    return;
  }

  // the water side: a broken line of foam right at the waterline, and the shallows a little paler than the deep water
  float reach = max(width * (0.40f + 0.60f * lap) * (0.35f + 1.30f * n), 0.02f);
  float foam = (1.0f - smoothstep(0.0f, reach, dist)) * (0.45f + 0.55f * n);
  float shallow = (1.0f - smoothstep(0.0f, width * 3.0f, dist)) * 0.16f;
  float a = clamp(foam * 0.80f + shallow, 0.0f, 1.0f) * strength * flat_fade;
  if (a < 0.003f)
    discard;

  vec3 foam_color = light * mix(vec3(0.62f, 0.74f, 0.80f), vec3(0.95f, 0.98f, 1.0f), clamp(foam * 1.6f, 0.0f, 1.0f));   // pale blue-green in the shallows, near white in the foam
  o_color = vec4(foam_color, a);
}
