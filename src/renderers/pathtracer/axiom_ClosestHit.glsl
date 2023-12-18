#include "axiom_RayCommon.glsl"

layout(location = 0) rayPayloadInEXT RayPayload rayPayload;
hitAttributeEXT vec2 bary;

void main()
{
    rayPayload.position[0] = gl_HitTriangleVertexPositionsEXT[0];
    rayPayload.position[1] = gl_HitTriangleVertexPositionsEXT[1];
    rayPayload.position[2] = gl_HitTriangleVertexPositionsEXT[2];
    rayPayload.bary = bary;

    rayPayload.instanceID = gl_InstanceID;
    rayPayload.customInstanceID = gl_InstanceCustomIndexEXT;
    rayPayload.geometryIndex = gl_GeometryIndexEXT;
    rayPayload.hitKind = gl_HitKindEXT;
    rayPayload.objToWorld = gl_ObjectToWorldEXT;
    rayPayload.primitiveID = gl_PrimitiveID;
    rayPayload.isHit = true;
}