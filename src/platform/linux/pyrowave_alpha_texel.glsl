// Per-texel alpha class. This file is compiled verbatim into pyrowave_alpha.comp
// and into the CPU contract tests, so keep it valid as both GLSL 4.50 and C++.
//
// `texel` is one unswizzled UNORM sample of the owned snapshot in its own
// VkFormat. Channel minimum and maximum convert to exactly 0.0 and 1.0, so every
// comparison below is exact and there is no tolerance.
const uint pyrowave_alpha_opaque = 0u;
const uint pyrowave_alpha_transparent_black = 1u;
const uint pyrowave_alpha_other = 2u;

uint pyrowave_alpha_texel_class(vec4 texel) {
  if (texel.a == 1.0) {
    return pyrowave_alpha_opaque;  // Any color, including black.
  }
  if (texel.a == 0.0 && texel.r == 0.0 && texel.g == 0.0 && texel.b == 0.0) {
    return pyrowave_alpha_transparent_black;
  }
  return pyrowave_alpha_other;  // Fractional alpha, or alpha zero with color.
}
