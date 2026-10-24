// Bindings and helpers shared by the global illumination's kernels (docs/DEVELOPMENT.md, "Global
// illumination"). The maths helpers are AMD's, from Capsaicin's GI-1.2 (MIT; licenses/Capsaicin-LICENSE.txt).
#ifndef GI_COMMON_HLSLI
#define GI_COMMON_HLSLI

#include "gi_shared.h"

// ---- Bindings: one root signature for every kernel (gi_renderer.cpp) ----

ConstantBuffer<GIFrameConstants> g_Frame : register(b0);
cbuffer DispatchConstants : register(b1)
{
    uint4 g_Dispatch; // Per dispatch: what each kernel says it is
};

RaytracingAccelerationStructure g_Scene : register(t0, space1);

// The frame's inputs, packed by Direct3D 11 from the game's G-buffer.
Texture2D<float> g_Depth : register(t0);    // Reversed-Z, 0 where the sky is
Texture2D<float4> g_Normal : register(t1);  // World space as 0.5 n + 0.5; 0 where the sky is
Texture2D<float4> g_Albedo : register(t2);  // Linear
StructuredBuffer<uint> g_NewGeometries : register(t4); // The frame's new geometries' rows of the table (counts.x)
// The frame's instances as the top level holds them, and the scene's geometry (gi_scene.hpp): the
// pool, vertices of 16 bytes (xyz the position, object space; w the texture coordinates, two halves,
// with GI_GEOMETRY_UV) and 16-bit indices; the table of geometries, an instance's from its first.
StructuredBuffer<GIInstanceDesc> g_Instances : register(t5);
ByteAddressBuffer g_GeometryPool : register(t6);
StructuredBuffer<GIGeometry> g_Geometries : register(t16);
// The game's textures the scene holds (gi_textures.hpp), each its levels of detail from the first no
// larger than the scene keeps, viewed as sRGB where the game's are (GIGeometry::texture).
Texture2D<float4> g_SceneTextures[GI_MAX_TEXTURES] : register(t0, space2);

RWTexture2D<float4> g_Output : register(u0);
RWByteAddressBuffer g_GeometryPoolOut : register(u1); // The pool, while the frame's terrain and characters' poses go in
RWStructuredBuffer<uint> g_Counters : register(u4);
RWStructuredBuffer<uint4> g_DispatchArgs : register(u5);
RWStructuredBuffer<uint4> g_GeometryAlbedo : register(u6); // Sums of linear albedo x 255 and their count, by geometry

// The game's velocity for its moving meshes (characters, the horse, trees in the wind) as its velocity
// passes drew it this frame, when it came in time (objectMotion.x): R 1 where one drew, B and A the
// screen motion from the last frame to this one in pixels / 64, y down, stored as 0.5 v + 0.5 in 8 bits
// (docs/DEVELOPMENT.md, "Velocity"). It carries the frame's jitter, as the camera's motion does here.
Texture2D<float4> g_ObjectVelocity : register(t17);

SamplerState g_PointSampler : register(s0);
SamplerState g_LinearSampler : register(s1);
SamplerState g_WrapSampler : register(s2);

// ---- Constants ----

#define QUARTER_PI  0.78539816339744830961566084581988f
#define HALF_PI     1.5707963267948966192313216916398f
#define PI          3.1415926535897932384626433832795f
#define TWO_PI      6.283185307179586476925286766559f
#define FOUR_PI     12.566370614359172953850573533118f
#define INV_PI      0.31830988618379067153776752674503f
#define TWO_INV_PI  0.63661977236758134307553505349006f
#define FLT_MAX     3.402823466e+38f

// ---- Maths (Capsaicin) ----

float hadd(float2 v) { return v.x + v.y; }
float hmax(float2 v) { return max(v.x, v.y); }
float hmin(float2 v) { return min(v.x, v.y); }
float squared(float v) { return v * v; }

