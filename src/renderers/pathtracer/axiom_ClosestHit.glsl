#include "axiom_RayCommon.glsl"

layout(location = 0) rayPayloadInEXT RayPayload rayPayload;

void main()
{
    rayPayload.position[0] = gl_HitTriangleVertexPositionsEXT[0];
    rayPayload.position[1] = gl_HitTriangleVertexPositionsEXT[1];
    rayPayload.position[2] = gl_HitTriangleVertexPositionsEXT[2];
}