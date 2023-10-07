#version 460
#extension GL_GOOGLE_include_directive : require

#include "axiom_RayCommon.glsl"

layout(location = 0) rayPayloadEXT RayPayload rayPayload;

layout(location = 0) hitObjectAttributeNV vec3 bary;

void main()
{
    vec2 pixelCenter = vec2(gl_LaunchIDEXT.xy);
    pixelCenter += vec2(0.5);
    vec2 inUV = pixelCenter / vec2(gl_LaunchSizeEXT.xy);
    vec2 d = inUV * 2.0 - 1.0;
    vec3 focalPoint = pc.camZOffset * cross(pc.camX, pc.camY);

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

    hitObjectNV hit;
    hitObjectTraceRayNV(hit,
        accelerationStructureEXT(pc.tlas),
        0,       // Flags
        0xFF,    // Hit Mask
        0,       // sbtOffset
        1,       // sbtStride
        0,       // missOffset
        pc.pos,  // rayOrigin
        0.0,     // tMin
        dir,     // rayDir
        8000000, // tMax
        0);      // payload

    // TODO: Only reorder on bounces
    reorderThreadNV(0, 0);

    vec3 color = vec3(0.1);
    if (hitObjectIsHitNV(hit)) {

        // Hit Attributes
        int instanceID = hitObjectGetInstanceIdNV(hit);
        int customInstanceID = hitObjectGetInstanceCustomIndexNV(hit);
        int geometryIndex = hitObjectGetGeometryIndexNV(hit);
        uint sbtIndex = hitObjectGetShaderBindingTableRecordIndexNV(hit);
        uint hitKind = hitObjectGetHitKindNV(hit);
        // GeometryInfo geometry = pc.geometries[sbtIndex];
        // GeometryInfo geometry = pc.geometries[pc.instances[instanceID].geometryOffset + geometryIndex];
        GeometryInfo geometry = pc.geometries[customInstanceID + geometryIndex];

        // Transforms
        mat4x3 objToWorld = hitObjectGetObjectToWorldNV(hit);
        mat3x3 tgtSpaceToWorld = transpose(inverse(mat3(objToWorld)));

        // Barycentric weights
        hitObjectGetAttributesNV(hit, 0);
        vec3 w = vec3(1.0 - bary.x - bary.y, bary.x, bary.y);

        // Indices
        uint primID = hitObjectGetPrimitiveIndexNV(hit);
        uint i0 = geometry.indices[primID * 3 + 0].value;
        uint i1 = geometry.indices[primID * 3 + 1].value;
        uint i2 = geometry.indices[primID * 3 + 2].value;

        // Shading attributes
        ShadingAttrib sa0 = geometry.shadingAttribs[i0];
        ShadingAttrib sa1 = geometry.shadingAttribs[i1];
        ShadingAttrib sa2 = geometry.shadingAttribs[i2];

        // Normals
        vec3 nrm0 = SignedOctDecode(sa0.tgtSpace);
        vec3 nrm1 = SignedOctDecode(sa1.tgtSpace);
        vec3 nrm2 = SignedOctDecode(sa2.tgtSpace);
        vec3 vertNrm = nrm0 * w.x + nrm1 * w.y + nrm2 * w.z;
        vertNrm = normalize(tgtSpaceToWorld * vertNrm);

        // Tangents
        vec3 tgt0 = DecodeTangent(nrm0, sa0.tgtSpace);
        vec3 tgt1 = DecodeTangent(nrm1, sa1.tgtSpace);
        vec3 tgt2 = DecodeTangent(nrm2, sa2.tgtSpace);
        vec3 tangent = tgt0 * w.x + tgt1 * w.y + tgt2 * w.z;
        tangent = normalize(tgtSpaceToWorld * tangent);

        // Tangent space
        tangent = normalize(tangent - dot(tangent, vertNrm) * vertNrm);
        vec3 bitangent = normalize(cross(tangent, vertNrm));
        mat3 TBN = mat3(tangent, bitangent, vertNrm);

        // Tex Coords
        vec2 uv0 = unpackHalf2x16(sa0.texCoords);
        vec2 uv1 = unpackHalf2x16(sa1.texCoords);
        vec2 uv2 = unpackHalf2x16(sa2.texCoords);
        vec2 uv = uv0 * w.x + uv1 * w.y + uv2 * w.z;

        // Positions (local)
        hitObjectExecuteShaderNV(hit, 0);
        vec3 v0 = rayPayload.position[0];
        vec3 v1 = rayPayload.position[1];
        vec3 v2 = rayPayload.position[2];

        // Positions (transformed)
        vec3 v0w = objToWorld * vec4(v0, 1);
        vec3 v1w = objToWorld * vec4(v1, 1);
        vec3 v2w = objToWorld * vec4(v2, 1);
        vec3 pos = v0w * w.x + v1w * w.y + v2w * w.z;

        // Flat normal
        vec3 v01 = v1w - v0w;
        vec3 v02 = v2w - v0w;
        vec3 flatNrm = normalize(cross(v01, v02));

        // Side corrected normals
        if (hitKind != gl_HitKindFrontFacingTriangleEXT) {
            vertNrm = -vertNrm;
            flatNrm = -flatNrm;
        }

        // Texture
        vec4 baseColor_alpha = texture(sampler2D(Image2D[nonuniformEXT(geometry.material.baseColor_alpha)], Sampler[pc.linearSampler]), uv);
        baseColor_alpha.rgb = Apply_sRGB_EOTF(baseColor_alpha.rgb);

        // Normal mapping
        vec3 nrm = texture(sampler2D(Image2D[nonuniformEXT(geometry.material.normals)], Sampler[pc.linearSampler]), uv).xyz;
        nrm = DecodeNormalMap(nrm);
        nrm = normalize(TBN * nrm);

// -----------------------------------------------------------------------------
// #define DEBUG_UV
// #define DEBUG_FLAT_NRM
// #define DEBUG_VERT_NRM
#define DEBUG_NRM
// #define DEBUG_TGT
// #define DEBUG_BARY
// -----------------------------------------------------------------------------

#if   defined(DEBUG_UV)
        color = vec3(uv, 0);
#elif defined(DEBUG_FLAT_NRM)
        color = DebugSNorm(flatNrm);
#elif defined(DEBUG_VERT_NRM)
        color = DebugSNorm(vertNrm);
#elif defined(DEBUG_NRM)
        color = DebugSNorm(nrm);
#elif defined(DEBUG_TGT)
        color = DebugSNorm(tgt);
#elif defined(DEBUG_BARY)
        color = vec3(1.0 - bary.x - bary.y, bary.x, bary.y);
#else
        color = baseColor_alpha.rgb;

        // sRGB EOTF correction
        color = Apply_sRGB_OETF(color);
#endif
    }

    imageStore(RWImage2D[pc.target], ivec2(gl_LaunchIDEXT.xy), vec4(color, 1));
}