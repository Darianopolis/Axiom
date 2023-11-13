#extension GL_EXT_scalar_block_layout                    : require
#extension GL_EXT_buffer_reference2                      : require
#extension GL_EXT_nonuniform_qualifier                   : require
#extension GL_EXT_shader_image_load_formatted            : require
#extension GL_EXT_shader_explicit_arithmetic_types_int8  : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

layout(set = 0, binding = 0) uniform texture2D Image2D[];
layout(set = 0, binding = 1) uniform image2D RWImage2D[];
layout(set = 0, binding = 2) uniform sampler   Sampler[];

float PI = 3.14159265358979323846264338327950288419716939937510;

vec3 SignedOctDecode_(vec3 n)
{
    vec3 OutN;

    OutN.x = (n.x - n.y);
    OutN.y = (n.x + n.y) - 1.0;
    OutN.z = n.z * 2.0 - 1.0;
    OutN.z = OutN.z * (1.0 - abs(OutN.x) - abs(OutN.y));

    OutN = normalize(OutN);
    return OutN;
}

vec3 SignedOctDecode(uint tgtSpace)
{
    float x = float(bitfieldExtract(tgtSpace, 0, 10)) / 1023.0;
    float y = float(bitfieldExtract(tgtSpace, 10, 10)) / 1023.0;
    float s = float(bitfieldExtract(tgtSpace, 20, 1));

    return SignedOctDecode_(vec3(x, y, s));
}

vec2 DecodeDiamond(float p)
{
    vec2 v;

    // Remap p to the appropriate segment on the diamond
    float p_sign = sign(p - 0.5);
    v.x = -p_sign * 4.0 * p + 1.0 + p_sign * 2.0;
    v.y = p_sign * (1.0 - abs(v.x));

    // Normalization extends the point on the diamond back to the unit circle
    return normalize(v);
}

vec3 DecodeTangent_(vec3 normal, float diamondTangent)
{
    // As in the encode step, find our canonical tangent basis span(t1, t2)
    vec3 t1;
    if (abs(normal.y) > abs(normal.z)) {
        t1 = vec3(normal.y, -normal.x, 0.f);
    } else {
        t1 = vec3(normal.z, 0.f, -normal.x);
    }
    t1 = normalize(t1);

    vec3 t2 = cross(t1, normal);

    // Recover the coordinates used with t1 and t2
    vec2 packedTangent = DecodeDiamond(diamondTangent);

    return packedTangent.x * t1 + packedTangent.y * t2;
}

vec3 DecodeTangent(vec3 normal, uint tgtSpace)
{
    float tgtAngle = float(bitfieldExtract(tgtSpace, 21, 10)) / 1023.0;
    return DecodeTangent_(normal, tgtAngle);
}

// sRGB linear -> compressed
vec3 Apply_sRGB_OETF(vec3 linear)
{
    bvec3 cutoff = lessThan(linear, vec3(0.0031308));
    vec3 higher = vec3(1.055) * pow(linear, vec3(1.0 / 2.4)) - vec3(0.055);
    vec3 lower = linear * vec3(12.92);

    return mix(higher, lower, cutoff);
}

// sRGB compressed -> linear
vec3 Apply_sRGB_EOTF(vec3 compressed)
{
    bvec3 cutoff = lessThan(compressed, vec3(0.04045));
    vec3 higher = pow((compressed + vec3(0.055)) / vec3(1.055), vec3(2.4));
    vec3 lower = compressed / vec3(12.92);

    return mix(higher, lower, cutoff);
}

vec3 DebugSNorm(vec3 snorm)
{
    return snorm * 0.5 + 0.5;
}

vec3 DecodeNormalMap(vec3 mapped)
{
    return clamp(((mapped * 255) - 127) / 127, -1, 1);
}

float sqr(float x) { return x*x; }

float sdot(vec3 x, vec3 y, float f)
{
    return clamp(dot(x, y) * f, 0, 1);
}

bool IsNanOrInf(vec3 v)
{
    float t = v.x + v.y + v.z;
    return isnan(t) || isinf(t);
}

vec3 OffsetPointByNormal(vec3 p, vec3 n)
{
    p = p + 0.0001 * n;
    const float Origin = 1.0 / 32.0;
    const float FloatScale = 1.0 / 65536.0;
    const float IntScale = 256.0;

    ivec3 offsetInt = ivec3(IntScale * n.x, IntScale * n.y, IntScale * n.z);

    vec3 positionInt = vec3(
        intBitsToFloat(floatBitsToInt(p.x) + ((p.x < 0) ? -offsetInt.x : offsetInt.x)),
        intBitsToFloat(floatBitsToInt(p.y) + ((p.y < 0) ? -offsetInt.y : offsetInt.y)),
        intBitsToFloat(floatBitsToInt(p.z) + ((p.z < 0) ? -offsetInt.z : offsetInt.z)));

    return vec3(
        abs(p.x) < Origin ? p.x + FloatScale * n.x : positionInt.x,
        abs(p.y) < Origin ? p.y + FloatScale * n.y : positionInt.y,
        abs(p.z) < Origin ? p.z + FloatScale * n.z : positionInt.z);
}