float luminance(float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

uint pcgHash(uint value)
{
    uint state = value * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

uint xxHash(uint value)
{
    const uint prime32_2 = 2246822519u, prime32_3 = 3266489917u;
    const uint prime32_4 = 668265263u, prime32_5 = 374761393u;
    uint ret = value + prime32_5;
    ret = prime32_4 * ((ret << 17) | (ret >> (32 - 17)));
    ret = prime32_2 * (ret ^ (ret >> 15));
    ret = prime32_3 * (ret ^ (ret >> 13));
    return ret ^ (ret >> 16);
}

uint2 packHalf3(float3 value)
{
    return uint2(f32tof16(value.x) | (f32tof16(value.y) << 16), f32tof16(value.z));
}

float3 unpackHalf3(uint2 packed)
{
    return float3(f16tof32(packed.x & 0xFFFFu), f16tof32(packed.x >> 16), f16tof32(packed.y & 0xFFFFu));
}

uint2 packHalf4(float4 value)
{
    return uint2(f32tof16(value.x) | (f32tof16(value.y) << 16), f32tof16(value.z) | (f32tof16(value.w) << 16));
}

float4 unpackHalf4(uint2 packed)
{
    return float4(f16tof32(packed.x & 0xFFFFu), f16tof32(packed.x >> 16), f16tof32(packed.y & 0xFFFFu), f16tof32(packed.y >> 16));
}

uint packNormal(float3 value)
{
    int3 packed = int3(clamp(value, -1.0f, 1.0f) * 511.0f + (0.5f * sign(value))) & 0x3FF;
    return packed.x | (packed.y << 10) | (packed.z << 20);
}

float3 unpackNormal(uint packed)
{
    int3 value = int3(packed << 22, packed << 12, packed << 2) >> 22;
    return float3(value) * (1.0f / 511.0f);
}

// An equal-area map of the unit square to the hemisphere around +z (Clarberg).
float3 mapToHemiOctahedron(float2 samples)
{
    float2 st = 2.0f * samples - 1.0f;
    st = float2(st.x + st.y, st.x - st.y) * 0.5f;

    float2 absMapped = abs(st);
    float distance = 1.0f - hadd(absMapped);
    float radius = 1.0f - abs(distance);

    float phi = (radius == 0.0f) ? 0.0f : QUARTER_PI * ((absMapped.y - absMapped.x) / radius + 1.0f);
    float radiusSqr = radius * radius;
    float sinTheta = radius * sqrt(2.0f - radiusSqr);
    float sinPhi, cosPhi;
    sincos(phi, sinPhi, cosPhi);
    return float3(sinTheta * sign(st.x) * cosPhi, sinTheta * sign(st.y) * sinPhi, sign(distance) * (1.0f - radiusSqr));
}

float2 mapToHemiOctahedronInverse(float3 direction)
{
    float3 absDir = abs(direction);

    float radius = sqrt(1.0f - absDir.z);
    float a = hmax(absDir.xy);
    float b = hmin(absDir.xy);
    b = a == 0.0f ? 0.0f : b / a;

    float phi = atan(b) * TWO_INV_PI;
    phi = (absDir.x >= absDir.y) ? phi : 1.0f - phi;

    float t = phi * radius;
    float s = radius - t;
    float2 st = float2(s, t) * sign(direction).xy;
    st = float2(st.x + st.y, st.x - st.y);
    return 0.5f * st + 0.5f;
}

void GetOrthoVectors(float3 n, out float3 b1, out float3 b2)
{
    bool sel = abs(n.z) > 0;
    float3 p2 = sel ? n : n.zyx;
    float k = 1.0f / sqrt(squared(p2.z) + squared(n.y));
    b1 = float3(0.0f, -p2.z * k, n.y * k);
    b1 = sel ? b1 : b1.zyx;
    b2 = cross(n, b1);
}

float3x3 CreateTBN(float3 n)
{
    float3 u, v;
    GetOrthoVectors(n, u, v);

    float3x3 tbn;
    tbn[0] = u;
    tbn[1] = v;
    tbn[2] = n;
    return transpose(tbn);
}

// Cosine-weighted direction around `normal` (Shirley).
float3 mapToCosineHemisphere(float2 samples, float3 normal)
{
    float3 u, v;
    GetOrthoVectors(normal, u, v);

    float phi = TWO_PI * samples.y;
    float sinPhi, cosPhi;
    sincos(phi, sinPhi, cosPhi);
    float sinTheta = sqrt(samples.x);
    float cosTheta = sqrt(1.0f - samples.x);
    return normalize(u * sinTheta * cosPhi + v * sinTheta * sinPhi + normal * cosTheta);
}

float3 transformPointProjection(float3 position, float4x4 transform)
{
    float4 result = mul(transform, float4(position, 1.0f));
    return result.xyz / result.w;
}

// The world position of a pixel's texture coordinate and depth.
float3 transformPointProjection(float2 uv, float depth, float4x4 transform)
{
    return transformPointProjection(float3(2.0f * float2(uv.x, 1.0f - uv.y) - 1.0f, depth), transform);
}

// Distance along the view of a reversed-Z depth with a finite far plane, as Fox Engine's.
float toLinearDepth(float depth, float2 nearFar)
{
    return (nearFar.x * nearFar.y) / (nearFar.x + depth * (nearFar.y - nearFar.x));
}

float CalculateHaltonNumber(uint index, uint base)
{
    float f = 1.0f, result = 0.0f;
    for (uint i = index; i > 0;)
    {
        f /= base;
        result += f * (i % base);
        i = uint(i / float(base));
    }

    return result;
}

float2 CalculateHaltonSequence(uint index)
{
    return float2(CalculateHaltonNumber((index & 0xFFu) + 1, 2), CalculateHaltonNumber((index & 0xFFu) + 1, 3));
}

// Capsaicin's PCG generator, seeded by hashing in place of its seed buffer.
struct Random
{
    uint state;

    uint randInt()
    {
        uint previous = state;
        state = state * 747796405u + 2891336453u;
        uint word = ((previous >> ((previous >> 28u) + 4u)) ^ previous) * 277803737u;
        return (word >> 22u) ^ word;
    }

    uint randInt(uint maximum)
    {
        return randInt() % maximum;
    }

    float rand()
    {
        return float(randInt() >> 8) * (1.0f / 16777216.0f);
    }

    float2 rand2()
    {
        return float2(rand(), rand());
    }
};

Random MakeRandom(uint seed, uint index)
{
    const uint increment = (index << 1) | 1u;
    Random result;
    result.state = (pcgHash(seed) + increment) * 747796405u + increment;
    return result;
}

// Interleaved gradient noise (Jimenez, "Next Generation Post Processing in Call of Duty: Advanced
// Warfare", SIGGRAPH 2014): a number in [0, 1) per pixel that differs most between neighbours, moving
// on each frame as its temporal form does.
float InterleavedGradientNoise(uint2 pixel, uint frame)
{
    float2 p = float2(pixel) + 5.588238f * float(frame & 63u);
    return frac(52.9829189f * frac(dot(p, float2(0.06711056f, 0.00583715f))));
}

// Point `index` of Roberts' R2 sequence: 2D points that fill the unit square evenly however many are taken.
float2 R2(uint index)
{
    return frac(float(index) * float2(0.7548776662466927f, 0.5698402909980532f));
}

// ---- Spherical harmonics (Capsaicin) ----

void SH_GetCoefficients(float3 direction, out float coefficients[9])
{
    float pz2 = direction.z * direction.z;
    coefficients[0] = 0.2820947917738781f;
    coefficients[2] = 0.4886025119029199f * direction.z;
    coefficients[6] = 0.9461746957575601f * pz2 + -0.3153915652525201f;
    float fC0 = direction.x, fS0 = direction.y;
    float fTmpA = -0.48860251190292f;
    coefficients[3] = fTmpA * fC0;
    coefficients[1] = fTmpA * fS0;
    float fTmpB = -1.092548430592079f * direction.z;
    coefficients[7] = fTmpB * fC0;
    coefficients[5] = fTmpB * fS0;
    float fC1 = direction.x * fC0 - direction.y * fS0;
    float fS1 = direction.x * fS0 + direction.y * fC0;
    float fTmpC = 0.5462742152960395f;
    coefficients[8] = fTmpC * fC1;
    coefficients[4] = fTmpC * fS1;
}

void SH_GetCoefficients_ClampedCosine_Cone(float3 lobe, float coneThetaMax, out float coefficients[9])
{
    float sinThetaMax, cosThetaMax;
    sincos(coneThetaMax, sinThetaMax, cosThetaMax);
    float sinThetaMax2 = sinThetaMax * sinThetaMax;
    float sinThetaMax3 = sinThetaMax2 * sinThetaMax;
    float cosThetaMax3 = cosThetaMax * cosThetaMax * cosThetaMax;

    float band1 = 1.023326707946489f * (1.f - cosThetaMax3);
    float band2 = (4.f - 3.f * sinThetaMax3) * sinThetaMax2;

    coefficients[0] = 0.886226925452758f * sinThetaMax2;
    coefficients[1] = -band1 * lobe.y;
    coefficients[2] = +band1 * lobe.z;
    coefficients[3] = -band1 * lobe.x;
    coefficients[4] = +0.8580855308097834f * band2 * lobe.x * lobe.y;
    coefficients[5] = -0.8580855308097834f * band2 * lobe.y * lobe.z;
    coefficients[6] = +0.2477079561003757f * band2 * (3.f * lobe.z * lobe.z - 1.f);
    coefficients[7] = -0.8580855308097834f * band2 * lobe.x * lobe.z;
    coefficients[8] = +0.4290427654048917f * band2 * (lobe.x * lobe.x - lobe.y * lobe.y);
}

// ---- The game's motion ----

// Whether a moving mesh of the game's covers a render pixel this frame, and its screen motion from the
// last frame to this one, in pixels, y down. The game clamps its velocity to 64 pixels; beyond that only
// the camera's motion is known.
bool ObjectMotion(int2 pixel, out float2 moved)
{
    moved = 0.0f;
    if (g_Frame.objectMotion.x == 0u) return false;

    float4 velocity = g_ObjectVelocity.Load(int3(pixel, 0));
    moved = (velocity.ba * 2.0f - 1.0f) * 64.0f;
    return velocity.r > 0.5f && length(moved) < 63.0f;
}

// ---- The scene ----

// The sky's radiance in a world direction, from the game's own sky light (gi_renderer.cpp).
float3 SkyRadiance(float3 direction)
{
    float coefficients[9];
    SH_GetCoefficients(direction, coefficients);

    float3 radiance = 0.0f;
    for (uint i = 0; i < 9; ++i) radiance += coefficients[i] * g_Frame.skySH[i].xyz;

    return max(radiance, 0.0f);
}

float3 DecodeNormal(float4 encoded)
{
    return normalize(2.0f * encoded.xyz - 1.0f);
}

// Rays leave a surface the depth buffer gave from slightly in front of it: the scene's triangles are
// not exactly where the game drew them (another level of detail, rounding).
float3 OffsetFromScreenSurface(float3 position, float3 normal)
{
    float distanceToEye = distance(position, g_Frame.eye.xyz);
    return position + normal * (0.02f + 0.002f * distanceToEye);
}

// Wächter and Binder's offset for a point on a triangle (Ray Tracing Gems, chapter 6).
float3 OffsetFromTriangle(float3 position, float3 normal)
{
    const float origin = 1.0f / 32.0f;
    const float floatScale = 1.0f / 65536.0f;
    const float intScale = 256.0f;

    int3 intN = int3(normal * intScale);
    float3 positionI = asfloat(asint(position) + select(position < 0, -intN, intN));
    float3 positionF = position + normal * floatScale;
    return select(abs(position) < origin, positionF, positionI);
}

// ---- The scene's geometry ----

// An instance's kind (GI_INSTANCE_*), and its first geometry's row of the table.
uint InstanceKind(GIInstanceDesc instance)
{
    return instance.idAndMask & GI_INSTANCE_KINDS;
}

uint FirstGeometry(GIInstanceDesc instance)
{
    return instance.hitGroupAndFlags & 0xFFFFFFu;
}

// A triangle's corners in its geometry.
uint3 TriangleIndices(GIGeometry geometry, uint primitive)
{
    if (geometry.indexOffset == GI_NO_INDICES) return 3 * primitive + uint3(0, 1, 2);

    // Three 16-bit indices from the four-byte word holding the first.
    uint address = geometry.indexOffset + 6 * primitive;
    uint2 words = g_GeometryPool.Load2(address & ~3u);
    return (address & 2u) ? uint3(words.x >> 16, words.y & 0xFFFFu, words.y >> 16) : uint3(words.x & 0xFFFFu, words.x >> 16, words.y & 0xFFFFu);
}

float4 PoolVertex(GIGeometry geometry, uint index)
{
    return asfloat(g_GeometryPool.Load4(geometry.vertexOffset + 16 * index));
}

float3 ToWorld(GIInstanceDesc instance, float3 position)
{
    float4 p = float4(position, 1.0f);
    return float3(dot(instance.transform[0], p), dot(instance.transform[1], p), dot(instance.transform[2], p));
}

// A triangle of an instance's geometry: its corners in world space, and the texture coordinates
// where the ray crossed it (with GI_GEOMETRY_UV).
struct SceneTriangle
{
    float3 v0, v1, v2;
    GIGeometry geometry;
    uint row; // The geometry's row of the table
    uint3 indices;
};

SceneTriangle LoadTriangle(uint instanceIndex, uint geometryIndex, uint primitive)
{
    GIInstanceDesc instance = g_Instances[instanceIndex];
    SceneTriangle t;
    t.row = FirstGeometry(instance) + geometryIndex;
    t.geometry = g_Geometries[t.row];
    t.indices = TriangleIndices(t.geometry, primitive);
    t.v0 = ToWorld(instance, PoolVertex(t.geometry, t.indices.x).xyz);
    t.v1 = ToWorld(instance, PoolVertex(t.geometry, t.indices.y).xyz);
    t.v2 = ToWorld(instance, PoolVertex(t.geometry, t.indices.z).xyz);
    return t;
}

float2 VertexUV(GIGeometry geometry, uint index)
{
    uint packed = g_GeometryPool.Load(geometry.vertexOffset + 16 * index + 12);
    return f16tof32(uint2(packed, packed >> 16));
}

float2 TriangleUV(GIGeometry geometry, uint3 indices, float2 barycentrics)
{
    return (1.0f - barycentrics.x - barycentrics.y) * VertexUV(geometry, indices.x) + barycentrics.x * VertexUV(geometry, indices.y)
        + barycentrics.y * VertexUV(geometry, indices.z);
}

// The level of detail at which a ray cone `footprint` metres across meets a triangle (Akenine-Moller
// et al., "Texture Level of Detail Strategies for Real-Time Ray Tracing", Ray Tracing Gems chapter 20):
// the texels across the footprint, from the triangle's texels per square metre.
float TextureLod(float3 v0, float3 v1, float3 v2, float2 uv0, float2 uv1, float2 uv2, float2 size, float footprint)
{
    float worldArea = length(cross(v1 - v0, v2 - v0));
    float texelArea = abs((uv1.x - uv0.x) * (uv2.y - uv0.y) - (uv2.x - uv0.x) * (uv1.y - uv0.y)) * size.x * size.y;
    return log2(max(footprint * sqrt(texelArea / max(worldArea, 1e-12f)), 1e-6f));
}

// Whether a cut-out geometry (the only non-opaque one) stops a ray where the ray crosses it: with its
// texture, where the texture's alpha is at least the game's threshold at the level of detail the ray's
// cone (`footprint` metres across there) meets; without, as `untextured` says.
bool CutOutBlocks(uint row, uint primitive, float2 barycentrics, bool untextured, float footprint, float3x4 objectToWorld)
{
    GIGeometry geometry = g_Geometries[row];
    if ((geometry.flags & GI_GEOMETRY_ALPHA_TEXTURE) == 0) return untextured;

    uint3 i = TriangleIndices(geometry, primitive);
    float2 uv0 = VertexUV(geometry, i.x), uv1 = VertexUV(geometry, i.y), uv2 = VertexUV(geometry, i.z);
    float2 uv = (1.0f - barycentrics.x - barycentrics.y) * uv0 + barycentrics.x * uv1 + barycentrics.y * uv2;
    Texture2D<float4> texture = g_SceneTextures[NonUniformResourceIndex(geometry.texture - 1)];
    float lod = 0.0f;
    if (footprint > 0.0f)
    {
        float3 v0 = mul(objectToWorld, float4(PoolVertex(geometry, i.x).xyz, 1.0f));
        float3 v1 = mul(objectToWorld, float4(PoolVertex(geometry, i.y).xyz, 1.0f));
        float3 v2 = mul(objectToWorld, float4(PoolVertex(geometry, i.z).xyz, 1.0f));
        float2 size;
        texture.GetDimensions(size.x, size.y);
        lod = TextureLod(v0, v1, v2, uv0, uv1, uv2, size, footprint);
    }

    float4 texel = texture.SampleLevel(g_WrapSampler, uv, lod);
    return texel.a >= geometry.alphaThreshold;
}

// Every ray's flags. The terrain is one-sided, as the game draws it: where its heightmap runs across a
// cliff's recesses, the game shows the rock behind it from below, and so must the rays. Every other
// instance is seen from both sides (D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE).
#define GI_RAY_FLAGS RAY_FLAG_CULL_BACK_FACING_TRIANGLES

struct SceneHit
{
    bool hit;
    float distance;
    uint instance;  // Into g_Instances
    uint geometry;  // Within the instance's structure
    uint primitive; // Triangle within the geometry
    float2 barycentrics;
    bool frontFace; // Clockwise seen from the ray's origin (DXR's front)
};

// A ray leaving the surface a pixel shows: its cone starts as wide as the pixel there and spreads as
// `spread` (radians) does.
float2 PixelCone(float3 position, float spread)
{
    return float2(distance(position, g_Frame.eye.xyz) * g_Frame.rayCone.x, spread);
}

// The global illumination's rays' cone: as wide as a pixel where they start, spreading as a pixel's
// angle does (from the eye, a pixel's own cone).
float2 LightCone(float3 origin)
{
    return PixelCone(origin, g_Frame.rayCone.x);
}

// Instances whose mask shares a bit with `mask` take part. With `alphaTest`, cut-out geometry with its
// texture lets the ray through where it is clear, and cut-out geometry without blocks it as
// `untextured` says; the ray's cone (`cone`: its width at the origin and its growth a metre, metres)
// picks the texture's level of detail. Without, every triangle is opaque. Every ray tests alpha: the
// global illumination's rays once took every triangle as opaque, and a bank of grass, a heap of solid
// cards to them, went dark (2026-10-23).
SceneHit TraceClosest(float3 origin, float3 direction, float minDistance, float maxDistance, uint mask, bool alphaTest, bool untextured, float2 cone)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = minDistance;
    ray.TMax = maxDistance;

    SceneHit result;
    if (alphaTest)
    {
        RayQuery<RAY_FLAG_NONE> query;
        query.TraceRayInline(g_Scene, GI_RAY_FLAGS, mask, ray);
        while (query.Proceed())
        {
            uint row = query.CandidateInstanceContributionToHitGroupIndex() + query.CandidateGeometryIndex();
            float footprint = cone.x + query.CandidateTriangleRayT() * cone.y;
            if (CutOutBlocks(row, query.CandidatePrimitiveIndex(), query.CandidateTriangleBarycentrics(), untextured, footprint, query.CandidateObjectToWorld3x4()))
                query.CommitNonOpaqueTriangleHit();
        }

        result.hit = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
        result.distance = result.hit ? query.CommittedRayT() : FLT_MAX;
        result.instance = result.hit ? query.CommittedInstanceIndex() : 0;
        result.geometry = result.hit ? query.CommittedGeometryIndex() : 0;
        result.primitive = result.hit ? query.CommittedPrimitiveIndex() : 0;
        result.barycentrics = result.hit ? query.CommittedTriangleBarycentrics() : 0.0f;
        result.frontFace = result.hit && query.CommittedTriangleFrontFace();
        return result;
    }

    RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(g_Scene, GI_RAY_FLAGS, mask, ray);
    query.Proceed();

    result.hit = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    result.distance = result.hit ? query.CommittedRayT() : FLT_MAX;
    result.instance = result.hit ? query.CommittedInstanceIndex() : 0;
    result.geometry = result.hit ? query.CommittedGeometryIndex() : 0;
    result.primitive = result.hit ? query.CommittedPrimitiveIndex() : 0;
    result.barycentrics = result.hit ? query.CommittedTriangleBarycentrics() : 0.0f;
    result.frontFace = result.hit && query.CommittedTriangleFrontFace();
    return result;
}

// A global illumination ray (LightCone); cut-out geometry without its texture lets it through.
SceneHit TraceClosest(float3 origin, float3 direction, float minDistance, float maxDistance)
{
    return TraceClosest(origin, direction, minDistance, maxDistance, GI_MASK_SCENE, true, false, LightCone(origin));
}

SceneHit TraceClosest(float3 origin, float3 direction, float maxDistance)
{
    return TraceClosest(origin, direction, 0.0f, maxDistance);
}

// Whether a global illumination ray gets away, any hit ending it.
bool TraceVisible(float3 origin, float3 direction, float minDistance, float maxDistance)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = minDistance;
    ray.TMax = maxDistance;

    float2 cone = LightCone(origin);
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER> query;
    query.TraceRayInline(g_Scene, GI_RAY_FLAGS, GI_MASK_SCENE, ray);
    while (query.Proceed())
    {
        uint row = query.CandidateInstanceContributionToHitGroupIndex() + query.CandidateGeometryIndex();
        float footprint = cone.x + query.CandidateTriangleRayT() * cone.y;
        if (CutOutBlocks(row, query.CandidatePrimitiveIndex(), query.CandidateTriangleBarycentrics(), false, footprint, query.CandidateObjectToWorld3x4()))
            query.CommitNonOpaqueTriangleHit();
    }

    return query.CommittedStatus() == COMMITTED_NOTHING;
}

