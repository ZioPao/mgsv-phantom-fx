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

// True when the whole box is in the camera's view: the game would have drawn what it holds.
bool InsideView(float3 lowest, float3 highest)
{
    for (uint corner = 0; corner < 8; ++corner)
    {
        float3 position = float3((corner & 1) ? highest.x : lowest.x, (corner & 2) ? highest.y : lowest.y, (corner & 4) ? highest.z : lowest.z);
        float4 clip = mul(g_Frame.viewProjection, float4(position, 1.0f));
        if (clip.w <= 0.0f || any(abs(clip.xy) > clip.w) || clip.z < 0.0f || clip.z > clip.w) return false;
    }

    return true;
}

// The frame's instances. An entry the game did not draw this frame keeps its place in the scene
// unless all of it is in view: then the game would have drawn it, so it has moved, been removed
// or been swapped for another level of detail.
[numthreads(64, 1, 1)]
void BuildInstances(uint did : SV_DispatchThreadID)
{
    if (did >= g_Frame.dimensions.w) return;

    GIInstanceRecord record = g_InstanceRecords[did];
    bool visible = (record.flags & GI_INSTANCE_DRAWN) != 0;
    if (!visible)
    {
        float3 lowest, highest;
        for (uint axis = 0; axis < 3; ++axis)
        {
            lowest[axis] = FloatFromOrdered(g_EntryBounds[6 * record.entry + axis]);
            highest[axis] = FloatFromOrdered(g_EntryBounds[6 * record.entry + 3 + axis]);
        }

        visible = !InsideView(lowest, highest);
    }

    // Alpha-tested meshes are non-opaque, so the shadow rays can let some sun through them; the
    // global illumination's rays force them opaque.
    bool alphaTested = (record.flags & GI_INSTANCE_ALPHA_TESTED) != 0;
    uint mask = alphaTested ? GI_MASK_ALPHA_TESTED : GI_MASK_OPAQUE;
    const uint forceNonOpaque = 0x8u; // D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE

    GIInstanceDesc desc;
    desc.transform[0] = float4(1.0f, 0.0f, 0.0f, 0.0f);
    desc.transform[1] = float4(0.0f, 1.0f, 0.0f, 0.0f);
    desc.transform[2] = float4(0.0f, 0.0f, 1.0f, 0.0f);
    desc.idAndMask = (did & 0xFFFFFFu) | ((visible ? mask : 0u) << 24);
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
