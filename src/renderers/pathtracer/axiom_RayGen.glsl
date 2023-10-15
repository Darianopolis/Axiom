#version 460
#extension GL_GOOGLE_include_directive : require

#include "axiom_RayCommon.glsl"
#include "axiom_Atmosphere.glsl"
#include "axiom_CookTorrance.glsl"

layout(location = 0) rayPayloadEXT        RayPayload rayPayload;
layout(location = 1) rayPayloadEXT        uint shadowRayPayload;
layout(location = 0) hitObjectAttributeNV vec2             bary;

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

uvec2 rnd;

float RandomUNorm()
{
    const uint64_t A = 4294883355;
    uint x = rnd.x, c = rnd.y;
    uint res = x ^ c;
    uint64_t next = x * A + c;
    rnd.x = uint(next & 4294967295);
    rnd.y = uint(next >> 32);

    return 2.3283064365387e-10 * res;
}

bool IsInfZeroOrNan(vec3 V)
{
    float t = V.x + V.y + V.z;
    return t == 0 || isnan(t) || isinf(t);
}

vec3 GetTangent(vec3 N)
{
    float s = sign(N.z);
    float a = -1.0 / (s + N.z);
    float b = N.x * N.y * a;

    return vec3(1 + s * sqr(N.x) * a, s * b, -s * N.x);
}

vec3 ChangeBasis(vec3 v, vec3 N)
{
    float s = sign(N.z);
    float a = -1.0 / (s + N.z);
    float b = N.x * N.y * a;

    vec3 T = vec3(1 + s * sqr(N.x) * a, s * b, -s * N.x);
    vec3 B = vec3(b, s + sqr(N.y) * a, -N.y);

    return normalize(
          (v.x * T)
        + (v.y * B)
        + (v.z * N));
}

vec3 RandomOnCone(vec3 dir, float cosThetaMax)
{
    float u0 = RandomUNorm();
    float cosTheta = (1 - u0) + u0 * cosThetaMax;
    float sinTheta = sqrt(1 - cosTheta * cosTheta);
    float phi = 2 * PI * RandomUNorm();

    vec3 v1 = vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
    return ChangeBasis(v1, dir);
}

vec2 ConcentricSampleDisk()
{
    vec2 uOffset = 2 * vec2(RandomUNorm(), RandomUNorm()) - vec2(1, 1);

    if (uOffset.x == 0 && uOffset.y == 0)
        return vec2(0, 0);

    float theta, r;
    if (abs(uOffset.x) > abs(uOffset.y)) {
        r = uOffset.x;
        theta = PI/4 * (uOffset.y / uOffset.x);
    } else {
        r = uOffset.y;
        theta = PI/2 - (PI/4 * uOffset.x / uOffset.y);
    }

    return r * vec2(cos(theta), sin(theta));
}

vec3 CosineSampleHemisphere()
{
    vec2 d = ConcentricSampleDisk();
    float z = sqrt(max(0, 1 - d.x * d.x - d.y * d.y));
    return vec3(d.x, d.y, z);
}

float CosineSampleHemispherePDF(float cosTheta)
{
    return cosTheta / PI;
}