// The geometric normal of a hit triangle, facing the ray.
float3 HitNormal(SceneHit hit, float3 direction)
{
    SceneTriangle t = LoadTriangle(hit.instance, hit.geometry, hit.primitive);
    float3 normal = cross(t.v1 - t.v0, t.v2 - t.v0);
    float lengthSquared = dot(normal, normal);
    normal = lengthSquared > 0.0f ? normal * rsqrt(lengthSquared) : -direction;
    return dot(normal, direction) > 0.0f ? -normal : normal;
}

// Diagnostics: adds the lanes for which `counted` holds to a counter, one atomic a wave.
void CountDiagnostic(uint counter, bool counted)
{
    uint count = WaveActiveCountBits(counted);
    if (WaveIsFirstLane() && count) InterlockedAdd(g_Counters[counter], count);
}

// Diagnostics: the wave's largest channel into a GI_COUNTER_MAX_* counter, one atomic a wave. Light is
// never negative, so its bits order as its values do (a NaN as the largest of all).
void MaxDiagnostic(uint counter, float3 value)
{
    float largest = WaveActiveMax(max(max(value.r, value.g), value.b));
    if (WaveIsFirstLane() && largest > 0.0f) InterlockedMax(g_Counters[counter], asuint(largest));
}

// ---- Point and spot lights ----
// A point or spot light's diffuse light at a surface, without shadows, as the game's light shaders work
// it out (Lighting_*_NA, SSLighting2_*), in the sun colour's units; its direction and distance in `toLight`
// and `lightDistance`. A spot's mask (a flashlight's pattern) is left out.
float3 LocalLightAt(GILocalLight light, float3 world, float3 normal, out float3 toLight, out float lightDistance)
{
    float3 offset = light.position.xyz - world;
    lightDistance = length(offset);
    toLight = offset / max(lightDistance, 1e-6f);
    float cosine = dot(normal, toLight);
    if (cosine <= 0.0f || lightDistance >= light.position.w) return 0.0f;

    // A point's distance is at least its least, plus its added; a spot's is as it is.
    float d = max(lightDistance, light.falloff.z) + light.falloff.y;
    float d2 = d * d;
    float falloff = light.falloff.x > 0.0f ? (1.0f - min(d2 * d2 * d2 * d2 * d2 * d2 * d2 * d2, FLT_MAX) * light.color.w) / d2
                                           : 1.0f / d2 - d2 * light.color.w;
    falloff = max(min(falloff, light.shape.w), 0.0f);

    // A spot's cone: saturate((cosine - umbra) / penumbra) to its exponent; a point's "cone" is everywhere.
    float cut = 1.0f;
    if (light.axis.w >= -1.0f) cut = pow(saturate((dot(light.axis.xyz, -toLight) - light.axis.w) * light.shape.y), light.shape.z);

    return light.color.rgb * (falloff * cut * cosine);
}

