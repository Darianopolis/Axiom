#include "axiom_Common.glsl"

// ACES: https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/
//       https://github.com/TheRealMJP/BakingLab/blob/master/BakingLab/ACES.hlsl

const mat3 ACESInputMat = mat3(
    vec3(0.59719, 0.35458, 0.04823),
    vec3(0.07600, 0.90834, 0.01566),
    vec3(0.02840, 0.13383, 0.83777)
);

const mat3 ACESOutputMat = mat3(
    vec3( 1.60475, -0.53108, -0.07367),
    vec3(-0.10208,  1.10813, -0.00605),
    vec3(-0.00327, -0.07276,  1.07602)
);

vec3 RRTAndODTFit(vec3 v)
{
    vec3 a = v * (v + 0.0245768) - 0.000090537;
    vec3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return a / b;
}

vec3 SaturateVec3(vec3 v)
{
    return vec3(clamp(v.x, 0, 1), clamp(v.y, 0, 1), clamp(v.z, 0, 1));
}

vec3 filmic(vec3 x) {
  vec3 X = max(vec3(0.0), x - 0.004);
  vec3 result = (X * (6.2 * X + 0.5)) / (X * (6.2 * X + 1.7) + 0.06);
  return pow(result, vec3(2.2));
}

vec3 lottes(vec3 x) {
  const vec3 a = vec3(1.6);
  const vec3 d = vec3(0.977);
  const vec3 hdrMax = vec3(8.0);
  const vec3 midIn = vec3(0.18);
  const vec3 midOut = vec3(0.267);

  const vec3 b =
      (-pow(midIn, a) + pow(hdrMax, a) * midOut) /
      ((pow(hdrMax, a * d) - pow(midIn, a * d)) * midOut);
  const vec3 c =
      (pow(hdrMax, a * d) * pow(midIn, a) - pow(hdrMax, a) * pow(midIn, a * d) * midOut) /
      ((pow(hdrMax, a * d) - pow(midIn, a * d)) * midOut);

  return pow(x, a) / (pow(x, a * d) * b + c);
}

vec3 reinhard(vec3 x) {
  return x / (1.0 + x);
}

vec3 reinhard2(vec3 x) {
  const float L_white = 4.0;

  return (x * (1.0 + x / (L_white * L_white))) / (1.0 + x);
}

vec3 uchimura(vec3 x, float P, float a, float m, float l, float c, float b) {
  float l0 = ((P - m) * l) / a;
  float L0 = m - m / a;
  float L1 = m + (1.0 - m) / a;
  float S0 = m + l0;
  float S1 = m + a * l0;
  float C2 = (a * P) / (P - S1);
  float CP = -C2 / P;

  vec3 w0 = vec3(1.0 - smoothstep(0.0, m, x));
  vec3 w2 = vec3(step(m + l0, x));
  vec3 w1 = vec3(1.0 - w0 - w2);

  vec3 T = vec3(m * pow(x / m, vec3(c)) + b);
  vec3 S = vec3(P - (P - S1) * exp(CP * (x - S0)));
  vec3 L = vec3(m + a * (x - m));

  return T * w0 + L * w1 + S * w2;
}

vec3 uchimura(vec3 x) {
  const float P = 1.0;  // max display brightness
  const float a = 1.0;  // contrast
  const float m = 0.22; // linear section start
  const float l = 0.4;  // linear section length
  const float c = 1.33; // black
  const float b = 0.0;  // pedestal

  return uchimura(x, P, a, m, l, c, b);
}

