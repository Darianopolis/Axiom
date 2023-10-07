#version 460
#extension GL_GOOGLE_include_directive : require

#include "axiom_RayCommon.glsl"

hitAttributeEXT vec3 bary;

void main()
{
    // Hit attributes
    GeometryInfo geometry = pc.geometries[gl_InstanceCustomIndexEXT + gl_GeometryIndexEXT];

    // Barycentric weights
    vec3 w = vec3(1.0 - bary.x - bary.y, bary.x, bary.y);

    // Indices
    uint i0 = geometry.indices[gl_PrimitiveID * 3 + 0].value;
    uint i1 = geometry.indices[gl_PrimitiveID * 3 + 1].value;
    uint i2 = geometry.indices[gl_PrimitiveID * 3 + 2].value;

    // Shading attributes
    ShadingAttrib sa0 = geometry.shadingAttribs[i0];
    ShadingAttrib sa1 = geometry.shadingAttribs[i1];
    ShadingAttrib sa2 = geometry.shadingAttribs[i2];

    // Tex Coords
    vec2 uv0 = unpackHalf2x16(sa0.texCoords);
    vec2 uv1 = unpackHalf2x16(sa1.texCoords);
    vec2 uv2 = unpackHalf2x16(sa2.texCoords);
    vec2 uv = uv0 * w.x + uv1 * w.y + uv2 * w.z;

    // Texture
    vec4 baseColor_alpha = texture(sampler2D(Image2D[geometry.material.baseColor_alpha], Sampler[pc.linearSampler]), uv);

    if (baseColor_alpha.a < geometry.material.alphaCutoff) {
        ignoreIntersectionEXT;
    }
}