// ---- Copies of a surface ----
// The scene can hold a surface twice, a little apart: the game swaps levels of detail, and draws
// some meshes at another level of detail for the shadows. A ray leaving one copy would find the
// other just above it and see nothing but its underside. Rays from a surface skip what faces the
// same way within this distance above where they start; it grows with the distance from the camera,
// as the levels of detail coarsen.
float CopyTolerance(float3 position)
{
    return min(0.05f + 0.005f * distance(position, g_Frame.eye.xyz), 0.5f);
}

bool IsCopyOfSurface(SceneHit hit, float3 normal, float3 direction, float tolerance)
{
    float height = hit.distance * dot(direction, normal);
    return height >= 0.0f && height < tolerance && abs(dot(HitNormal(hit, direction), normal)) > 0.85f;
}

// The closest hit of a ray leaving a surface of `normal` from `origin`, past copies of the surface;
// `alphaTest` as TraceClosest.
SceneHit TraceFromSurface(float3 origin, float3 normal, float tolerance, float3 direction, float maxDistance, uint mask, bool alphaTest, float2 cone)
{
    float start = 0.0f;
    SceneHit hit;
    for (uint attempt = 0; attempt < 4; ++attempt)
    {
        hit = TraceClosest(origin, direction, start, maxDistance, mask, alphaTest, false, cone);
        bool copy = hit.hit && IsCopyOfSurface(hit, normal, direction, tolerance);
        CountDiagnostic(GI_COUNTER_COPIES, copy);
        if (!copy) return hit;

        start = hit.distance * 1.0001f + 1e-3f;
    }

    hit.hit = false;
    hit.distance = FLT_MAX;
    return hit;
}

