#version 460
#extension GL_GOOGLE_include_directive : require

#include "axiom_RayCommon.glsl"
#include "axiom_Atmosphere.glsl"
#include "axiom_CookTorrance.glsl"

layout(location = 0) rayPayloadEXT        RayPayload rayPayload;
layout(location = 1) rayPayloadEXT        uint shadowRayPayload;
layout(location = 0) hitObjectAttributeNV vec3             bary;

bool IsUnobstructed(vec3 origin, vec3 dir, float tMax)
{
    uint rayFlags = 0;
    hitObjectNV hit;
    hitObjectTraceRayNV(hit,
        accelerationStructureEXT(pc.tlas),
        rayFlags,
        0xFF,
        0,
        1,
        0,
        origin,
        0.0,
        dir,
        tMax,
        1);
    return !hitObjectIsHitNV(hit);
}

void main()
{
    vec2 pixelCenter = vec2(gl_LaunchIDEXT.xy);
    pixelCenter += pc.jitter;
    vec2 inUV = pixelCenter / vec2(gl_LaunchSizeEXT.xy);
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

    vec3 color      = vec3(0.0);
    vec3 throughput = vec3(1.0);
    uint maxDepth   = 1;

    const vec3  SunDir       = normalize(vec3(2, 4, 1));
    // const vec3  SunDir       = normalize(vec3(-1, 1, -1));
    const float SunIntensity = 22.0;

    for (uint i = 0; i < maxDepth; ++i) {

        hitObjectNV hit;
        hitObjectTraceRayNV(hit,
            accelerationStructureEXT(pc.tlas),
            0,       // Flags
            0xFF,    // Hit Mask
            0,       // sbtOffset
            1,       // sbtStride
            0,       // missOffset
            origin,  // rayOrigin
            0.0,     // tMin
            dir,     // rayDir
            8000000, // tMax
            0);      // payload

        // TODO: Only reorder on bounces
        if (i > 0) {
            reorderThreadNV(hit);
        }

        if (!hitObjectIsHitNV(hit)) {

            // Basic atmosphere
            color += throughput * atmosphere(
                normalize(dir),                 // ray dir
                vec3(0, 6372e3, 0),             // ray origin
                SunDir,
                SunIntensity,
                6371e3,                         // radius of the planet in meters
                6471e3,                         // radius of the atmosphere in meters
                vec3(5.5e-6, 13.0e-6, 22.4e-6), // Rayleigh scattering coefficient
                21e-6,                          // Mie scattering coefficient
                8e3,                            // Rayleigh scale height
                1.2e3,                          // Mie scale height
                0.758);                         // Mie preferred scattering direction

            break;

        } else {

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
            float tgtSpaceSign = float(bitfieldExtract(sa0.tgtSpace, 31, 1)) * 2.0 - 1.0;
            vec3 bitangent = normalize(cross(tangent, vertNrm) * tgtSpaceSign);
            mat3 TBN = mat3(tangent, bitangent, vertNrm);

            // Tex Coords
            vec2 uv0 = unpackHalf2x16(sa0.texCoords);
            vec2 uv1 = unpackHalf2x16(sa1.texCoords);
            vec2 uv2 = unpackHalf2x16(sa2.texCoords);
            vec2 uv = uv0 * w.x + uv1 * w.y + uv2 * w.z;

            // Positions
            hitObjectExecuteShaderNV(hit, 0);
            vec3 v0w = objToWorld * vec4(rayPayload.position[0], 1);
            vec3 v1w = objToWorld * vec4(rayPayload.position[1], 1);
            vec3 v2w = objToWorld * vec4(rayPayload.position[2], 1);
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
            vec3 baseColor = Apply_sRGB_EOTF(baseColor_alpha.rgb);

            // Metalness Roughness
            vec2 metalness_roughness = texture(sampler2D(Image2D[nonuniformEXT(geometry.material.metalness_roughness)], Sampler[pc.linearSampler]), uv).bg;
            float metalness = metalness_roughness.x;
            float roughness = metalness_roughness.y;

            // Emissivity
            vec3 emissivity = texture(sampler2D(Image2D[nonuniformEXT(geometry.material.emissivity)], Sampler[pc.linearSampler]), uv).rgb;

            // Normal mapping
            vec3 nrm = texture(sampler2D(Image2D[nonuniformEXT(geometry.material.normals)], Sampler[pc.linearSampler]), uv).xyz;
            nrm = DecodeNormalMap(nrm);
            nrm = normalize(TBN * nrm);

// -----------------------------------------------------------------------------
//                          Debug writeout - Begin
// -----------------------------------------------------------------------------
// #define DEBUG_UV
// #define DEBUG_FLAT_NRM
// #define DEBUG_VERT_NRM
// #define DEBUG_NRM
// #define DEBUG_TGT
// #define DEBUG_BARY
// #define DEBUG_BASE
// #define DEBUG_MRAO
// #define DEBUG_EMIS
// -----------------------------------------------------------------------------
#if   defined(DEBUG_UV)
            color = vec3(mod(uv, 1.0), 0);
#elif defined(DEBUG_FLAT_NRM)
            color = DebugSNorm(flatNrm);
#elif defined(DEBUG_VERT_NRM)
            color = DebugSNorm(vertNrm);
#elif defined(DEBUG_NRM)
            color = DebugSNorm(nrm);
#elif defined(DEBUG_TGT)
            color = DebugSNorm(tangent);
#elif defined(DEBUG_BARY)
            color = vec3(1.0 - bary.x - bary.y, bary.x, bary.y);
#elif defined(DEBUG_BASE)
            color = baseColor_alpha.rgb;
#elif defined(DEBUG_MRAO)
            color = vec3(0, roughness, metalness);
#elif defined(DEBUG_EMIS)
            color = emissivity;
#else
            if (false)
#endif
            {
                color = Apply_sRGB_EOTF(color);
                break;
            }
// -----------------------------------------------------------------------------
//                         Debug writeout - End
// -----------------------------------------------------------------------------

            // Surface interaction

            // Emissive term
            color += throughput * emissivity;

            // Ambient term
            color += throughput * baseColor * 0.1;

            // BRDF
            if (IsUnobstructed(OffsetPointByNormal(pos, flatNrm), SunDir, 8000000.0)) {
                color += throughput *
                    CookTorranceBrdf(nrm, -dir, SunDir, baseColor, roughness, metalness, 1.5, true);
            }
        }
    }

    vec4 prev = imageLoad(RWImage2D[pc.target], ivec2(gl_LaunchIDEXT.xy));
    vec4 new = vec4(color, 1);

    float oldWeight = float(pc.sampleCount) / float(pc.sampleCount + 1);
    float newWeight = 1 - oldWeight;

    // Check for oldWeight = 0 to avoid inf/nan propagating over reset
    vec4 updated = (oldWeight == 0)
        ? new
        : (prev * oldWeight) + (new * newWeight);

    imageStore(RWImage2D[pc.target], ivec2(gl_LaunchIDEXT.xy), updated);
}