vec3 uncharted2Tonemap(vec3 x) {
  float A = 0.15;
  float B = 0.50;
  float C = 0.10;
  float D = 0.20;
  float E = 0.02;
  float F = 0.30;
  float W = 11.2;
  return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

vec3 uncharted2(vec3 color) {
  const float W = 11.2;
  float exposureBias = 2.0;
  vec3 curr = uncharted2Tonemap(exposureBias * color);
  vec3 whiteScale = 1.0 / uncharted2Tonemap(vec3(W));
  return curr * whiteScale;
}

vec3 unreal(vec3 x) {
  return x / (x + 0.155) * 1.019;
}

const float saturation_compression = 0.1f;
const float saturation_boost = 0.3f;
vec3 tone_scale(vec3 color) { return color; }

vec2 create_modified_primary(vec2 input_primary, vec2 white_point, float scaling, float rotation)
{
    vec2 output_primary = input_primary - white_point;

    float primary_length = (1.0 + scaling) * length(output_primary);
    float primary_angle = atan(output_primary.y, output_primary.x) + rotation;

    output_primary = vec2(primary_length * cos(primary_angle), primary_length * sin(primary_angle));

    return output_primary + white_point;
}

vec3 xy_to_xyz(vec2 input_color)
{
    return vec3(input_color.x / input_color.y, 1.0, (1.0 - (input_color.x + input_color.y)) / input_color.y);
}

vec3 agx_tonemapper(vec3 input_color)
{
    const vec2 srgb_red_primary = vec2(0.64, 0.33);
    const vec2 srgb_green_primary = vec2(0.3, 0.6);
    const vec2 srgb_blue_primary = vec2(0.15, 0.06);

    const vec2 srgb_white_point = vec2(0.3127, 0.329);

    mat3 srgb_xyz_matrix = mat3(vec3(0.4124, 0.2126, 0.0193), vec3(0.3576, 0.7152, 0.1192), vec3(0.1805, 0.0722, 0.9505));
    mat3 xyz_srgb_matrix = inverse(srgb_xyz_matrix);

    vec2 modified_red_primary = create_modified_primary(srgb_red_primary, srgb_white_point, saturation_compression, -0.0325);
    vec2 modified_green_primary = create_modified_primary(srgb_green_primary, srgb_white_point, saturation_compression, 0.0);
    vec2 modified_blue_primary = create_modified_primary(srgb_blue_primary, srgb_white_point, saturation_compression, 0.0374);

    mat3 modified_rgb_xyz_matrix = mat3(xy_to_xyz(modified_red_primary), xy_to_xyz(modified_green_primary), xy_to_xyz(modified_blue_primary));

    vec3 white_point_scale = inverse(modified_rgb_xyz_matrix) * xy_to_xyz(srgb_white_point);
    modified_rgb_xyz_matrix = matrixCompMult(modified_rgb_xyz_matrix, mat3(white_point_scale.xxx, white_point_scale.yyy, white_point_scale.zzz));

    mat3 xyz_modified_rgb_matrix = inverse(modified_rgb_xyz_matrix);

    vec3 modified_color = xyz_modified_rgb_matrix * srgb_xyz_matrix * input_color;

    modified_color = tone_scale(modified_color);
    modified_color = mix(vec3(dot(modified_color, white_point_scale)), modified_color, 1.0 + saturation_boost);

    return clamp(xyz_srgb_matrix * modified_rgb_xyz_matrix * modified_color, 0.0, 1.0);
}

layout(push_constant, scalar) uniform pc_ {
    uvec2     size;
    uint    source;
    uint    target;
    float exposure;
    int       mode;
} pc;

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;
void main()
{
    ivec2 pos = ivec2(gl_GlobalInvocationID);
    if (pos.x >= pc.size.x || pos.y >= pc.size.y)
        return;

    vec4 source = imageLoad(RWImage2D[pc.source], pos);
    vec3 color = source.rgb;

    // Exposure mapping

    if (pc.exposure > 0) {
        color = vec3(1.0) - exp(-color * pc.exposure);
    }

    bool noSRGBCorrection = false;
    switch (pc.mode) {

        case 1: // ACES tone mapping
            color = color * ACESInputMat;
            color = RRTAndODTFit(color);
            color = color * ACESOutputMat;
            color = SaturateVec3(color);
        break;case 2:
            color = filmic(color);
        break;case 3:
            color = lottes(color);
        break;case 4:
            color = reinhard(color);
        break;case 5:
            color = reinhard2(color);
        break;case 6:
            color = uchimura(color);
        break;case 7:
            color = uncharted2(color);
        break;case 8:
            color = unreal(color);
            noSRGBCorrection = true;
        break;case 9:
            color = agx_tonemapper(color);
    }

    // Apply sRGB correction

    if (!noSRGBCorrection) {
        color = Apply_sRGB_OETF(color.rgb);
    }

    imageStore(RWImage2D[pc.target], pos, vec4(color, 1.0));
}