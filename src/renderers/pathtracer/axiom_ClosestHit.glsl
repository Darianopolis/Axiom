#version 460
#extension GL_EXT_ray_tracing                : require
#extension GL_EXT_ray_tracing_position_fetch : require

struct RayPayload {
    vec3 position[3];
};
layout(location = 0) rayPayloadInEXT RayPayload rayPayload;

void main()
{
    rayPayload.position[0] = gl_HitTriangleVertexPositionsEXT[0];
    rayPayload.position[1] = gl_HitTriangleVertexPositionsEXT[1];
    rayPayload.position[2] = gl_HitTriangleVertexPositionsEXT[2];
}