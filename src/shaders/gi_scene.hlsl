// The ray-traced scene: captured geometry into the vertex pool, the instance list of the frame's
// acceleration structure, and a debug view of the scene from the camera (docs/DEVELOPMENT.md,
// "Global illumination").
#include "gi_common.hlsli"

// Each entry the frame captured starts its bounds over: its geometry was captured again, or for
// the first time.
[numthreads(64, 1, 1)]
void ClearBounds(uint did : SV_DispatchThreadID)
{
    if (did >= g_Frame.counts.x) return;

    uint entry = g_ScatterRecords[did].entry;
    if (g_ScatterRecords[did].flags & GI_SCATTER_NEW) g_EntryAlbedo[entry] = 0;
    for (uint axis = 0; axis < 3; ++axis)
    {
        g_EntryBounds[6 * entry + axis] = OrderedFromFloat(FLT_MAX);
        g_EntryBounds[6 * entry + 3 + axis] = OrderedFromFloat(-FLT_MAX);
    }
}

// The record holding a transfer vertex: records are sorted by their first vertex and adjoin.
uint FindRecord(uint vertex)
{
    uint low = 0, high = g_Frame.counts.x;
    while (high - low > 1)
    {
        uint middle = (low + high) / 2;
        if (g_ScatterRecords[middle].transferOffset <= vertex) low = middle;
        else high = middle;
    }

    return low;
}

// One thread per vertex the frame captured (g_Dispatch.x of them).
[numthreads(64, 1, 1)]
void Scatter(uint did : SV_DispatchThreadID)
{
    bool valid = did < g_Dispatch.x;
    uint index = valid ? FindRecord(did) : 0;
    GIScatterRecord record = g_ScatterRecords[index];
    float3 position = 0.0f;
    if (valid)
    {
        position = g_Transfer.Load(int3(did % GI_TRANSFER_WIDTH, did / GI_TRANSFER_WIDTH, 0)).xyz;
        g_VerticesOut[record.poolOffset + did - record.transferOffset] = float4(position, 1.0f);
    }

    // Most waves lie within one record: one lane updates its bounds for all.
    if (WaveActiveAllTrue(!valid || index == WaveReadLaneFirst(index)) && WaveActiveAnyTrue(valid))
    {
        float3 lowest = WaveActiveMin(valid ? position : FLT_MAX);
        float3 highest = WaveActiveMax(valid ? position : -FLT_MAX);
        if (valid && WaveIsFirstLane())
            for (uint axis = 0; axis < 3; ++axis)
            {
                InterlockedMin(g_EntryBounds[6 * record.entry + axis], OrderedFromFloat(lowest[axis]));
                InterlockedMax(g_EntryBounds[6 * record.entry + 3 + axis], OrderedFromFloat(highest[axis]));
            }
    }
    else if (valid)
        for (uint axis = 0; axis < 3; ++axis)
        {
            InterlockedMin(g_EntryBounds[6 * record.entry + axis], OrderedFromFloat(position[axis]));
            InterlockedMax(g_EntryBounds[6 * record.entry + 3 + axis], OrderedFromFloat(position[axis]));
        }
}

// The depth pyramid: level k holds the farthest depth (the smallest, reversed-Z) of each square of
// 2^(k+1) pixels, so a box can be tested against the frame's depth with a few reads.
#define GI_DEPTH_PYRAMID_LEVELS 12
Texture2D<float> g_DepthPyramid : register(t10);                                    // All levels, for BuildInstances
RWTexture2D<float> g_DepthPyramidLevels[GI_DEPTH_PYRAMID_LEVELS] : register(u96); // u96 to u107: one level each

