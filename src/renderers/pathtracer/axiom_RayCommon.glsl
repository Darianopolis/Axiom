#extension GL_EXT_scalar_block_layout                    : require
#extension GL_EXT_buffer_reference2                      : require
#extension GL_EXT_nonuniform_qualifier                   : require
#extension GL_EXT_ray_tracing                            : require
#extension GL_EXT_ray_tracing_position_fetch             : require
#extension GL_NV_shader_invocation_reorder               : require
#extension GL_EXT_shader_image_load_formatted            : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

layout(set = 0, binding = 0) uniform image2D RWImage2D[];

layout(set = 0, binding = 0) uniform texture2D Image2D[];
layout(set = 0, binding = 0) uniform sampler Sampler[];

struct RayPayload {
    vec3 position[3];
};
layout(location = 0) rayPayloadEXT RayPayload rayPayload;

layout(location = 0) hitObjectAttributeNV vec3 bary;

layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer ShadingAttrib {
    uint  tgtSpace;
    uint texCoords;
};

layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer Index {
    uint value;
};

layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer Material {
    uint baseColor_alpha;
};

layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer InstanceData {
    uint geometryOffset;
};

layout(buffer_reference, scalar, buffer_reference_align = 8) readonly buffer GeometryInfo {
    ShadingAttrib shadingAttribs;
    Index                indices;
    Material            material;
};

layout(push_constant, scalar) uniform pc_ {
    uint64_t           tlas;
    GeometryInfo geometries;
    InstanceData  instances;
    uint             target;
    vec3                pos;
    vec3               camX;
    vec3               camY;
    float        camZOffset;
    uint      linearSampler;
} pc;

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