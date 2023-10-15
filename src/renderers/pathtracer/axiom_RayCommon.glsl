#include "axiom_Common.glsl"
#include "src/scene/runtime/axiom_Attributes.glsl"

#extension GL_EXT_ray_tracing                            : require
#extension GL_EXT_ray_tracing_position_fetch             : require
#extension GL_NV_shader_invocation_reorder               : require

layout(set = 0, binding = 0) uniform image2D RWImage2D[];

layout(set = 0, binding = 0) uniform texture2D Image2D[];
layout(set = 0, binding = 0) uniform sampler Sampler[];

struct RayPayload {
    vec3 position[3];
};

layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer ShadingAttributes {
    axiom_TangentSpace tangentSpace;
    axiom_TexCoords       texCoords;
};

layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer Index {
    uint value;
};

layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer Material {
    uint     baseColor_alpha;
    uint             normals;
    uint          emissivity;
    uint        transmission;
    uint metalness_roughness;

    float  alphaCutoff;
    uint8_t  alphaMask;
    uint8_t alphaBlend;
    uint8_t       thin;
    uint8_t subsurface;
};

layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer InstanceData {
    uint geometryOffset;
};

layout(buffer_reference, scalar, buffer_reference_align = 8) readonly buffer GeometryInfo {
    ShadingAttributes shadingAttributes;
    Index                       indices;
    Material                   material;
};

layout(push_constant, scalar) uniform pc_ {
    uint64_t           tlas;
    GeometryInfo geometries;
    InstanceData  instances;
    Index         noiseSeed;
    uint             target;
    vec3                pos;
    vec3               camX;
    vec3               camY;
    float        camZOffset;
    uint      linearSampler;
    uint        sampleCount;
    vec2             jitter;
    uint       sampleRadius;
} pc;