// One level a dispatch, g_Dispatch.x: the farthest of the 2 x 2 texels under each texel of the
// level before (the depth itself before level 0). Levels are rounded down in size, so the last
// texel of a row or column also takes in what an odd level below leaves over: every texel below
// is covered.
[numthreads(8, 8, 1)]
void BuildDepthPyramid(uint2 did : SV_DispatchThreadID)
{
    uint level = g_Dispatch.x;
    uint width, height;
    g_DepthPyramidLevels[level].GetDimensions(width, height);
    if (any(did >= uint2(width, height))) return;

    uint2 below = g_Frame.dimensions.xy;
    if (level > 0) g_DepthPyramidLevels[level - 1].GetDimensions(below.x, below.y);
    uint2 from = min(did * 2, below - 1);
    uint2 to = min(did * 2 + 1, below - 1);
    if (did.x == width - 1) to.x = below.x - 1;
    if (did.y == height - 1) to.y = below.y - 1;

    // HLSL evaluates both sides of ?:, so the levels are read apart (there is no level -1).
    float farthest = FLT_MAX;
    if (level == 0)
    {
        for (uint y = from.y; y <= to.y; ++y)
            for (uint x = from.x; x <= to.x; ++x) farthest = min(farthest, g_Depth.Load(int3(x, y, 0)));
    }
    else
    {
        for (uint y = from.y; y <= to.y; ++y)
            for (uint x = from.x; x <= to.x; ++x) farthest = min(farthest, g_DepthPyramidLevels[level - 1][uint2(x, y)]);
    }

    g_DepthPyramidLevels[level][did] = farthest;
}

// True when the whole box is in the camera's view; `nearest` is then its nearest depth (the
// largest, reversed-Z) and `pixels` its rectangle on screen, in pixels (lowest xy, highest xy).
bool InsideView(float3 lowest, float3 highest, out float nearest, out float4 pixels)
{
    nearest = 0.0f;
    pixels = 0.0f;
    float2 ndcLow = 1.0f, ndcHigh = -1.0f;
    for (uint corner = 0; corner < 8; ++corner)
    {
        float3 position = float3((corner & 1) ? highest.x : lowest.x, (corner & 2) ? highest.y : lowest.y, (corner & 4) ? highest.z : lowest.z);
        float4 clip = mul(g_Frame.viewProjection, float4(position, 1.0f));
        if (clip.w <= 0.0f || any(abs(clip.xy) > clip.w) || clip.z < 0.0f || clip.z > clip.w) return false;

        float3 ndc = clip.xyz / clip.w;
        nearest = max(nearest, ndc.z);
        ndcLow = min(ndcLow, ndc.xy);
        ndcHigh = max(ndcHigh, ndc.xy);
    }

    // NDC y runs up, pixels down.
    float2 size = g_Frame.dimensions.xy;
    pixels = float4((ndcLow.x * 0.5f + 0.5f) * size.x, (0.5f - ndcHigh.y * 0.5f) * size.y, (ndcHigh.x * 0.5f + 0.5f) * size.x,
        (0.5f - ndcLow.y * 0.5f) * size.y);
    return true;
}

// True when something the game drew lies in front of all of the box: then the game left it out
// because it was hidden (occlusion culling), not because it is gone. The level read is the one
// where the rectangle spans at most about four texels a side.
bool Hidden(float nearest, float4 pixels, out float farthest, out uint level)
{
    farthest = 0.0f;
    level = 0;
    uint width, height, levels;
    g_DepthPyramid.GetDimensions(0, width, height, levels);
    if (levels == 0) return false;

    float span = max(max(pixels.z - pixels.x, pixels.w - pixels.y), 1.0f);
    level = min(uint(max(ceil(log2(span / 4.0f)) - 1.0f, 0.0f)), levels - 1);
    float texel = exp2(float(level + 1));
    uint levelWidth = max(width >> level, 1u), levelHeight = max(height >> level, 1u);
    uint2 first = min(uint2(max(pixels.xy, 0.0f) / texel), uint2(levelWidth, levelHeight) - 1);
    uint2 last = min(uint2(max(pixels.zw, 0.0f) / texel), uint2(levelWidth, levelHeight) - 1);

    farthest = FLT_MAX;
    for (uint y = first.y; y <= last.y; ++y)
        for (uint x = first.x; x <= last.x; ++x)
            farthest = min(farthest, g_DepthPyramid.Load(int3(x, y, level)));

    return nearest < farthest;
}