vec3 VCosineSampleHemisphere(float alpha)
{
    float cosTheta = pow(RandomUNorm(), 1 / (alpha + 1));
    float sinTheta = sqrt(1 - cosTheta * cosTheta);
    float phi = 2 * PI * RandomUNorm();
    return vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

float VCosineSampleHemispherePDF(vec3 v, float alpha)
{
    float cosTheta = v.z;
    return (cosTheta + alpha) * pow(cosTheta, alpha) / PI;
}

void main()
{
    {
        // uint sx = uint(gl_LaunchIDEXT.x);
        // uint sy = gl_LaunchSizeEXT.x + 4 + uint(gl_LaunchIDEXT.y);
        // rnd.x = pc.noiseSeed[sx + 0].value ^ pc.noiseSeed[sy + 1].value;
        // rnd.y = pc.noiseSeed[sx + 3].value ^ pc.noiseSeed[sy + 2].value;

        uint sx = uint(gl_LaunchIDEXT.x) * 2;
        uint sy = (gl_LaunchSizeEXT.x + uint(gl_LaunchIDEXT.y)) * 2;
        rnd.x = pc.noiseSeed[sx + 0].value + pc.noiseSeed[sy + 1].value;
        rnd.y = pc.noiseSeed[sx + 1].value + pc.noiseSeed[sy + 0].value;
    }

    // Write out location

    const uint PixelSize = pc.sampleRadius;
    const uint PixelArea = PixelSize * PixelSize;

    uint ox = (pc.sampleCount % PixelArea) / PixelSize;
    uint oy = (pc.sampleCount % PixelSize);
    ivec2 imgPosBase = ivec2(gl_LaunchIDEXT.xy) * ivec2(PixelSize) + ivec2(ox, oy);

    // Pixel

    vec2 pixelCenter = imgPosBase;
    pixelCenter += pc.jitter * vec2(PixelSize);
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

    vec3 color      = vec3(0.0);
    vec3 throughput = vec3(1.0);
    uint maxDepth   = 10;

    const vec3  SunDir       = normalize(vec3(2, 4, 1));
    // const vec3  SunDir       = normalize(vec3(-1, 1, -1));
    const float SunIntensity = 25.0;
    const float SkyIntensity = 50.0;
    const float SunCosTheta  = cos(radians(0.54) * 0.5);

    for (uint i = 0; i < maxDepth; ++i) {

        hitObjectNV hit;

        // Stochastically kill rays

        bool killRay = false;
        if (i > 0) {
            float lum = LuminanceRGB(throughput);
            if (lum < 0.001)
                killRay = true;

            if (i > 4) {
                float q = max(0.05, 1 - lum);
                if (RandomUNorm() < q)
                    killRay = true;
                throughput /= 1 - q;
            }
        }

        // Trace ray with kill mask

        hitObjectTraceRayNV(hit,
            accelerationStructureEXT(pc.tlas),
            0,                         // Flags
            0xFF * (1 - int(killRay)), // Hit Mask
            0,                         // sbtOffset
            1,                         // sbtStride
            0,                         // missOffset
            origin,                    // rayOrigin
            0.0,                       // tMin
            dir,                       // rayDir
            8000000,                   // tMax
            0);                        // payload

        // Reorder and kill

        if (i > 0) {
            uint hint = 0;
            const uint CH_NumBits = 1;
            const uint CH_KillRay = 1;

            hint |= CH_NumBits * int(killRay);

            reorderThreadNV(hit, hint, CH_NumBits);

            if (killRay) {
                break;
            }
        }

        if (!hitObjectIsHitNV(hit)) {

            // Basic atmosphere
            color += throughput * atmosphere(
                normalize(dir),                 // ray dir
                vec3(0, 6372e3, 0),             // ray origin
                SunDir,
                SkyIntensity,
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
            ShadingAttributes sa0 = geometry.shadingAttributes[i0];
            ShadingAttributes sa1 = geometry.shadingAttributes[i1];
            ShadingAttributes sa2 = geometry.shadingAttributes[i2];

            // Normals + Tangents
            vec3 nrm0, nrm1, nrm2, tgt0, tgt1, tgt2;
            axiom_UnpackTangentSpace(sa0.tangentSpace, nrm0, tgt0);
            axiom_UnpackTangentSpace(sa1.tangentSpace, nrm1, tgt1);
            axiom_UnpackTangentSpace(sa2.tangentSpace, nrm2, tgt2);
            vec3 vertNrm = normalize(tgtSpaceToWorld * (nrm0 * w.x + nrm1 * w.y + nrm2 * w.z));
            vec3 tangent = normalize(tgtSpaceToWorld * (tgt0 * w.x + tgt1 * w.y + tgt2 * w.z));

            // Tex Coords
            vec2 uv0 = axiom_UnpackTexCoords(sa0.texCoords);
            vec2 uv1 = axiom_UnpackTexCoords(sa1.texCoords);
            vec2 uv2 = axiom_UnpackTexCoords(sa2.texCoords);
            vec2 uv = uv0 * w.x + uv1 * w.y + uv2 * w.z;

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
            vec3 flatTgt;
            {
                vec3 v12 = v01;
                vec3 v13 = v02;
                vec2 u12 = uv1 - uv0;
                vec2 u13 = uv2 - uv0;
                float f = 1.f / (u12.x * u13.y - u13.x * u12.y);
                flatTgt = normalize(f * vec3(
                    u13.y * v12.x - u12.y * v13.x,
                    u13.y * v12.y - u12.y * v13.y,
                    u13.y * v12.z - u12.y * v13.z));
            }
            if (IsInfZeroOrNan(flatTgt)) {
                flatTgt = GetTangent(flatNrm);
            }

            // Side corrected normals
            if (hitKind != gl_HitKindFrontFacingTriangleEXT) {
                vertNrm = -vertNrm;
                flatNrm = -flatNrm;
            }

            // Tangent Space
            tangent = normalize(tangent - dot(tangent, vertNrm) * vertNrm);
            vec3 bitangent = normalize(cross(tangent, vertNrm)
                * axiom_UnpackBitangentSign(sa0.tangentSpace));
            mat3 TBN = mat3(tangent, bitangent, vertNrm);
            // flatTgt = normalize(flatTgt - dot(flatTgt, flatNrm) * flatNrm);
            // vec3 bitangent = normalize(cross(flatTgt, flatNrm));
            // mat3 TBN = mat3(flatTgt, bitangent, flatNrm);

            // Texture
            vec4 baseColor_alpha = texture(sampler2D(Image2D[nonuniformEXT(geometry.material.baseColor_alpha)], Sampler[pc.linearSampler]), uv);
            vec3 baseColor = Apply_sRGB_EOTF(baseColor_alpha.rgb);

            // Metalness Roughness
            vec2 metalness_roughness = texture(sampler2D(Image2D[nonuniformEXT(geometry.material.metalness_roughness)], Sampler[pc.linearSampler]), uv).bg;
            float metalness = metalness_roughness.x;
            float roughness = metalness_roughness.y;

            // Emissivity
            vec3 emissivity = texture(sampler2D(Image2D[nonuniformEXT(geometry.material.emissivity)], Sampler[pc.linearSampler]), uv).rgb;
            emissivity *= 20;

            // Normal mapping
            vec3 nrm = texture(sampler2D(Image2D[nonuniformEXT(geometry.material.normals)], Sampler[pc.linearSampler]), uv).xyz;
            nrm = DecodeNormalMap(nrm);
            nrm = normalize(TBN * nrm);

// -----------------------------------------------------------------------------
//                          Debug writeout - Begin
// -----------------------------------------------------------------------------
// #define DEBUG_UV
// #define DEBUG_FLAT_NRM
// #define DEBUG_FLAT_TGT
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
#elif defined(DEBUG_FLAT_TGT)
            color = DebugSNorm(flatTgt);
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

            if (roughness > 0.0) {
                if (roughness > 0.0) {
                    // BRDF

                    if (RandomUNorm() > 0.5)
                    {
                        vec3 sampleDir = RandomOnCone(SunDir, SunCosTheta);
                        if (IsUnobstructed(OffsetPointByNormal(pos, flatNrm), sampleDir, 8000000.0)) {
                            color += throughput * SunIntensity *
                                CookTorranceBrdf(nrm, -dir, sampleDir, baseColor, roughness, metalness, 1.5, true);
                        }

                        break;
                    }
                    else
                    {
                        // bool flip = RandomUNorm() > 0.5;
                        // if (flip) {
                        //     nrm = -nrm;
                        //     flatNrm = -flatNrm;
                        // }

                        vec3 L = CosineSampleHemisphere();
                        float pdf = CosineSampleHemispherePDF(L.z);
                        L = ChangeBasis(L, nrm);
                        pdf = clamp(pdf, 0.01, 10000);
                        roughness = max(0.04, roughness);

                        vec3 brdf = 2.0 * CookTorranceBrdf(nrm, -dir, L, baseColor, roughness, metalness, 1.5, true);
                        throughput *= brdf * max(dot(nrm, L), 0.0) / pdf;
                        dir = L;
                        origin = OffsetPointByNormal(pos, flatNrm);
                    }
                }
                else {
                    origin = OffsetPointByNormal(pos, flatNrm);
                    dir = reflect(dir, nrm);
                    throughput *= baseColor;
                }
            } else {
                origin = OffsetPointByNormal(pos, flatNrm);
                dir = reflect(dir, nrm);
                throughput *= baseColor;
            }

        }
    }

    // Write out

    for (int dx = 0; dx < PixelSize; dx++) {
        for (int dy = 0; dy < PixelSize; dy++) {
            ivec2 imgPos = imgPosBase + ivec2(dx, dy);

            vec4 prev = imageLoad(RWImage2D[pc.target], imgPos);
            vec4 new = vec4(color, 1);

            float oldWeight = float(pc.sampleCount) / float(pc.sampleCount + 1);
            float newWeight = 1 - oldWeight;

            // Check for oldWeight = 0 to avoid inf/nan propagating over reset
            vec4 updated = (oldWeight == 0)
                ? new
                : (prev * oldWeight) + (new * newWeight);

            imageStore(RWImage2D[pc.target], imgPos, updated);
        }
    }
}