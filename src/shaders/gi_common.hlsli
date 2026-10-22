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
Texture2D<float4> g_Transfer : register(t3); // Captured vertices, world space
StructuredBuffer<GIScatterRecord> g_ScatterRecords : register(t4);
StructuredBuffer<GIInstanceRecord> g_InstanceRecords : register(t5);
StructuredBuffer<float4> g_Vertices : register(t6); // The vertex pool: three per triangle, world space

RWTexture2D<float4> g_Output : register(u0);
RWStructuredBuffer<float4> g_VerticesOut : register(u1); // The vertex pool, while the frame's captures go in
RWStructuredBuffer<uint> g_EntryBounds : register(u2);   // Six per entry: minimum xyz, maximum xyz, ordered
RWStructuredBuffer<GIInstanceDesc> g_InstanceDescs : register(u3);
RWStructuredBuffer<uint> g_Counters : register(u4);
RWStructuredBuffer<uint4> g_DispatchArgs : register(u5);
RWStructuredBuffer<uint4> g_EntryAlbedo : register(u6); // Sums of linear albedo x 255 and their count, by entry
// By instance: why it is in the scene or left out (BuildInstances): x GI_REASON_*, y asuint(its box's
// nearest depth), z asuint(the farthest depth over its screen rectangle), w the pyramid level read |
// 256 when all of it is in view. For the shadow debug view and probe.
RWStructuredBuffer<uint4> g_InstanceDebug : register(u76);

SamplerState g_PointSampler : register(s0);
SamplerState g_LinearSampler : register(s1);

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

// Rays leave a surface the depth buffer gave from slightly in front of it: the captured triangles
// are not exactly where the game drew them (another level of detail, rounding).
float3 OffsetFromScreenSurface(float3 position, float3 normal)
{
    float distanceToEye = distance(position, g_Frame.eye.xyz);
    return position + normal * (0.02f + 0.002f * distanceToEye);
}

// Wächter and Binder's offset for a point on a captured triangle (Ray Tracing Gems, chapter 6).
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

struct SceneHit
{
    bool hit;
    float distance;
    uint instance;  // Into g_InstanceRecords
    uint primitive; // Triangle within the instance
    float2 barycentrics;
};

// Instances whose mask shares a bit with `mask` (GI_MASK_*) take part; the left-out (GI_MASK_HIDDEN) only
// for the shadow debug view.
SceneHit TraceClosest(float3 origin, float3 direction, float minDistance, float maxDistance, uint mask)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = minDistance;
    ray.TMax = maxDistance;

    RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(g_Scene, RAY_FLAG_NONE, mask, ray);
    query.Proceed();

    SceneHit result;
    result.hit = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    result.distance = result.hit ? query.CommittedRayT() : FLT_MAX;
    result.instance = result.hit ? query.CommittedInstanceID() : 0;
    result.primitive = result.hit ? query.CommittedPrimitiveIndex() : 0;
    result.barycentrics = result.hit ? query.CommittedTriangleBarycentrics() : 0.0f;
    return result;
}

SceneHit TraceClosest(float3 origin, float3 direction, float minDistance, float maxDistance)
{
    return TraceClosest(origin, direction, minDistance, maxDistance, GI_MASK_TRACED);
}

SceneHit TraceClosest(float3 origin, float3 direction, float maxDistance)
{
    return TraceClosest(origin, direction, 0.0f, maxDistance);
}

bool TraceVisible(float3 origin, float3 direction, float minDistance, float maxDistance)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = minDistance;
    ray.TMax = maxDistance;

    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER> query;
    query.TraceRayInline(g_Scene, RAY_FLAG_NONE, GI_MASK_TRACED, ray);
    query.Proceed();
    return query.CommittedStatus() == COMMITTED_NOTHING;
}

// The geometric normal of a hit triangle, facing the ray.
float3 HitNormal(SceneHit hit, float3 direction)
{
    GIInstanceRecord instance = g_InstanceRecords[hit.instance];
    uint first = instance.vertexOffset + 3 * hit.primitive;
    float3 v0 = g_Vertices[first].xyz, v1 = g_Vertices[first + 1].xyz, v2 = g_Vertices[first + 2].xyz;
    float3 normal = cross(v1 - v0, v2 - v0);
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

// The closest hit of a ray leaving a surface of `normal` from `origin`, past copies of the surface.
SceneHit TraceFromSurface(float3 origin, float3 normal, float tolerance, float3 direction, float maxDistance, uint mask)
{
    float start = 0.0f;
    SceneHit hit;
    for (uint attempt = 0; attempt < 4; ++attempt)
    {
        hit = TraceClosest(origin, direction, start, maxDistance, mask);
        bool copy = hit.hit && IsCopyOfSurface(hit, normal, direction, tolerance);
        CountDiagnostic(GI_COUNTER_COPIES, copy);
        if (!copy) return hit;

        start = hit.distance * 1.0001f + 1e-3f;
    }

    hit.hit = false;
    hit.distance = FLT_MAX;
    return hit;
}

SceneHit TraceFromSurface(float3 origin, float3 normal, float tolerance, float3 direction, float maxDistance)
{
    return TraceFromSurface(origin, normal, tolerance, direction, maxDistance, GI_MASK_TRACED);
}

// Whether a ray leaving a surface gets away, past copies of the surface: the stretch of the ray
// where copies can be takes closest hits, the rest the first hit found.
bool VisibleFromSurface(float3 origin, float3 normal, float tolerance, float3 direction, float maxDistance)
{
    float nearEnd = min(tolerance / max(dot(direction, normal), 0.1f), maxDistance);
    if (TraceFromSurface(origin, normal, tolerance, direction, nearEnd).hit) return false;

    return nearEnd >= maxDistance || TraceVisible(origin, direction, nearEnd, maxDistance);
}

// Floats as uints whose order is the floats' order, for atomic minimum and maximum.
uint OrderedFromFloat(float value)
{
    uint bits = asuint(value);
    return (bits & 0x80000000u) ? ~bits : bits | 0x80000000u;
}

float FloatFromOrdered(uint ordered)
{
    return asfloat((ordered & 0x80000000u) ? ordered & 0x7FFFFFFFu : ~ordered);
}

#endif