// The frame's instances. An entry the game did not draw this frame (nor lately: GI_INSTANCE_DRAWN)
// keeps its place in the scene unless all of it is in view and something of it would show: then
// the game would have drawn it, so it has moved, been removed or been swapped for another level of
// detail. One hidden behind what the game drew stays: the game culled it, and it still casts a
// shadow (in view and dropped, a cliff behind a nearer rock lost its shadow as the camera moved).
[numthreads(64, 1, 1)]
void BuildInstances(uint did : SV_DispatchThreadID)
{
    if (did >= g_Frame.dimensions.w) return;

    GIInstanceRecord record = g_InstanceRecords[did];
    uint reason = GI_REASON_DRAWN;
    float nearest = 0.0f, farthest = 0.0f;
    uint level = 0;
    bool inView = false;
    if ((record.flags & GI_INSTANCE_DRAWN) == 0)
    {
        float3 lowest, highest;
        for (uint axis = 0; axis < 3; ++axis)
        {
            lowest[axis] = FloatFromOrdered(g_EntryBounds[6 * record.entry + axis]);
            highest[axis] = FloatFromOrdered(g_EntryBounds[6 * record.entry + 3 + axis]);
        }

        // HLSL evaluates both sides of ?:, and Hidden must not run on a box not in view.
        float4 pixels;
        inView = InsideView(lowest, highest, nearest, pixels);
        if (!inView) reason = GI_REASON_OUT_OF_VIEW;
        else if (Hidden(nearest, pixels, farthest, level)) reason = GI_REASON_BEHIND;
        else reason = GI_REASON_LEFT_OUT;
    }

    bool visible = reason != GI_REASON_LEFT_OUT;
    g_InstanceDebug[did] = uint4(reason, asuint(nearest), asuint(farthest), level | (inView ? 256u : 0u));

    // Alpha-tested meshes are non-opaque, so the shadow rays can let some sun through them; the
    // global illumination's rays force them opaque.
    bool alphaTested = (record.flags & GI_INSTANCE_ALPHA_TESTED) != 0;
    uint mask = alphaTested ? GI_MASK_ALPHA_TESTED : GI_MASK_OPAQUE;
    const uint forceNonOpaque = 0x8u; // D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE

    GIInstanceDesc desc;
    desc.transform[0] = float4(1.0f, 0.0f, 0.0f, 0.0f);
    desc.transform[1] = float4(0.0f, 1.0f, 0.0f, 0.0f);
    desc.transform[2] = float4(0.0f, 0.0f, 1.0f, 0.0f);
    // A left-out instance stays for the shadow debug view's rays alone.
    desc.idAndMask = (did & 0xFFFFFFu) | ((visible ? mask : GI_MASK_HIDDEN) << 24);
    desc.hitGroupAndFlags = alphaTested ? forceNonOpaque << 24 : 0u;
    desc.blas = record.blas;
    g_InstanceDescs[did] = desc;
}

// The traced scene from the camera, shaded by its normals; red where it disagrees with the game's
// depth, magenta where the game drew something the scene lacks, yellow where the scene has
// something in the game's sky.
[numthreads(8, 8, 1)]
void DebugScene(uint2 did : SV_DispatchThreadID)
{
    if (any(did >= g_Frame.dimensions.xy)) return;

    float2 uv = (did + 0.5f) / g_Frame.dimensions.xy;
    float3 farPoint = transformPointProjection(uv, 1e-6f, g_Frame.viewProjectionInverse);
    float3 direction = normalize(farPoint - g_Frame.eye.xyz);
    SceneHit hit = TraceClosest(g_Frame.eye.xyz, direction, 1e5f);

    float depth = g_Depth.Load(int3(did, 0));
    float gameDistance = depth > 0.0f ? distance(transformPointProjection(uv, depth, g_Frame.viewProjectionInverse), g_Frame.eye.xyz) : FLT_MAX;

    float3 color = float3(0.02f, 0.03f, 0.08f);
    if (hit.hit)
    {
        float3 normal = HitNormal(hit, direction);
        color = (0.5f * normal + 0.5f) * 0.7f;
        if (depth <= 0.0f) color = lerp(color, float3(1.0f, 0.9f, 0.0f), 0.6f);
        else if (abs(hit.distance - gameDistance) > 0.05f * gameDistance + 0.1f) color = lerp(color, float3(1.0f, 0.0f, 0.0f), 0.6f);
    }
    else if (depth > 0.0f)
        color = float3(1.0f, 0.0f, 1.0f);

    g_Output[did] = float4(color, 1.0f);
}

// No light yet: the resolve adds nothing.
[numthreads(8, 8, 1)]
void ClearOutput(uint2 did : SV_DispatchThreadID)
{
    if (all(did < g_Frame.dimensions.xy)) g_Output[did] = 0.0f;
}