// A global illumination ray leaving a surface (LightCone).
SceneHit TraceFromSurface(float3 origin, float3 normal, float tolerance, float3 direction, float maxDistance)
{
    return TraceFromSurface(origin, normal, tolerance, direction, maxDistance, GI_MASK_SCENE, true, LightCone(origin));
}

// Whether a ray leaving a surface gets away, past copies of the surface: the stretch of the ray
// where copies can be takes closest hits, the rest the first hit found.
bool VisibleFromSurface(float3 origin, float3 normal, float tolerance, float3 direction, float maxDistance)
{
    float nearEnd = min(tolerance / max(dot(direction, normal), 0.1f), maxDistance);
    if (TraceFromSurface(origin, normal, tolerance, direction, nearEnd).hit) return false;

    return nearEnd >= maxDistance || TraceVisible(origin, direction, nearEnd, maxDistance);
}

// ---- The game's terrain ----
// The game's virtual heightmap, as its terrain vertex shaders read it (NewTerrain_*: t0 the page
// table, t1 the height atlas), for the rays: rings of cells around the camera, finer near
// (TerrainGrid in gi_scene.hlsl).
Texture2D<float4> g_TerrainPages : register(t14);
Texture2D<float> g_TerrainHeights : register(t15);

float2 TerrainUv(float2 xz)
{
    return min((xz - g_Frame.terrainOffset.xz) / g_Frame.terrainScale.xz, 0.999756f);
}

// The atlas tile (xy) and level of detail (z) resident for virtual heightmap coordinates.
float3 TerrainTile(float2 virtualUv)
{
    return floor(g_TerrainPages.SampleLevel(g_PointSampler, virtualUv, 0.0f).xyz * 255.4f);
}

// The height at virtual heightmap coordinates, before the terrain's scale: the tile's texel in the atlas.
float TerrainHeight(float2 virtualUv)
{
    float3 tile = TerrainTile(virtualUv);
    float2 tileUv = frac(virtualUv * g_Frame.terrainUvConverter.xy * exp2(-tile.z)) + tile.xy;
    float2 physicalUv = tileUv * g_Frame.terrainUvConverter.zw + g_Frame.terrainTexelCorrect.xy;
    return g_TerrainHeights.SampleLevel(g_LinearSampler, physicalUv, 0.0f);
}

float3 TerrainPoint(float2 xz)
{
    return float3(xz.x, TerrainHeight(TerrainUv(xz)) * g_Frame.terrainScale.y + g_Frame.terrainOffset.y, xz.y);
}

#endif
