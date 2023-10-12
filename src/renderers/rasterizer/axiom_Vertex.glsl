#version 460
#extension GL_EXT_scalar_block_layout  : require
#extension GL_EXT_buffer_reference2    : require
#extension GL_EXT_nonuniform_qualifier : require

layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer PosAttrib {
    vec3 position;
};

layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer ShadingAttributes {
    axiom_TangentSpace tangentSpace;
    axiom_TexCoords       texCoords;
};

layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer Instance {
    mat4 transform;
};

layout(push_constant, scalar) readonly uniform pc_ {
    PosAttrib                posAttribs;
    ShadingAttributes shadingAttributes;
    Instance                  instances;
    mat4                       viewProj;
} pc;

layout(location = 0) out vec3 outPosition;

void main()
{
    PosAttrib p = pc.posAttribs[gl_VertexIndex];
    Instance instance = pc.instances[gl_InstanceIndex];

    vec4 worldPos = instance.transform * vec4(p.position, 1);
    outPosition = vec3(worldPos);
    gl_Position = pc.viewProj * worldPos;
}