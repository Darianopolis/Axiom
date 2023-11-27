#include "axiom_RayCommon.glsl"

layout(location = 0) rayPayloadEXT        RayPayload rayPayload;
layout(location = 0) hitObjectAttributeNV vec2             bary;

void main()
{

    // Write out location

    const uint PixelSize = pc.sampleRadius;
    const uint PixelArea = PixelSize * PixelSize;

    uint ox = (pc.sampleCount % PixelArea) / PixelSize;
    uint oy = (pc.sampleCount % PixelSize);
    ivec2 imgPosBase = ivec2(gl_LaunchIDEXT.xy) * ivec2(PixelSize) + ivec2(ox, oy);

    // Pixel

    vec2 pixelCenter = imgPosBase;
    pixelCenter += vec2(0.5) * vec2(PixelSize);
    vec2 inUV = pixelCenter / (vec2(gl_LaunchSizeEXT.xy) * vec2(PixelSize));
    vec2 d = inUV * 2.0 - 1.0;
    vec3 focalPoint = pc.camZOffset * cross(pc.camX, pc.camY);
    vec3 origin = pc.pos;

    // Perspective
    d.x *= float(gl_LaunchSizeEXT.x) / float(gl_LaunchSizeEXT.y);
    d.y *= -1.0;
    vec3 dir = normalize((pc.camY * d.y) + (pc.camX * d.x) - focalPoint);

    // // Equirectangular
    // vec2 uv = d;
    // float yaw = uv.x * PI;
    // float pitch = uv.y * PI * 0.5f;
    // float x = sin(yaw) * cos(pitch);
    // float y = -sin(pitch);
    // float z = -cos(yaw) * cos(pitch);
    // mat3 tbn = mat3(pc.camX, pc.camY, focalPoint);
    // vec3 dir = tbn * vec3(x, y, z);

    // // Fisheye
    // float fov = radians(180);
    // float aspect = float(gl_LaunchSizeEXT.x) / float(gl_LaunchSizeEXT.y);
    // vec2 uv = d;
    // vec2 xy = uv * vec2(1, -aspect);
    // float r = sqrt(dot(xy, xy));
    // vec2 cs = vec2 (cos(r * fov), sin(r * fov));
    // mat3 tbn = mat3(pc.camX, pc.camY, -focalPoint);
    // vec3 dir = tbn * vec3 (cs.y * xy / r, cs.x);

    vec3 color = vec3(0.0);

    hitObjectNV hit;

    hitObjectTraceRayNV(hit,
        accelerationStructureEXT(pc.tlas),
        0,                         // Flags
        0xFF,                      // Hit Mask
        0,                         // sbtOffset
        1,                         // sbtStride
        0,                         // missOffset
        origin,                    // rayOrigin
        0.0,                       // tMin
        dir,                       // rayDir
        8000000,                   // tMax
        0);                        // payload

    if (hitObjectIsHitNV(hit)) {

        // Hit Attributes
        int instanceID = hitObjectGetInstanceIdNV(hit);
        int customInstanceID = hitObjectGetInstanceCustomIndexNV(hit);
        int geometryIndex = hitObjectGetGeometryIndexNV(hit);
        uint sbtIndex = hitObjectGetShaderBindingTableRecordIndexNV(hit);
        uint hitKind = hitObjectGetHitKindNV(hit);
        GeometryInfo geometry = pc.geometries[customInstanceID + geometryIndex];

        // Transforms
        mat4x3 objToWorld = hitObjectGetObjectToWorldNV(hit);

        // Barycentric weights
        hitObjectGetAttributesNV(hit, 0);
        vec3 w = vec3(1.0 - bary.x - bary.y, bary.x, bary.y);

        // Indices
        uint primID = hitObjectGetPrimitiveIndexNV(hit);
        uint i0 = geometry.indices[primID * 3 + 0].value;
        uint i1 = geometry.indices[primID * 3 + 1].value;
        uint i2 = geometry.indices[primID * 3 + 2].value;

        // Positions
        hitObjectExecuteShaderNV(hit, 0);
        vec3 v0w = objToWorld * vec4(rayPayload.position[0], 1);
        vec3 v1w = objToWorld * vec4(rayPayload.position[1], 1);
        vec3 v2w = objToWorld * vec4(rayPayload.position[2], 1);
        vec3 pos = v0w * w.x + v1w * w.y + v2w * w.z;

        // Flat Normals + Tangents
        vec3 v01 = v1w - v0w;
        vec3 v02 = v2w - v0w;
        vec3 flatNrm = normalize(cross(v01, v02));

        color = DebugSNorm(flatNrm);
    }

    imageStore(RWImage2D[pc.target], ivec2(gl_LaunchIDEXT.xy), vec4(color, 1));
}