float LuminanceRGB(vec3 rgb)
{
    // https://stackoverflow.com/questions/596216/formula-to-determine-perceived-brightness-of-rgb-color
    // return 0.2126 * rgb.r + 0.7152 * rgb.g + 0.0722 * rgb.b;
    return 0.3 * rgb.r + 0.6 * rgb.g + 0.1 * rgb.b;
}


uvec2 rnd;

float RandomUNorm()
{
    const uint64_t A = 4294883355;
    uint x = rnd.x, c = rnd.y;
    uint res = x ^ c;
    uint64_t next = x * A + c;
    rnd.x = uint(next & 4294967295);
    rnd.y = uint(next >> 32);

    return 2.3283064365387e-10 * res;
}

bool IsInfZeroOrNan(vec3 V)
{
    float t = V.x + V.y + V.z;
    return t == 0 || isnan(t) || isinf(t);
}

vec3 GetTangent(vec3 N)
{
    float s = sign(N.z);
    float a = -1.0 / (s + N.z);
    float b = N.x * N.y * a;

    return vec3(1 + s * sqr(N.x) * a, s * b, -s * N.x);
}

vec3 RandomOnSphere()
{
    float z = 1 - 2 * RandomUNorm();
    float r = sqrt(max(0, 1 - z * z));
    float phi = 2 * PI * RandomUNorm();
    return vec3(r * cos(phi), r * sin(phi), z);
}

mat3 MakeTBN(vec3 N)
{
    float s = sign(N.z);
    if (s == 0) s = 1;
    float a = -1.0 / (s + N.z);
    float b = N.x * N.y * a;

    vec3 T = vec3(1 + s * sqr(N.x) * a, s * b, -s * N.x);
    vec3 B = vec3(b, s + sqr(N.y) * a, -N.y);

    return mat3(T, B, N);
}

vec3 ChangeBasis(vec3 v, vec3 N)
{
    float s = sign(N.z);
    float a = -1.0 / (s + N.z);
    float b = N.x * N.y * a;

    vec3 T = vec3(1 + s * sqr(N.x) * a, s * b, -s * N.x);
    vec3 B = vec3(b, s + sqr(N.y) * a, -N.y);

    return normalize(
          (v.x * T)
        + (v.y * B)
        + (v.z * N));
}

vec3 RandomOnCone(vec3 dir, float cosThetaMax)
{
    float u0 = RandomUNorm();
    float cosTheta = (1 - u0) + u0 * cosThetaMax;
    float sinTheta = sqrt(1 - cosTheta * cosTheta);
    float phi = 2 * PI * RandomUNorm();

    vec3 v1 = vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
    return ChangeBasis(v1, dir);
}

vec2 ConcentricSampleDisk()
{
    vec2 uOffset = 2 * vec2(RandomUNorm(), RandomUNorm()) - vec2(1, 1);

    if (uOffset.x == 0 && uOffset.y == 0)
        return vec2(0, 0);

    float theta, r;
    if (abs(uOffset.x) > abs(uOffset.y)) {
        r = uOffset.x;
        theta = PI/4 * (uOffset.y / uOffset.x);
    } else {
        r = uOffset.y;
        theta = PI/2 - (PI/4 * uOffset.x / uOffset.y);
    }

    return r * vec2(cos(theta), sin(theta));
}

vec3 CosineSampleHemisphere()
{
    vec2 d = ConcentricSampleDisk();
    float z = sqrt(max(0, 1 - d.x * d.x - d.y * d.y));
    return vec3(d.x, d.y, z);
}

float CosineSampleHemispherePDF(float cosTheta)
{
    return cosTheta / PI;
}

vec3 VCosineSampleHemisphere(float alpha)
{
    float cosTheta = pow(RandomUNorm(), 1 / (alpha + 1));
    float sinTheta = sqrt(1 - cosTheta * cosTheta);
    float phi = 2 * PI * RandomUNorm();
    return vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

float VCosineSampleHemispherePDF(vec3 v, float alpha)
{
    float cosTheta = v.z;
    return (cosTheta + alpha) * pow(cosTheta, alpha) / PI;
}