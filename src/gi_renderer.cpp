// The global illumination's Direct3D 12 side; gi_renderer.hpp describes the route.
#include "gi_renderer.hpp"

#include "gi_d3d12.hpp"
#include "gi_light.hpp"
#include "gi_scene.hpp"
#include "gi_shadow.hpp"
#include "log.hpp"
#include "shaders/gi_shared.h"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <algorithm>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

// The kernels, compiled by DXC at build time (CMakeLists.txt).
#include "BlendScreenProbes.h"
#include "BuildDepthPyramid.h"
#include "CullCaptured.h"
#include "TerrainGrid.h"
#include "ClearBounds.h"
#include "ClassifyShadowTiles.h"
#include "ClearBuffer.h"
#include "ClearCounters.h"
#include "ClearOutput.h"
#include "ClearProbeMask.h"
#include "CompactScreenProbes.h"
#include "CopyProbes.h"
#include "CountScreenProbes.h"
#include "DebugScene.h"
#include "DebugShadows.h"
#include "FilterGI.h"
#include "FilterProbeMask.h"
#include "FilterScreenProbes.h"
#include "FilterShadows.h"
#include "GenerateDispatch.h"
#include "InitCachedTileLRU.h"
#include "InterpolateScreenProbes.h"
#include "LearnAlbedo.h"
#include "PatchScreenProbes.h"
#include "PopulateCells.h"
#include "ProbeShadows.h"
#include "PopulateMultibounceCells.h"
#include "PopulateScreenProbes.h"
#include "ProjectScreenProbes.h"
#include "PurgeTiles.h"
#include "ReorderScreenProbes.h"
#include "ReprojectGI.h"
#include "ReprojectScreenProbes.h"
#include "ResolveCells.h"
#include "SampleScreenProbes.h"
#include "ScanAddOffsets.h"
#include "ScanBlockSums.h"
#include "ScanBlocks.h"
#include "Scatter.h"
#include "ScatterScreenProbes.h"
#include "SpawnScreenProbes.h"
#include "StoreSpawnCount.h"
#include "TraceSunShadows.h"
#include "UpdateMultibounceCells.h"
#include "UpdateTiles.h"

namespace phantom_fx::gi_renderer
{
namespace
{
using namespace phantom_fx::gi;
using namespace phantom_fx::gi_d3d12;
using gi_scene::BlasId;
using gi_scene::FramesInFlight;
using gi_scene::MaxGeometries;
using gi_scene::MaxInstances;

static_assert(MaxInstances <= (1u << GI_HIT_INSTANCE_BITS));

// ReShade's proxies return their native object for this interface (IID_UnwrappedObject).
constexpr GUID UnwrappedObject{0x7f2c9a11, 0x3b4e, 0x4d6a, {0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42}};
constexpr DWORD FenceTimeout = 10'000;

// The frame's skinned meshes, as many vertices as a frame can capture, in one structure built again
// each frame. The renderer's own upload buffer holds the frame's constants, scatter records and the
// probe's entry states.
constexpr std::uint64_t SkinnedVertices = TransferVertices;
constexpr std::uint64_t UploadBytes = 8ull << 20;
// A captured mesh the game has not drawn for this many frames leaves the scene.
constexpr std::uint32_t RetentionFrames = 3600;
// A captured mesh the game drew in more than one frame counts as drawn this long after: when the
// camera moves, a static mesh can get a new key (terrain did each frame, gi_capture.cpp, until its keys
// left out the stale second vertex slot), and a copy under a new key waits up to about 20 frames for its
// acceleration structure. Without this, the copy from before the camera moved left the scene at once
// (in view, not drawn) and the mesh was missing until the camera stopped: its ray-traced shadow
// vanished while the camera turned. Two seconds cover a turn. A moving mesh, drawn once under each
// key, gets none.
constexpr std::uint32_t DrawnGraceFrames = 120;
// The depth pyramid's levels at most (GI_DEPTH_PYRAMID_LEVELS in gi_scene.hlsl), and the first of
// their unordered access slots.
constexpr std::size_t MaxPyramidLevels = 12;
constexpr UINT PyramidSlot = 96;
// Where no albedo is known yet: MGSV's materials are dark.
constexpr float DefaultAlbedo = 0.2f;
// Tangent of the sun's angular radius, 0.27 degrees.
constexpr float SunRadius = 0.0047f;
// The sun's ray-traced shadows: how much of the sun cut-out geometry without the game's alpha blocks
// beyond the game's shadow map, and how far the rays look for what blocks the sun.
constexpr float AlphaTestedBlocking = 0.5f, ShadowRayLength = 5000;
// Diagnostics: the most phases a frame's list times.
constexpr std::uint32_t MaxMarks = 32;
// A model the game has not placed for this many frames is let go.
constexpr std::uint32_t ModelRetentionFrames = 600;
// The meshes of a model the scene takes at most (GI_HIT_INSTANCE_BITS).
constexpr std::size_t MaxModelParts = 4096;
// The terrain's rings (TerrainGrid in gi_scene.hlsl), a structure each. Ring r's cells are
// TerrainCellSize x 4^r metres: 256 m, 1 km, 4 km and 16 km across. A ring's corner moves in steps of
// eight of its cells (two of the next ring's: the rings' edges stay on the outer ring's grid lines); a
// ring that moves, or whose inner ring moves, is made again and its structure built again; every
// TerrainRefreshFrames (staggered by ring) it is made again and refitted, as the game streams finer
// pages of its heightmap in.
constexpr float TerrainCellSize = 1.0f;
constexpr std::uint32_t TerrainRefreshFrames = 120;
constexpr std::uint32_t TerrainRingVertices = GI_TERRAIN_CELLS * GI_TERRAIN_CELLS * 6;

// The kernels' DXIL in gi_d3d12::Kernel's order.
constexpr std::span<const unsigned char> Bytecode[]{GI_ClearBounds, GI_Scatter, GI_CullCaptured, GI_DebugScene, GI_ClearOutput, GI_BuildDepthPyramid, GI_TerrainGrid,
    GI_ClearCounters, GI_GenerateDispatch, GI_ClearBuffer, GI_ScanBlocks, GI_ScanBlockSums, GI_ScanAddOffsets, GI_ClearProbeMask,
    GI_FilterProbeMask, GI_InitCachedTileLRU, GI_ReprojectScreenProbes, GI_CountScreenProbes, GI_ScatterScreenProbes, GI_SpawnScreenProbes,
    GI_CompactScreenProbes, GI_PatchScreenProbes, GI_StoreSpawnCount, GI_SampleScreenProbes, GI_LearnAlbedo, GI_PopulateScreenProbes,
    GI_PurgeTiles, GI_PopulateMultibounceCells, GI_PopulateCells, GI_UpdateTiles, GI_UpdateMultibounceCells, GI_ResolveCells,
    GI_BlendScreenProbes, GI_CopyProbes, GI_ReorderScreenProbes, GI_FilterScreenProbes, GI_ProjectScreenProbes, GI_InterpolateScreenProbes,
    GI_ReprojectGI, GI_FilterGI, GI_TraceSunShadows, GI_ClassifyShadowTiles, GI_FilterShadows,
    GI_DebugShadows, GI_ProbeShadows};
static_assert(std::size(Bytecode) == static_cast<std::size_t>(Kernel::Count));

// A static mesh captured from the game's draws (gi_capture.cpp): world-space triangles, three
// vertices each, in the pool, and its structure.
struct Entry
{
    bool live{};
    bool hasVertices{};
    std::uint64_t poolOffset{};
    std::uint32_t vertexCount{};
    std::optional<BlasId> blas;
    bool needsBlas{};
    std::uint32_t lastDrawn{};
    std::uint32_t framesDrawn{}; // Diagnostics: the frames the game drew it in
    bool alphaTested{};
};

// One of the game's models on the GPU side: its meshes in the pool from `poolOffset`, a geometry
// each of its structure, made once it is first placed; the frame it was last placed.
struct ModelRecord
{
    std::shared_ptr<const Model> geometry;
    std::optional<std::uint64_t> poolOffset;
    std::uint64_t poolBytes{};
    std::optional<BlasId> blas;
    std::uint32_t lastPlaced{};
    std::array<float, 6> bounds{}; // Diagnostics (the shadow probe): object space, lowest then highest
};

// Diagnostics (the probe): one of a frame's instances of the game's models, and where it is.
struct InstancedModel
{
    std::uint32_t instance{};
    std::uint64_t hash{};
    std::array<float, 3> position{};
};

// A ring of the terrain: its vertices in the pool and its structure, kept while the renderer runs.
struct TerrainRing
{
    std::uint64_t poolOffset{};
    BlasId blas{};
    bool placed{};
    std::array<float, 2> corner{};
    std::uint32_t refreshed{};
};

// Direct3D 12's view of a texture Direct3D 11 shares, opened again when Direct3D 11 replaces it.
// Holding the texture keeps its address from being reused by another.
struct Shared
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D12Resource> resource;
};

// One of the game's textures (SceneTexture) and its descriptor in the scene's texture range
// (g_SceneTextures); the models whose meshes use it.
struct TextureRecord
{
    Shared shared;
    std::uint32_t index{};
    std::unordered_set<std::uint64_t> users;
};

// A texture descriptor, or the texture it views, the GPU may still use: given back once it is past the frame.
struct RetiredTexture
{
    std::uint64_t fenceValue{};
    std::uint32_t index{};
    ComPtr<ID3D12Resource> resource;
};

// Diagnostics: the steps of a frame's CPU time.
enum CpuStep { WaitStep, ProbeStep, ModelsStep, InstancesStep, RecordStep, CpuSteps };
constexpr const char* CpuStepNames[CpuSteps]{"waiting for the GPU", "the probe", "models", "instances", "recording"};

struct State
{
    ComPtr<ID3D12Device> wrapper;
    ComPtr<ID3D12Device5> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    std::array<ComPtr<ID3D12CommandAllocator>, FramesInFlight> allocators;
    std::array<std::uint64_t, FramesInFlight> submitted{};
    ComPtr<ID3D12GraphicsCommandList4> list;
    HANDLE event{};
    std::uint64_t frames{};

    ComPtr<ID3D12RootSignature> rootSignature;
    Pipelines pipelines;
    ComPtr<ID3D12CommandSignature> dispatchSignature;
    ComPtr<ID3D12DescriptorHeap> heap;
    UINT descriptorSize{};

    std::unique_ptr<gi_scene::Scene> scene;
    ComPtr<ID3D12Resource> entryBounds, geometryAlbedo, counters, dispatchArguments;
    std::array<ComPtr<ID3D12Resource>, FramesInFlight> upload;
    std::array<std::uint8_t*, FramesInFlight> uploadData{};

    // The captured meshes, and the frame's skinned meshes' vertices and structure.
    std::vector<Entry> entries = std::vector<Entry>(MaxEntries);
    std::uint64_t skinnedOffset{};
    BlasId skinnedBlas{};
    // The game's models (engine_scene.hpp) by hash, and the latest placements.
    std::unordered_map<std::uint64_t, ModelRecord> models;
    std::vector<ModelInstance> modelInstances;
    std::array<TerrainRing, GI_TERRAIN_RINGS> terrain;
    // The game's textures the models' meshes use, by path hash; their free descriptors, and those the GPU
    // may still read.
    std::unordered_map<std::uint64_t, TextureRecord> textures;
    std::vector<std::uint32_t> freeTextureIndices = [] {
        std::vector<std::uint32_t> free;
        for (auto index = GI_MAX_TEXTURES; index > 0; --index) free.push_back(index - 1);
        return free;
    }();
    std::vector<RetiredTexture> retiredTextures;
    // Diagnostics: the frame's model instances and their triangles, the models waiting for room, and
    // models too large for a frame's upload.
    std::uint32_t modelInstanceCount{}, modelsWaiting{}, modelsTooLarge{}, terrainRings{};
    std::uint64_t modelTriangles{};
    std::uint32_t liveEntries{}, instances{};
    std::uint64_t triangles{};
    // Diagnostics: the live captured meshes, and their triangles, by when the game last drew them: this
    // frame, within a second, within ten seconds, before.
    std::array<std::uint32_t, 4> entriesByAge{};
    std::array<std::uint64_t, 4> trianglesByAge{};
    // Diagnostics: the live captured meshes the game drew in one frame only, an earlier one.
    std::uint32_t drawnOnce{};
    std::uint64_t drawnOnceTriangles{};

    std::unique_ptr<gi_light::Light> light;
    std::unique_ptr<gi_shadow::Shadows> shadows;
    // Diagnostics (the shadow debug view and probe): why each instance is in the scene or left out,
    // the probe and the captured meshes its ray crosses, read back when the frame's slot comes round
    // again; each frame's instance by entry.
    ComPtr<ID3D12Resource> instanceDebug, probe, probeCrossings;
    std::array<ComPtr<ID3D12Resource>, FramesInFlight> probeReadback;
    std::array<bool, FramesInFlight> probeWritten{};
    std::vector<std::uint32_t> entryInstance = std::vector<std::uint32_t>(MaxEntries, GI_REASON_NONE);
    std::array<std::vector<InstancedModel>, FramesInFlight> instancedModels; // Each slot's frame's, by instance
    // Diagnostics: the entries evicted in each of the last frames.
    std::array<std::uint32_t, 60> evictedByFrame{};
    // The frame's depth pyramid for CullCaptured (gi_scene.hlsl): level k is the farthest depth of
    // each square of 2^(k+1) pixels, the first at half the render size.
    ComPtr<ID3D12Resource> depthPyramid;
    UINT pyramidWidth{}, pyramidHeight{}, pyramidLevels{};

    // Diagnostics: GPU timestamps between the named phases of each frame's list (Recorder::mark),
    // read when the frame's slot comes round again. The scene's and the light's times are averaged
    // for the overlay; each phase's are summed for the log, with the frame's builds and CPU time.
    ComPtr<ID3D12QueryHeap> timestamps;
    ComPtr<ID3D12Resource> timestampReadback;
    std::uint64_t timestampFrequency{};
    std::array<std::vector<const char*>, FramesInFlight> marks;
    double sceneMilliseconds{}, lightMilliseconds{}, shadowMilliseconds{};
    struct Tally
    {
        std::vector<std::pair<const char*, double>> phases; // Milliseconds, in the order they first ran
        std::uint32_t timedFrames{}, frames{}, builds{}, mostBuilds{}, waiting{}, compactions{};
        std::uint64_t builtTriangles{}, skinnedTriangles{};
        double cpuMilliseconds{}, mostCpuMilliseconds{};
        std::array<double, CpuSteps> cpuSteps{};
        std::uint64_t since{};
    } tally;

    // Diagnostics: a copy of the output every ten seconds, read once the GPU is past it.
    ComPtr<ID3D12Resource> statsReadback, statsCounters;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT statsFootprint{};
    std::uint64_t statsFence{}, lastStats{};
    Shared depth, normal, albedo, transfer, output, shadow, shadowDebug, terrainPages, terrainHeights;

    ~State()
    {
        if (event) CloseHandle(event);
    }
};
std::unique_ptr<State> state;

// The latest shadow probe as text, for the overlay (another thread) and the log; kept apart from
// the state so stopping does not race the overlay.
std::mutex probeMutex;
std::string probeText, lastProbeKey;
std::uint64_t lastProbeLog{};

ComPtr<ID3D12Resource> openShared(State& s, Shared& shared, ID3D11Texture2D* texture, const char* what)
{
    if (shared.texture.Get() == texture) return shared.resource;

    ComPtr<IDXGIResource1> resource;
    check(texture->QueryInterface(IID_PPV_ARGS(&resource)), what);
    HANDLE handle{};
    check(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle), what);
    ComPtr<ID3D12Resource> opened;
    const auto result = s.device->OpenSharedHandle(handle, IID_PPV_ARGS(&opened));
    CloseHandle(handle);
    check(result, what);
    shared.texture = texture;
    shared.resource = opened;

    return opened;
}

void createPipelines(State& s)
{
    D3D12_DESCRIPTOR_RANGE ranges[3]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = SrvCount;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = UavCount;
    ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[2].NumDescriptors = GI_MAX_TEXTURES;
    ranges[2].RegisterSpace = 2;

    D3D12_ROOT_PARAMETER parameters[6]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; // b0: the frame's constants
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; // b1: the dispatch's
    parameters[1].Constants.ShaderRegister = 1;
    parameters[1].Constants.Num32BitValues = 4;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[2].DescriptorTable.NumDescriptorRanges = 1;
    parameters[2].DescriptorTable.pDescriptorRanges = &ranges[0];
    parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[3].DescriptorTable.NumDescriptorRanges = 1;
    parameters[3].DescriptorTable.pDescriptorRanges = &ranges[1];
    parameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; // t0, space1: the scene
    parameters[4].Descriptor.RegisterSpace = 1;
    parameters[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; // t0 on, space2: the game's textures
    parameters[5].DescriptorTable.NumDescriptorRanges = 1;
    parameters[5].DescriptorTable.pDescriptorRanges = &ranges[2];

    // Point and linear clamped, linear wrapped (s2, for the models' texture coordinates).
    D3D12_STATIC_SAMPLER_DESC samplers[3]{};
    for (UINT i = 0; i < 3; ++i)
    {
        samplers[i].Filter = i == 0 ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplers[i].AddressU = samplers[i].AddressV = samplers[i].AddressW = i == 2 ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[i].ShaderRegister = i;
    }

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = static_cast<UINT>(std::size(parameters));
    desc.pParameters = parameters;
    desc.NumStaticSamplers = static_cast<UINT>(std::size(samplers));
    desc.pStaticSamplers = samplers;
    ComPtr<ID3DBlob> blob, errors;
    const auto result = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, blob.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(result))
        throw std::runtime_error("Serializing the root signature failed: "
            + (errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : hex(static_cast<std::uint32_t>(result))));

    check(s.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&s.rootSignature)), "Creating the root signature");

    for (std::size_t kernel = 0; kernel < s.pipelines.size(); ++kernel)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature = s.rootSignature.Get();
        pipeline.CS = {Bytecode[kernel].data(), Bytecode[kernel].size()};
        check(s.device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&s.pipelines[kernel])), "Creating a kernel");
    }

    D3D12_INDIRECT_ARGUMENT_DESC argument{};
    argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC signature{};
    signature.ByteStride = 16;
    signature.NumArgumentDescs = 1;
    signature.pArgumentDescs = &argument;
    check(s.device->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&s.dispatchSignature)), "Creating the dispatch signature");
}

void writeTexture(State& s, std::uint32_t index, ID3D12Resource* texture, DXGI_FORMAT format, std::uint32_t mips);

void createResources(State& s)
{
    auto* device = s.device.Get();
    s.scene = std::make_unique<gi_scene::Scene>(device);
    s.entryBounds = createStorage(device, std::uint64_t{MaxEntries} * 6 * 4, "Creating the entry bounds");
    s.geometryAlbedo = createStorage(device, std::uint64_t{MaxGeometries} * 16, "Creating the geometry albedo");
    s.counters = createStorage(device, GI_COUNTER_COUNT * 4, "Creating the counters");
    s.instanceDebug = createStorage(device, std::uint64_t{MaxInstances} * 16, "Creating the instance diagnostics");
    s.probe = createStorage(device, sizeof(GIShadowProbe), "Creating the shadow probe");
    s.probeCrossings = createStorage(device, std::uint64_t{GI_PROBE_CROSSINGS} * 16, "Creating the shadow probe's crossings");
    for (auto& readback : s.probeReadback)
        readback = createBuffer(device, sizeof(GIShadowProbe) + std::uint64_t{GI_PROBE_CROSSINGS} * 16, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_FLAG_NONE, "Creating the shadow probe's readback");
    s.dispatchArguments = createStorage(device, GI_DISPATCH_SLOTS * 16, "Creating the dispatch arguments");

    for (std::uint32_t slot = 0; slot < FramesInFlight; ++slot)
    {
        s.upload[slot] = createBuffer(device, UploadBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE,
            "Creating an upload buffer");
        void* data{};
        check(s.upload[slot]->Map(0, nullptr, &data), "Mapping an upload buffer");
        s.uploadData[slot] = static_cast<std::uint8_t*>(data);
    }

    // The skinned meshes' and the terrain's room in the pool and their structures, kept while the
    // renderer runs.
    const auto skinned = s.scene->allocate(SkinnedVertices * 16);
    const auto skinnedBlas = skinned ? s.scene->createBlas(gi_scene::Use::PerFrame, {{*skinned, static_cast<std::uint32_t>(SkinnedVertices)}}) : std::nullopt;
    if (!skinnedBlas) throw std::runtime_error("The scene has no room for the skinned meshes.");

    s.skinnedOffset = *skinned;
    s.skinnedBlas = *skinnedBlas;
    for (auto& ring : s.terrain)
    {
        const auto vertices = s.scene->allocate(std::uint64_t{TerrainRingVertices} * 16);
        const auto blas = vertices ? s.scene->createBlas(gi_scene::Use::Refit, {{*vertices, TerrainRingVertices}}) : std::nullopt;
        if (!blas) throw std::runtime_error("The scene has no room for the terrain.");

        ring.poolOffset = *vertices;
        ring.blas = *blas;
    }

    D3D12_QUERY_HEAP_DESC queries{};
    queries.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    queries.Count = MaxMarks * FramesInFlight;
    check(device->CreateQueryHeap(&queries, IID_PPV_ARGS(&s.timestamps)), "Creating the timestamps");
    s.timestampReadback = createBuffer(device, std::uint64_t{queries.Count} * 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_FLAG_NONE, "Creating the timestamp readback");
    check(s.queue->GetTimestampFrequency(&s.timestampFrequency), "Reading the timestamp frequency");

    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = TableSize * FramesInFlight + GI_MAX_TEXTURES;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    check(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&s.heap)), "Creating the descriptor heap");
    s.descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (std::uint32_t index = 0; index < GI_MAX_TEXTURES; ++index) writeTexture(s, index, nullptr, DXGI_FORMAT_R8G8B8A8_UNORM, 1);
}

// A descriptor of the scene's texture range: the texture's levels of detail, or a null view.
void writeTexture(State& s, std::uint32_t index, ID3D12Resource* texture, DXGI_FORMAT format, std::uint32_t mips)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = format;
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Texture2D.MipLevels = mips;
    auto at = s.heap->GetCPUDescriptorHandleForHeapStart();
    at.ptr += (std::size_t{TableSize} * FramesInFlight + index) * s.descriptorSize;
    s.device->CreateShaderResourceView(texture, &desc, at);
}

void waitFor(State& s, std::uint64_t value)
{
    if (!value || s.fence->GetCompletedValue() >= value) return;

    check(s.fence->SetEventOnCompletion(value, s.event), "Waiting for Direct3D 12");
    if (WaitForSingleObject(s.event, FenceTimeout) != WAIT_OBJECT_0) throw std::runtime_error("The global illumination's GPU work did not finish within 10 seconds.");
}

void evict(State& s, std::uint32_t index, std::vector<std::uint32_t>& evicted)
{
    auto& entry = s.entries[index];
    if (!entry.live) return;

    if (entry.hasVertices) s.scene->release(entry.poolOffset, std::uint64_t{entry.vertexCount} * 16);
    if (entry.blas) s.scene->destroyBlas(*entry.blas);
    entry = {};
    evicted.push_back(index);
}

// The live captured meshes the frame did not draw, the one drawn longest ago first.
std::vector<std::uint32_t> leastRecentlyDrawn(const State& s, const Frame& frame)
{
    std::vector<std::uint32_t> candidates;
    for (std::uint32_t i = 0; i < DynamicEntry; ++i)
        if (s.entries[i].live && s.entries[i].lastDrawn != frame.frameIndex) candidates.push_back(i);

    std::sort(candidates.begin(), candidates.end(), [&](auto a, auto b) { return s.entries[a].lastDrawn < s.entries[b].lastDrawn; });

    return candidates;
}

// When the pool or the structures have no room: the first time a frame, evicts the captured meshes the
// game drew longest ago until an eighth of the room, and at least `wanted`, is on its way back. The
// memory returns once the GPU is past this frame, so what wanted the room waits, as does the frame's
// other: the search runs once a frame, not once an allocation.
void evictForRoom(State& s, bool structures, std::uint64_t wanted, const Frame& frame, std::vector<std::uint32_t>& evicted, bool& evictedThisFrame)
{
    if (evictedThisFrame) return;

    evictedThisFrame = true;
    const auto stats = s.scene->stats();
    const auto target = std::max(wanted, (structures ? stats.blasCapacity : stats.poolCapacity) / 8);
    std::uint64_t freed{};
    for (const auto index : leastRecentlyDrawn(s, frame))
    {
        if (freed >= target) break;

        const auto& entry = s.entries[index];
        freed += structures ? (entry.blas ? s.scene->size(*entry.blas) : 0) : (entry.hasVertices ? std::uint64_t{entry.vertexCount} * 16 : 0);
        evict(s, index, evicted);
    }
}

// A model mesh's row of the geometry table, with its base texture once the game's is copied: its
// colour for the hits' albedo, and a cut-out mesh's alpha for the shadow rays' test.
gi_scene::Geometry materialOf(const State& s, const ModelPart& part)
{
    gi_scene::Geometry geometry;
    geometry.flags = (part.alphaTested ? GI_GEOMETRY_CUT_OUT : 0u) | (part.uvs.empty() ? 0u : GI_GEOMETRY_UV);
    const auto texture = part.uvs.empty() ? s.textures.end() : s.textures.find(part.baseTexture);
    if (texture == s.textures.end() || !texture->second.shared.resource) return geometry;

    geometry.texture = texture->second.index + 1;
    if (part.maskInRed) geometry.flags |= GI_GEOMETRY_MASK_IN_RED;
    if (part.alphaTested)
    {
        geometry.flags |= GI_GEOMETRY_ALPHA_TEXTURE;
        geometry.alphaThreshold = CutOutThreshold;
    }

    return geometry;
}

// The bytes of a model's meshes in the pool: each mesh's vertices (16 bytes: the position, then the
// texture coordinates), then its indices, from 16-byte boundaries.
std::uint64_t modelBytes(const Model& model)
{
    std::uint64_t bytes{};
    for (std::size_t i = 0; i < model.parts.size() && i < MaxModelParts; ++i)
        bytes += model.parts[i].vertices.size() * 16 + (model.parts[i].indices.size() * 2 + 15) / 16 * 16;

    return bytes;
}

// The model's meshes laid out from `poolOffset` into `bytes`, and their geometries.
std::vector<gi_scene::Geometry> stageModel(const State& s, const Model& model, std::uint64_t poolOffset, std::vector<std::uint8_t>& bytes)
{
    std::vector<gi_scene::Geometry> geometries;
    bytes.assign(modelBytes(model), 0);
    std::uint64_t at{};
    for (std::size_t i = 0; i < model.parts.size() && i < MaxModelParts; ++i)
    {
        const auto& part = model.parts[i];
        auto geometry = materialOf(s, part);
        geometry.vertexOffset = poolOffset + at;
        geometry.vertexCount = static_cast<std::uint32_t>(part.vertices.size());
        for (std::size_t v = 0; v < part.vertices.size(); ++v)
        {
            const std::uint32_t uv = v < part.uvs.size() ? part.uvs[v] : 0;
            std::memcpy(bytes.data() + at, part.vertices[v].data(), 12);
            std::memcpy(bytes.data() + at + 12, &uv, 4);
            at += 16;
        }

        geometry.indexOffset = poolOffset + at;
        geometry.indexCount = static_cast<std::uint32_t>(part.indices.size());
        std::memcpy(bytes.data() + at, part.indices.data(), part.indices.size() * 2);
        at += (part.indices.size() * 2 + 15) / 16 * 16;
        geometries.push_back(geometry);
    }

    return geometries;
}

// The game's textures copied since the last frame into the scene's texture range: a new descriptor each,
// the one before retired once the GPU is past this frame, and the meshes using them given theirs.
void updateTextures(State& s, const Frame& frame)
{
    for (const auto& made : frame.newTextures)
    {
        const auto found = s.textures.find(made.path);
        if (found == s.textures.end() || s.freeTextureIndices.empty()) continue;

        auto& record = found->second;
        const bool replaced = record.shared.resource != nullptr;
        if (replaced) s.retiredTextures.push_back({frame.signalValue, record.index, record.shared.resource});

        record.shared = {};
        const auto resource = openShared(s, record.shared, made.texture, "Opening one of the game's textures");
        record.index = s.freeTextureIndices.back();
        s.freeTextureIndices.pop_back();
        writeTexture(s, record.index, resource.Get(), made.viewFormat, made.mips);

        for (const auto user : record.users)
        {
            const auto model = s.models.find(user);
            if (model == s.models.end() || !model->second.blas) continue;

            const auto& parts = model->second.geometry->parts;
            for (std::uint32_t i = 0; i < parts.size() && i < MaxModelParts; ++i)
                if (parts[i].baseTexture == made.path)
                {
                    const auto geometry = materialOf(s, parts[i]);
                    s.scene->setMaterial(*model->second.blas, i, geometry.flags, geometry.texture, geometry.alphaThreshold);
                }
        }
    }

    const auto completed = s.fence->GetCompletedValue();
    std::erase_if(s.retiredTextures, [&](const RetiredTexture& retired) {
        if (retired.fenceValue > completed) return false;

        s.freeTextureIndices.push_back(retired.index);
        return true;
    });
}

// A model's meshes' textures, wanted (`use`) or no longer: a texture no model uses is let go
// (Result::releasedTextures), its descriptor retired.
void useTextures(State& s, const Model& model, bool use, const Frame& frame, Result& result)
{
    for (const auto& part : model.parts)
    {
        if (!part.baseTexture) continue;

        if (use)
        {
            s.textures[part.baseTexture].users.insert(model.hash);
            continue;
        }

        const auto found = s.textures.find(part.baseTexture);
        if (found == s.textures.end()) continue;

        found->second.users.erase(model.hash);
        if (!found->second.users.empty()) continue;

        if (found->second.shared.resource) s.retiredTextures.push_back({frame.signalValue, found->second.index, found->second.shared.resource});
        result.releasedTextures.push_back(found->first);
        s.textures.erase(found);
    }
}

// The frame's models: new ones registered, the placed ones' meshes into the pool and their structures
// made (as the frame's upload and the memory allow), those not placed for ModelRetentionFrames let go
// (Result::releasedModels).
void updateModels(State& s, const Frame& frame, Result& result)
{
    for (const auto& model : frame.newModels)
    {
        if (!model || s.models.contains(model->hash)) continue;

        useTextures(s, *model, true, frame, result);
        auto& record = s.models[model->hash];
        record.geometry = model;
        record.bounds = {FLT_MAX, FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX};
        for (const auto& part : model->parts)
            for (const auto& vertex : part.vertices)
                for (int axis = 0; axis < 3; ++axis)
                {
                    record.bounds[axis] = std::min(record.bounds[axis], vertex[axis]);
                    record.bounds[3 + axis] = std::max(record.bounds[3 + axis], vertex[axis]);
                }
    }

    updateTextures(s, frame);

    if (frame.modelInstancesUpdated) s.modelInstances = frame.modelInstances;
    // Placements come grouped by model (an instance group's, a brush plugin's): one lookup a run.
    auto found = s.models.end();
    std::uint64_t lastHash{};
    for (const auto& placement : s.modelInstances)
    {
        if (placement.hash != lastHash) found = s.models.find(lastHash = placement.hash);
        if (found != s.models.end()) found->second.lastPlaced = frame.frameIndex;
    }

    s.modelsWaiting = 0;
    std::vector<std::uint8_t> bytes;
    for (auto it = s.models.begin(); it != s.models.end();)
    {
        auto& record = it->second;
        if (frame.frameIndex - record.lastPlaced > ModelRetentionFrames)
        {
            if (record.blas) s.scene->destroyBlas(*record.blas);
            if (record.poolOffset) s.scene->release(*record.poolOffset, record.poolBytes);
            useTextures(s, *record.geometry, false, frame, result);
            result.releasedModels.push_back(it->first);
            it = s.models.erase(it);
            continue;
        }

        // Made once first placed; a model larger than a frame's upload never is.
        const auto needed = modelBytes(*record.geometry);
        if (record.lastPlaced == frame.frameIndex && !record.blas && needed && needed <= gi_scene::GeometryUploadBytes)
        {
            // What has no room this frame waits for the next; an upload whose structure has no room
            // lands in the range given back, which nothing uses before this frame's copy.
            const auto offset = needed <= s.scene->uploadRoom() ? s.scene->allocate(needed) : std::nullopt;
            const auto geometries = offset ? stageModel(s, *record.geometry, *offset, bytes) : std::vector<gi_scene::Geometry>{};
            const auto blas = offset && s.scene->upload(*offset, bytes) ? s.scene->createBlas(gi_scene::Use::Static, geometries) : std::nullopt;
            if (blas)
            {
                record.poolOffset = offset;
                record.poolBytes = needed;
                record.blas = blas;
            }
            else
            {
                if (offset) s.scene->release(*offset, needed);
                ++s.modelsWaiting;
            }
        }
        else if (needed > gi_scene::GeometryUploadBytes && record.lastPlaced == frame.frameIndex && !record.blas)
            ++s.modelsTooLarge;

        ++it;
    }
}

// The frame's terrain rings: where each lies, and which to make again with a new structure, or with
// the old one refitted, this frame (`remade`). Each ring's place goes into the frame's constants.
void planTerrain(State& s, const Frame& frame, std::array<bool, GI_TERRAIN_RINGS>& remade)
{
    bool innerMoved{};
    for (std::uint32_t r = 0; r < GI_TERRAIN_RINGS; ++r)
    {
        auto& ring = s.terrain[r];
        const float cell = TerrainCellSize * std::pow(4.0f, static_cast<float>(r));
        const float step = cell * 8;
        const std::array<float, 2> corner{std::floor((frame.eye[0] - cell * GI_TERRAIN_CELLS / 2) / step) * step,
            std::floor((frame.eye[2] - cell * GI_TERRAIN_CELLS / 2) / step) * step};
        const bool moved = !ring.placed || corner != ring.corner || innerMoved || !s.scene->ready(ring.blas);
        if (moved)
        {
            ring.corner = corner;
            ring.placed = true;
            ring.refreshed = frame.frameIndex;
            remade[r] = s.scene->rebuild(ring.blas);
        }
        else if (frame.frameIndex - ring.refreshed > TerrainRefreshFrames + r * TerrainRefreshFrames / GI_TERRAIN_RINGS)
        {
            ring.refreshed = frame.frameIndex;
            remade[r] = s.scene->refit(ring.blas);
        }

        innerMoved = moved;
    }
}

struct Uploads
{
    std::uint8_t* data{};
    D3D12_GPU_VIRTUAL_ADDRESS address{};
    std::uint64_t used{};

    // Room for `count` elements of `stride` bytes, aligned for a constant buffer and a structured view.
    std::uint64_t reserve(std::uint64_t stride, std::uint64_t count)
    {
        used = (used + 255) / 256 * 256;
        used = (used + stride - 1) / stride * stride;
        const auto offset = used;
        used += stride * std::max<std::uint64_t>(count, 1);
        if (used > UploadBytes) throw std::runtime_error("The global illumination's upload buffer is full.");

        return offset;
    }
};

// Gauss-Jordan with partial pivoting; projection-views are invertible.
Matrix inverse(const Matrix& input)
{
    std::array<double, 16> matrix{}, result{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    for (int i = 0; i < 16; ++i) matrix[i] = input[i];

    for (int column = 0; column < 4; ++column)
    {
        int pivot = column;
        for (int row = column + 1; row < 4; ++row)
            if (std::abs(matrix[row * 4 + column]) > std::abs(matrix[pivot * 4 + column])) pivot = row;

        for (int k = 0; k < 4; ++k)
        {
            std::swap(matrix[column * 4 + k], matrix[pivot * 4 + k]);
            std::swap(result[column * 4 + k], result[pivot * 4 + k]);
        }

        const double scale = matrix[column * 4 + column] != 0 ? 1 / matrix[column * 4 + column] : 0;
        for (int k = 0; k < 4; ++k)
        {
            matrix[column * 4 + k] *= scale;
            result[column * 4 + k] *= scale;
        }

        for (int row = 0; row < 4; ++row)
        {
            if (row == column) continue;

            const double factor = matrix[row * 4 + column];
            for (int k = 0; k < 4; ++k)
            {
                matrix[row * 4 + k] -= factor * matrix[column * 4 + k];
                result[row * 4 + k] -= factor * result[column * 4 + k];
            }
        }
    }

    Matrix single{};
    for (int i = 0; i < 16; ++i) single[i] = static_cast<float>(result[i]);

    return single;
}

void copyMatrix(float4x4& to, const Matrix& from)
{
    std::memcpy(to.m, from.data(), sizeof(to.m));
}

GIFrameConstants constantsOf(const Frame& frame, std::uint32_t scatterCount, std::uint32_t instanceCount)
{
    GIFrameConstants constants{};
    copyMatrix(constants.viewProjection, frame.viewProjection);
    copyMatrix(constants.viewProjectionInverse, inverse(frame.viewProjection));
    copyMatrix(constants.previousViewProjection, frame.previousViewProjection);
    copyMatrix(constants.previousViewProjectionInverse, inverse(frame.previousViewProjection));
    constants.eye = {frame.eye[0], frame.eye[1], frame.eye[2], 0};
    constants.previousEye = {frame.previousEye[0], frame.previousEye[1], frame.previousEye[2], 0};
    constants.sunDirection = {frame.sunDirection[0], frame.sunDirection[1], frame.sunDirection[2], SunRadius};
    constants.sunColor = {frame.sunColor[0], frame.sunColor[1], frame.sunColor[2], 0};
    for (int i = 0; i < 9; ++i) constants.skySH[i] = {frame.skyRadiance[i][0], frame.skyRadiance[i][1], frame.skyRadiance[i][2], 0};
    constants.dimensions = {frame.width, frame.height, frame.frameIndex, instanceCount};
    constants.nearFar = {frame.nearPlane, frame.farPlane, DefaultAlbedo, frame.outputScale};
    constants.counts = {scatterCount, scatterCount, frame.showScene ? GI_VIEW_SCENE : GI_VIEW_OFF, 0};
    constants.shadowRange = {frame.shadowRange[0], frame.shadowRange[1], frame.shadowRange[2], frame.shadowRange[3]};
    constants.shadowParams = {frame.sunTangent, AlphaTestedBlocking, ShadowRayLength, 0};
    constants.rayCone = {2 * std::tan(frame.verticalFov / 2) / static_cast<float>(std::max(frame.height, 1u)), 0, 0, 0};

    return constants;
}

std::uint32_t groups(std::uint64_t threads, std::uint32_t size)
{
    return static_cast<std::uint32_t>((threads + size - 1) / size);
}
}

void start(ID3D11Device* device11, HANDLE sharedFence)
{
    if (state) return;

    auto s = std::make_unique<State>();
    ComPtr<IDXGIDevice> dxgi;
    check(device11->QueryInterface(IID_PPV_ARGS(&dxgi)), "Finding the DXGI device");
    ComPtr<IDXGIAdapter> adapter;
    check(dxgi->GetAdapter(adapter.GetAddressOf()), "Finding the GPU");
    ComPtr<ID3D12Device> created, native;
    check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&created)), "Creating the Direct3D 12 device");
    if (SUCCEEDED(created->QueryInterface(UnwrappedObject, reinterpret_cast<void**>(native.GetAddressOf())))) s->wrapper = created;
    else native = created;

    check(native.As(&s->device), "Finding Direct3D 12 ray tracing");
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
    s->device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options, sizeof(options));
    if (options.RaytracingTier < D3D12_RAYTRACING_TIER_1_1) throw std::runtime_error("The GPU or its driver cannot trace rays from compute shaders (DXR 1.1).");

    const D3D12_COMMAND_QUEUE_DESC queue{D3D12_COMMAND_LIST_TYPE_COMPUTE};
    check(s->device->CreateCommandQueue(&queue, IID_PPV_ARGS(&s->queue)), "Creating the Direct3D 12 queue");
    for (auto& allocator : s->allocators)
        check(s->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&allocator)), "Creating a command allocator");

    check(s->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, s->allocators[0].Get(), nullptr, IID_PPV_ARGS(&s->list)),
        "Creating the command list");
    check(s->list->Close(), "Closing the command list");
    check(s->device->OpenSharedHandle(sharedFence, IID_PPV_ARGS(&s->fence)), "Opening the shared fence");
    s->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!s->event) throw std::runtime_error("Creating the fence event failed.");

    createPipelines(*s);
    createResources(*s);

    state = std::move(s);
    log("Global illumination started on Direct3D 12.");
}

void stop() noexcept
{
    if (!state) return;

    try
    {
        waitFor(*state, *std::max_element(state->submitted.begin(), state->submitted.end()));
    }
    catch (const std::exception& error)
    {
        log(error.what());
    }

    state.reset();
    log("Global illumination stopped.");
}

bool started() noexcept
{
    return state != nullptr;
}

// Diagnostics: the output light's level, logged once its copy is back.
void logStats(State& s)
{
    if (!s.statsFence || s.fence->GetCompletedValue() < s.statsFence) return;

    s.statsFence = 0;
    void* data{};
    if (FAILED(s.statsReadback->Map(0, nullptr, &data))) return;

    const auto& footprint = s.statsFootprint.Footprint;
    std::vector<float> luminance;
    double sum[3]{};
    for (UINT y = 0; y < footprint.Height; y += 8)
    {
        const auto* row = reinterpret_cast<const std::uint16_t*>(static_cast<const std::uint8_t*>(data) + std::uint64_t{y} * footprint.RowPitch);
        for (UINT x = 0; x < footprint.Width; x += 8)
        {
            float rgb[3]{};
            for (int c = 0; c < 3; ++c)
            {
                // Half to float.
                const std::uint32_t h = row[x * 4 + c];
                const std::uint32_t sign = (h >> 15) & 1, exponent = (h >> 10) & 31, mantissa = h & 1023;
                float value = exponent == 0 ? std::ldexp(static_cast<float>(mantissa), -24)
                    : exponent == 31 ? 65504.0f : std::ldexp(static_cast<float>(mantissa | 1024), static_cast<int>(exponent) - 25);
                rgb[c] = sign ? -value : value;
                sum[c] += rgb[c];
            }

            luminance.push_back(0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2]);
        }
    }

    s.statsReadback->Unmap(0, nullptr);
    if (luminance.empty()) return;

    // On the game's render thread: one partial sort a share, not a whole sort.
    const auto at = [&](double share) {
        const auto nth = luminance.begin() + static_cast<std::ptrdiff_t>(share * static_cast<double>(luminance.size() - 1));
        std::nth_element(luminance.begin(), nth, luminance.end());
        return std::to_string(*nth);
    };
    const auto count = static_cast<double>(luminance.size());
    log("Global illumination's light (luminance): 1% " + at(0.01) + ", 10% " + at(0.1) + ", median " + at(0.5) + ", 90% " + at(0.9) + ", 99% "
        + at(0.99) + ", max " + at(1.0) + "; mean rgb " + std::to_string(sum[0] / count) + " " + std::to_string(sum[1] / count) + " "
        + std::to_string(sum[2] / count) + ".");

    void* mapped{};
    if (FAILED(s.statsCounters->Map(0, nullptr, &mapped))) return;

    std::uint32_t counters[GI_COUNTER_COUNT]{};
    std::memcpy(counters, mapped, sizeof(counters));
    s.statsCounters->Unmap(0, nullptr);
    const auto value = [&](std::uint32_t counter) { return std::to_string(counters[counter]); };
    log("Global illumination's rays: " + std::to_string(counters[GI_COUNTER_COUNT - 1] * 64) + " from " + value(GI_COUNTER_COUNT - 1) + " probes, "
        + value(GI_COUNTER_HITS) + " hits (" + value(GI_COUNTER_NEAR_HITS) + " within 25 cm, " + value(GI_COUNTER_FULL_BUCKETS)
        + " without room in the cache), " + value(GI_COUNTER_VISIBILITY1) + " second bounces, " + value(GI_COUNTER_LIGHTS) + " hits lit, "
        + value(GI_COUNTER_COPIES) + " copies of a surface skipped, " + value(GI_COUNTER_UPDATE_TILES) + " cache tiles updated.");

    constexpr const char* MaximumNames[GI_COUNTER_MAXIMA]{"a hit's albedo", "a hit's direct light", "a cell's direct light", "a cell's indirect light",
        "a second bounce", "a probe ray", "a probe cell", "a pixel from the probes", "a denoised pixel"};
    std::string maxima;
    for (std::uint32_t i = 0; i < GI_COUNTER_MAXIMA; ++i)
        maxima += std::string(i ? ", " : "") + MaximumNames[i] + " " + std::to_string(std::bit_cast<float>(counters[GI_COUNTER_MAXIMA_FIRST + i]));
    log("Global illumination's largest values in that frame (before the exposure; the lights line gives the sun's and sky's): " + maxima + ".");
}

// The phases a frame slot timed, once the GPU is past them: into the averages and the tally.
void readTimes(State& s, std::uint32_t slot)
{
    auto& marks = s.marks[slot];
    if (marks.size() < 2 || !s.timestampFrequency) return;

    void* data{};
    const D3D12_RANGE range{std::size_t{slot} * MaxMarks * 8, std::size_t{slot + 1} * MaxMarks * 8};
    if (FAILED(s.timestampReadback->Map(0, &range, &data))) return;

    const auto* ticks = reinterpret_cast<const std::uint64_t*>(static_cast<const std::uint8_t*>(data) + range.Begin);
    const double toMilliseconds = 1000.0 / static_cast<double>(s.timestampFrequency);
    double scene{}, light{}, shadow{};
    for (std::size_t i = 0; i + 1 < marks.size(); ++i)
    {
        const double milliseconds = ticks[i + 1] >= ticks[i] ? static_cast<double>(ticks[i + 1] - ticks[i]) * toMilliseconds : 0.0;
        // The scene's phases come first, up to the top level; the shadows' are named so.
        (i < 3 ? scene : std::strncmp(marks[i], "shadow", 6) == 0 ? shadow : light) += milliseconds;
        auto& phases = s.tally.phases;
        const auto found = std::find_if(phases.begin(), phases.end(), [&](const auto& phase) { return std::strcmp(phase.first, marks[i]) == 0; });
        if (found != phases.end()) found->second += milliseconds;
        else phases.emplace_back(marks[i], milliseconds);
    }

    const D3D12_RANGE none{};
    s.timestampReadback->Unmap(0, &none);
    marks.clear();
    s.sceneMilliseconds += (scene - s.sceneMilliseconds) * 0.05;
    s.lightMilliseconds += (light - s.lightMilliseconds) * 0.05;
    s.shadowMilliseconds += (shadow - s.shadowMilliseconds) * 0.05;
    ++s.tally.timedFrames;
}

const char* reasonName(std::uint32_t reason)
{
    switch (reason)
    {
    case GI_REASON_DRAWN: return "in the scene";
    case GI_REASON_OUT_OF_VIEW: return "in the scene, kept: not drawn, not all of it in view";
    case GI_REASON_BEHIND: return "in the scene, kept: not drawn, all in view but behind what the game drew";
    case GI_REASON_LEFT_OUT: return "LEFT OUT: not drawn, all in view, and its place on screen shows";
    default: return "no instance";
    }
}

std::string entryState(std::uint32_t bits)
{
    if (!(bits & GI_ENTRY_LIVE)) return "not live";

    std::string text = !(bits & GI_ENTRY_BUILT) ? "live, no acceleration structure yet"
        : !(bits & GI_ENTRY_INSTANCED) ? "live, built, past the instance limit" : "live, built, instanced";
    if (bits & GI_ENTRY_ALPHA_TESTED) text += ", alpha-tested";

    return text + ", last drawn " + std::to_string((bits >> 8) & 0xFFFu) + " frames ago, drawn in " + std::to_string(bits >> 20) + " frames";
}

std::string percentOf(std::uint64_t used, std::uint64_t capacity)
{
    return std::to_string(capacity ? used * 100 / capacity : 0) + "%";
}

// A hit the probe lists (GIShadowProbe::lightNearest) as text: what it hit, which side, how far.
std::string describeHit(const State& s, std::span<const InstancedModel> instancedModels, const uint4& hit)
{
    if (!hit.x) return "nothing";

    const auto instance = hit.x - 1, geometry = hit.y & 0x7FFFFFFFu, kind = hit.z;
    std::string what = (kind & GI_INSTANCE_TERRAIN) ? "the terrain" : (kind & GI_INSTANCE_DYNAMIC) ? "the skinned meshes" : "a captured mesh";
    if (kind & GI_INSTANCE_MODEL)
    {
        const auto placed = std::lower_bound(instancedModels.begin(), instancedModels.end(), instance,
            [](const InstancedModel& model, std::uint32_t value) { return model.instance < value; });
        if (placed == instancedModels.end() || placed->instance != instance) what = "one of the game's models";
        else
        {
            what = std::format("model {:x} placed at ({:.1f} {:.1f} {:.1f})", placed->hash, placed->position[0], placed->position[1], placed->position[2]);
            const auto found = s.models.find(placed->hash);
            if (found != s.models.end() && geometry < found->second.geometry->parts.size())
            {
                const auto& part = found->second.geometry->parts[geometry];
                what += std::format(", mesh {} ({}, {} triangles)", part.mesh, part.alphaTested ? "cut-out" : "opaque", part.indices.size() / 3);
            }
        }
    }

    return std::format("{} at {:.2f} m, its {} face (instance {})", what, std::bit_cast<float>(hit.w), (hit.y >> 31) ? "front" : "back", instance);
}

// The probe as text, and a key that changes when what blocks its ray changes.
std::pair<std::string, std::string> describeProbe(const State& s, std::span<const InstancedModel> instancedModels, const GIShadowProbe& probe,
    std::vector<uint4> crossings)
{
    const auto& r = probe.reasons;
    const auto stats = s.scene->stats();
    std::uint32_t evicted{};
    for (const auto count : s.evictedByFrame) evicted += count;
    const auto scene = std::format("scene: {} instances: {} in the scene, {} captured kept out of view, {} kept behind what the game drew, {} left out; "
        "geometry pool {}, structures {}, {} structures waiting to be built; {} captured meshes evicted in the last {} frames",
        r.x + r.y + r.z + r.w, r.x, r.y, r.z, r.w, percentOf(stats.poolUsed, stats.poolCapacity), percentOf(stats.blasUsed, stats.blasCapacity),
        stats.waiting, evicted, s.evictedByFrame.size());
    if (probe.receiver.w <= 0) return {"centre pixel: the sky\n" + scene, "sky"};

    std::string text = std::format("centre pixel: surface {:.1f} m away at ({:.1f} {:.1f} {:.1f}), normal ({:.2f} {:.2f} {:.2f}), {:.2f} beyond the "
        "shadow map (0 inside, 1 past it); shadow there {:.2f}, this frame's ray {}\n",
        probe.normal.w, probe.receiver.x, probe.receiver.y, probe.receiver.z, probe.normal.x, probe.normal.y, probe.normal.z, probe.ray.w,
        probe.shadow.x, probe.shadow.y > 0.5f ? "lit" : "blocked");

    const auto& h = probe.hits;
    text += std::format("sun ray towards ({:.2f} {:.2f} {:.2f}): near segment to {:.2f} m (copy tolerance {:.2f} m), {} copies of the surface skipped; "
        "the shadow rays {}\n", probe.ray.x, probe.ray.y, probe.ray.z, probe.segments.x, probe.segments.y, h.x,
        h.y ? std::format("are blocked near, at {:.2f} m (instance {})", probe.segments.z, h.y - 1)
            : h.z ? std::format("are blocked at {:.1f} m (instance {})", probe.segments.w, h.z - 1)
                  : std::string("reach the sun"));

    std::string key = std::format("{}|{}", h.y || h.z ? "blocked" : "lit", h.w ? std::to_string(probe.hitEntry.x) : "-");
    if (h.w)
    {
        const auto& e = probe.hitEntry;
        const auto kind = e.z & 0xFFFFu, flags = e.z >> 16;
        const bool captured = (kind & (GI_INSTANCE_CAPTURED | GI_INSTANCE_DYNAMIC)) != 0;
        const auto what = (kind & GI_INSTANCE_MODEL) ? std::format("one of the game's models, geometry {}", e.x)
            : (kind & GI_INSTANCE_TERRAIN) ? std::format("the terrain, geometry {}", e.x)
            : (kind & GI_INSTANCE_DYNAMIC) ? std::string("the skinned meshes")
                                           : std::format("captured entry {}", e.x);
        text += std::format("with every instance: hits {} ({}{}), {}", what, (flags & GI_GEOMETRY_CUT_OUT) ? "cut-out" : "opaque",
            (flags & GI_GEOMETRY_ALPHA_TEXTURE) ? " with the game's alpha" : "", reasonName(e.y));
        if (probe.hitBox.w > 0.5f)
            text += std::format(" (its box's nearest depth {:.6f}, the farthest drawn over its rectangle {:.6f} at pyramid level {:.0f})", probe.hitBox.x,
                probe.hitBox.y, probe.hitBox.z);
        if (kind & GI_INSTANCE_TERRAIN)
            text += std::format("; its triangle's highest corner at {:.1f} m as its ring was made, the heightmap {:.1f} m there now (level of detail {:.0f})",
                probe.terrain.x, probe.terrain.y, probe.terrain.z);
        if (captured) text += "; entry " + entryState(e.w);
        text += "\n";
        key += std::format("|{}", e.y);
    }
    else
        text += "with every instance: nothing in the scene blocks it\n";

    const auto& g = probe.lightRays;
    text += std::format("its global illumination rays ({}): {} hit within 1 m, {} within 10 m, {} get away, {} hit a back face; the nearest hit: {}\n",
        GI_PROBE_LIGHT_RAYS, g.x, g.y, g.z, g.w, describeHit(s, instancedModels, probe.lightNearest));
    text += std::format("the camera's ray (the scene view's): {}; the game's surface at {:.2f} m\n", describeHit(s, instancedModels, probe.view), probe.normal.w);
    key += std::format("|{}", probe.view.x);

    text += std::format("frame {}; the terrain rings last made at frames", static_cast<std::uint32_t>(probe.terrain.w));
    for (const auto& ring : s.terrain) text += " " + std::to_string(ring.refreshed);
    text += "\n";

    // The captured meshes whose bounds the ray crosses, nearest first, past the receiver's own.
    std::sort(crossings.begin(), crossings.end(), [](const uint4& a, const uint4& b) { return std::bit_cast<float>(a.y) < std::bit_cast<float>(b.y); });
    std::uint32_t inScene{}, leftOut{}, unbuilt{}, uninstanced{};
    for (const auto& c : crossings)
    {
        if (!(c.z & GI_ENTRY_BUILT)) ++unbuilt;
        else if (!(c.z & GI_ENTRY_INSTANCED)) ++uninstanced;
        else if (c.w == GI_REASON_LEFT_OUT) ++leftOut;
        else ++inScene;
    }

    text += std::format("bounds crossed: {} captured meshes{}: {} in the scene, {} left out, {} without a structure yet, {} past the instance limit",
        probe.crossings.x, probe.crossings.x > crossings.size() ? std::format(" ({} listed)", crossings.size()) : "", inScene, leftOut, unbuilt, uninstanced);
    std::uint32_t shown{};
    for (const auto& c : crossings)
    {
        const float enter = std::bit_cast<float>(c.y);
        if (enter < 0.5f || shown == 8) continue;

        text += std::format("{} entry {} at {:.1f} m ({})", shown++ ? ";" : "; nearest past 0.5 m:", c.x, enter,
            !(c.z & GI_ENTRY_BUILT) ? "no structure yet" : !(c.z & GI_ENTRY_INSTANCED) ? "past the instance limit" : reasonName(c.w));
    }

    // The game's models whose world bounds the ray crosses, past the receiver: whether the renderer has
    // them, and each mesh.
    struct Crossed
    {
        float enter;
        const ModelInstance* placement;
    };
    std::vector<Crossed> crossed;
    const float origin[3]{probe.receiver.x, probe.receiver.y, probe.receiver.z}, direction[3]{probe.ray.x, probe.ray.y, probe.ray.z};
    std::uint32_t unknown{};
    for (const auto& placement : s.modelInstances)
    {
        const auto found = s.models.find(placement.hash);
        if (found == s.models.end())
        {
            ++unknown;
            continue;
        }

        // A model all of whose meshes were left out has nothing to cross.
        if (found->second.geometry->parts.empty()) continue;

        // The box's corners through the transform, then the slab test.
        const auto& b = found->second.bounds;
        float lowest[3]{FLT_MAX, FLT_MAX, FLT_MAX}, highest[3]{-FLT_MAX, -FLT_MAX, -FLT_MAX};
        for (int corner = 0; corner < 8; ++corner)
        {
            const float local[3]{b[(corner & 1) ? 3 : 0], b[(corner & 2) ? 4 : 1], b[(corner & 4) ? 5 : 2]};
            for (int row = 0; row < 3; ++row)
            {
                const auto* t = &placement.transform[row * 4];
                const float world = t[0] * local[0] + t[1] * local[1] + t[2] * local[2] + t[3];
                lowest[row] = std::min(lowest[row], world);
                highest[row] = std::max(highest[row], world);
            }
        }

        float enter = 0, leave = ShadowRayLength;
        for (int axis = 0; axis < 3 && enter <= leave; ++axis)
        {
            if (std::abs(direction[axis]) < 1e-8f)
            {
                if (origin[axis] < lowest[axis] || origin[axis] > highest[axis]) enter = leave + 1;
                continue;
            }

            float t0 = (lowest[axis] - origin[axis]) / direction[axis], t1 = (highest[axis] - origin[axis]) / direction[axis];
            if (t0 > t1) std::swap(t0, t1);
            enter = std::max(enter, t0);
            leave = std::min(leave, t1);
        }

        if (enter <= leave) crossed.push_back({enter, &placement});
    }

    std::sort(crossed.begin(), crossed.end(), [](const Crossed& a, const Crossed& b) { return a.enter < b.enter; });
    text += std::format("\nthe game's models whose bounds the ray crosses: {}{}", crossed.size(),
        unknown ? std::format(" ({} placements of models not read yet)", unknown) : "");
    for (std::size_t i = 0; i < crossed.size() && i < 8; ++i)
    {
        const auto& record = s.models.at(crossed[i].placement->hash);
        text += std::format("{} {:x} at {:.1f} m ({}):", i ? ";" : ":", crossed[i].placement->hash, crossed[i].enter,
            !record.blas ? "waiting for room" : !s.scene->ready(*record.blas) ? "waiting for its structure" : "in the scene");
        for (const auto& part : record.geometry->parts)
        {
            const auto texture = s.textures.find(part.baseTexture);
            const bool textured = !part.uvs.empty() && texture != s.textures.end() && texture->second.shared.resource;
            text += std::format(" mesh {} {}, {} {} triangles", part.mesh, part.alphaTested ? "cut-out" : "opaque",
                !part.baseTexture ? "no base texture" : textured ? "its texture copied" : "its texture not copied yet", part.indices.size() / 3);
        }
    }

    key += std::format("|{}", crossed.empty() ? 0 : crossed.front().placement->hash);
    return {text + "\n" + scene, key};
}

// The probe the slot's last frame wrote, once the GPU is past it: into the overlay's text, and the
// log when what blocks its ray changed (at most five times a second).
void readProbe(State& s, std::uint32_t slot)
{
    if (!s.probeWritten[slot]) return;

    s.probeWritten[slot] = false;
    void* data{};
    if (FAILED(s.probeReadback[slot]->Map(0, nullptr, &data))) return;

    GIShadowProbe probe{};
    std::memcpy(&probe, data, sizeof(probe));
    std::vector<uint4> crossings(std::min(probe.crossings.y, GI_PROBE_CROSSINGS));
    std::memcpy(crossings.data(), static_cast<const std::uint8_t*>(data) + sizeof(probe), crossings.size() * sizeof(uint4));
    const D3D12_RANGE none{};
    s.probeReadback[slot]->Unmap(0, &none);

    auto [text, key] = describeProbe(s, s.instancedModels[slot], probe, std::move(crossings));
    const auto now = GetTickCount64();
    std::lock_guard lock(probeMutex);
    probeText = text;
    if (key != lastProbeKey && now - lastProbeLog >= 200)
    {
        lastProbeKey = key;
        lastProbeLog = now;
        log("Shadow probe:\n" + text);
    }
}

// Diagnostics: every ten seconds, what a frame cost on average.
void logTally(State& s)
{
    auto& t = s.tally;
    const auto now = GetTickCount64();
    if (!t.since) t.since = now;
    if (now - t.since < 10'000 || !t.frames) return;

    const auto milliseconds = [](double value) {
        auto text = std::to_string(value);
        return text.substr(0, text.find('.') + 4);
    };

    std::string line = "Global illumination's GPU time a frame (Direct3D 12):";
    double total{};
    for (const auto& [phase, sum] : t.phases)
    {
        const double perFrame = t.timedFrames ? sum / t.timedFrames : 0.0;
        line += std::string(" ") + phase + " " + milliseconds(perFrame) + ",";
        total += perFrame;
    }

    std::string steps;
    for (std::size_t step = 0; step < CpuSteps; ++step)
        steps += std::string(step ? ", " : "") + CpuStepNames[step] + " " + milliseconds(t.cpuSteps[step] / t.frames);

    log(line + " total " + milliseconds(total) + " ms. Bottom-level structures a frame: " + std::to_string(t.builds / t.frames) + " built ("
        + std::to_string(t.builtTriangles / t.frames / 1000) + "k triangles), at most " + std::to_string(t.mostBuilds) + " in a frame, "
        + std::to_string(t.waiting / t.frames) + " waiting, " + std::to_string(t.compactions) + " compacted in all; skinned meshes: "
        + std::to_string(t.skinnedTriangles / t.frames / 1000) + "k triangles. CPU a frame: " + milliseconds(t.cpuMilliseconds / t.frames)
        + " ms (" + steps + "), at most " + milliseconds(t.mostCpuMilliseconds) + " ms.");
    t = {};
    t.since = now;
}

Result render(const Frame& frame)
{
    auto& s = *state;
    LARGE_INTEGER started{}, frequency{};
    QueryPerformanceCounter(&started);
    QueryPerformanceFrequency(&frequency);

    auto lapStarted = started;
    const auto lap = [&](CpuStep step) {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        s.tally.cpuSteps[step] += static_cast<double>(now.QuadPart - lapStarted.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
        lapStarted = now;
    };

    Result result;
    const auto slot = static_cast<std::uint32_t>(s.frames++ % FramesInFlight);
    waitFor(s, s.submitted[slot]);
    lap(WaitStep);
    auto& scene = *s.scene;
    scene.beginFrame(slot, s.fence->GetCompletedValue(), frame.signalValue);
    logStats(s);
    readTimes(s, slot);
    readProbe(s, slot);
    lap(ProbeStep);
    logTally(s);

    // A new render size starts the light and the shadows over. Each has memory only once traced;
    // the shadows give theirs back when turned off.
    const auto waitForAll = [&] { waitFor(s, *std::max_element(s.submitted.begin(), s.submitted.end())); };
    if (frame.traceLight && (!s.light || s.light->width() != frame.width || s.light->height() != frame.height))
    {
        waitForAll();
        s.light.reset();
        s.light = std::make_unique<gi_light::Light>(s.device.Get(), frame.width, frame.height);
        log("Global illumination's light at " + std::to_string(frame.width) + "x" + std::to_string(frame.height) + ".");
    }

    if (frame.traceShadows && (!s.shadows || s.shadows->width() != frame.width || s.shadows->height() != frame.height))
    {
        waitForAll();
        s.shadows.reset();
        s.shadows = std::make_unique<gi_shadow::Shadows>(s.device.Get(), frame.width, frame.height);
        log("Ray-traced shadows at " + std::to_string(frame.width) + "x" + std::to_string(frame.height) + ".");
    }
    else if (!frame.traceShadows && s.shadows)
    {
        waitForAll();
        s.shadows.reset();
    }

    if (s.pyramidWidth != (frame.width + 1) / 2 || s.pyramidHeight != (frame.height + 1) / 2)
    {
        waitForAll();
        s.pyramidWidth = (frame.width + 1) / 2;
        s.pyramidHeight = (frame.height + 1) / 2;
        s.pyramidLevels = static_cast<UINT>(std::min<std::size_t>(std::bit_width(std::max(s.pyramidWidth, s.pyramidHeight)), MaxPyramidLevels));
        s.depthPyramid = createTexture(s.device.Get(), s.pyramidWidth, s.pyramidHeight, DXGI_FORMAT_R32_FLOAT, static_cast<UINT16>(s.pyramidLevels),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "Creating the depth pyramid");
    }

    const bool traceLight = frame.traceLight && s.light;
    const bool traceShadows = frame.traceShadows && s.shadows && frame.textures.shadow;
    const bool debugShadows = traceShadows && frame.debugShadows && frame.textures.shadowDebug;
    if (!debugShadows)
    {
        std::lock_guard lock(probeMutex);
        probeText.clear();
    }

    // Captured meshes the frame drew stay; eviction takes others first.
    for (const auto index : frame.drawn)
        if (index < DynamicEntry && s.entries[index].live && s.entries[index].lastDrawn != frame.frameIndex)
        {
            s.entries[index].lastDrawn = frame.frameIndex;
            ++s.entries[index].framesDrawn;
        }

    // ---- The game's models ----
    updateModels(s, frame, result);
    lap(ModelsStep);

    // ---- The frame's captures into the pool ----
    std::vector<GIScatterRecord> scatter;
    std::uint32_t scatterVertices{}, skinnedVertices{};
    bool evictedVertices{}, evictedStructures{};
    for (const auto& capture : frame.captures)
    {
        if (!capture.vertexCount) continue;

        // The skinned meshes follow one another in their room.
        if (capture.dynamic)
        {
            if (skinnedVertices + capture.vertexCount > SkinnedVertices) continue;

            scatter.push_back({DynamicEntry, capture.transferOffset, capture.vertexCount, static_cast<std::uint32_t>(s.skinnedOffset + std::uint64_t{skinnedVertices} * 16), 0, 0, {}});
            scatterVertices = std::max(scatterVertices, capture.transferOffset + capture.vertexCount);
            skinnedVertices += capture.vertexCount;
            continue;
        }

        if (capture.entry >= DynamicEntry) continue;

        auto& entry = s.entries[capture.entry];
        if (!entry.live)
        {
            entry = {};
            entry.live = true;
            entry.framesDrawn = 1;
        }

        entry.lastDrawn = frame.frameIndex;
        entry.alphaTested |= capture.alphaTested;
        if (!entry.hasVertices || entry.vertexCount != capture.vertexCount)
        {
            if (entry.hasVertices) scene.release(entry.poolOffset, std::uint64_t{entry.vertexCount} * 16);
            entry.hasVertices = false;
            const auto offset = scene.allocate(std::uint64_t{capture.vertexCount} * 16);
            if (!offset)
            {
                evictForRoom(s, false, std::uint64_t{capture.vertexCount} * 16, frame, result.evicted, evictedVertices);
                evict(s, capture.entry, result.evicted);
                continue;
            }

            entry.poolOffset = *offset;
            entry.vertexCount = capture.vertexCount;
            entry.hasVertices = true;
        }

        // Records adjoin in the transfer texture, in the order they came.
        scatter.push_back({capture.entry, capture.transferOffset, capture.vertexCount, static_cast<std::uint32_t>(entry.poolOffset), 0, 0, {}});
        scatterVertices = std::max(scatterVertices, capture.transferOffset + capture.vertexCount);
        entry.needsBlas = true;
    }

    for (const auto index : frame.alphaTested)
        if (index < DynamicEntry && s.entries[index].live && !s.entries[index].alphaTested)
        {
            s.entries[index].alphaTested = true;
            s.entries[index].needsBlas = s.entries[index].hasVertices;
        }

    // A captured mesh's structure is made again with its vertices, and with its cut-out flag; those not
    // drawn for RetentionFrames leave.
    std::uint32_t live{};
    for (std::uint32_t index = 0; index < DynamicEntry; ++index)
    {
        auto& entry = s.entries[index];
        if (!entry.live) continue;

        if (frame.frameIndex - entry.lastDrawn > RetentionFrames)
        {
            evict(s, index, result.evicted);
            continue;
        }

        ++live;
        if (!entry.needsBlas) continue;

        if (entry.blas) scene.destroyBlas(*entry.blas);
        entry.blas = scene.createBlas(gi_scene::Use::Static,
            {{entry.poolOffset, entry.vertexCount, 0, 0, entry.alphaTested ? GI_GEOMETRY_CUT_OUT : 0u}});
        entry.needsBlas = false;
        if (!entry.blas)
        {
            evictForRoom(s, true, 0, frame, result.evicted, evictedStructures);
            evict(s, index, result.evicted);
        }
    }

    // ---- The game's terrain, and the skinned meshes ----
    std::array<bool, GI_TERRAIN_RINGS> remadeRing{};
    const bool terrain = frame.terrain.has_value();
    if (terrain) planTerrain(s, frame, remadeRing);
    scene.buildFrame(s.skinnedBlas, skinnedVertices);

    // New geometry starts what was learnt about it over (ClearBounds); its records scatter no
    // vertices, so they lie past the captures'.
    for (const auto row : scene.takeNewGeometries()) scatter.push_back({0, scatterVertices, 0, 0, row, GI_SCATTER_NEW, {}});

    scene.planBuilds();

    check(s.allocators[slot]->Reset(), "Resetting a command allocator");
    check(s.list->Reset(s.allocators[slot].Get(), nullptr), "Resetting the command list");
    auto& list = *s.list.Get();
    try
    {
        auto* depth = openShared(s, s.depth, frame.textures.depth, "Opening the depth").Get();
        auto* normal = openShared(s, s.normal, frame.textures.normal, "Opening the normals").Get();
        auto* albedo = openShared(s, s.albedo, frame.textures.albedo, "Opening the albedo").Get();
        auto* transfer = openShared(s, s.transfer, frame.textures.transfer, "Opening the transfer texture").Get();
        auto* output = openShared(s, s.output, frame.textures.output, "Opening the output").Get();
        auto* shadow = traceShadows ? openShared(s, s.shadow, frame.textures.shadow, "Opening the shadow").Get() : nullptr;
        auto* shadowDebug = debugShadows ? openShared(s, s.shadowDebug, frame.textures.shadowDebug, "Opening the shadow debug view").Get() : nullptr;
        auto* terrainPages = terrain ? openShared(s, s.terrainPages, frame.terrain->pages, "Opening the terrain's page table").Get() : nullptr;
        auto* terrainHeights = terrain ? openShared(s, s.terrainHeights, frame.terrain->heights, "Opening the terrain's heights").Get() : nullptr;

        // ---- The frame's instances: the skinned meshes, the game's models where the game has them
        // (drawn this frame or not), the terrain, the captured meshes ----
        std::uint64_t triangles{};
        if (debugShadows) std::fill(s.entryInstance.begin(), s.entryInstance.end(), GI_REASON_NONE);
        if (const auto instance = scene.addInstance(s.skinnedBlas, nullptr, GI_INSTANCE_DYNAMIC | GI_INSTANCE_DRAWN | (DynamicEntry << GI_INSTANCE_ENTRY_SHIFT)))
        {
            s.entryInstance[DynamicEntry] = *instance;
            triangles += skinnedVertices / 3;
        }

        std::uint32_t modelInstances{};
        std::uint64_t modelTriangles{};
        auto found = s.models.end();
        std::uint64_t lastHash{};
        auto& instancedModels = s.instancedModels[slot];
        instancedModels.clear();
        for (const auto& placement : s.modelInstances)
        {
            if (placement.hash != lastHash) found = s.models.find(lastHash = placement.hash);
            if (found == s.models.end() || !found->second.blas) continue;

            const auto instance = scene.addInstance(*found->second.blas, placement.transform.data(), GI_INSTANCE_MODEL);
            if (!instance) continue;

            if (debugShadows) instancedModels.push_back({*instance, placement.hash, {placement.transform[3], placement.transform[7], placement.transform[11]}});

            ++modelInstances;
            for (const auto& part : found->second.geometry->parts) modelTriangles += part.indices.size() / 3;
        }

        triangles += modelTriangles;
        std::uint32_t terrainRings{};
        for (std::uint32_t r = 0; r < GI_TERRAIN_RINGS && terrain; ++r)
            if (scene.addInstance(s.terrain[r].blas, nullptr, GI_INSTANCE_TERRAIN))
            {
                triangles += TerrainRingVertices / 3;
                ++terrainRings;
            }

        s.terrainRings = terrainRings;
        s.modelInstanceCount = modelInstances;
        s.modelTriangles = modelTriangles;

        std::array<std::uint32_t, 4> entriesByAge{};
        std::array<std::uint64_t, 4> trianglesByAge{};
        std::uint32_t drawnOnce{};
        std::uint64_t drawnOnceTriangles{};
        for (std::uint32_t index = 0; index < DynamicEntry; ++index)
        {
            const auto& entry = s.entries[index];
            if (!entry.live) continue;

            const auto age = frame.frameIndex - entry.lastDrawn;
            const bool drawn = age <= (entry.framesDrawn > 1 ? DrawnGraceFrames : 0);
            const auto bucket = age == 0 ? 0 : age <= 60 ? 1 : age <= 600 ? 2 : 3;
            ++entriesByAge[bucket];
            trianglesByAge[bucket] += entry.vertexCount / 3;
            if (age > 0 && entry.framesDrawn == 1)
            {
                ++drawnOnce;
                drawnOnceTriangles += entry.vertexCount / 3;
            }

            if (!entry.blas) continue;

            const auto kind = GI_INSTANCE_CAPTURED | (drawn ? GI_INSTANCE_DRAWN : 0u) | (index << GI_INSTANCE_ENTRY_SHIFT);
            if (const auto instance = scene.addInstance(*entry.blas, nullptr, kind, entry.alphaTested ? GI_MASK_CAPTURED_CUT_OUT : GI_MASK_SCENE))
            {
                s.entryInstance[index] = *instance;
                triangles += entry.vertexCount / 3;
            }
        }

        const auto instanceCount = scene.instanceCount();
        s.liveEntries = live;
        s.instances = instanceCount;
        s.triangles = triangles;
        s.entriesByAge = entriesByAge;
        s.trianglesByAge = trianglesByAge;
        s.drawnOnce = drawnOnce;
        s.drawnOnceTriangles = drawnOnceTriangles;
        lap(InstancesStep);

        Uploads uploads{s.uploadData[slot], s.upload[slot]->GetGPUVirtualAddress()};
        const auto scatterCount = static_cast<std::uint32_t>(scatter.size());
        const auto scatterOffset = uploads.reserve(sizeof(GIScatterRecord), scatterCount);
        std::memcpy(uploads.data + scatterOffset, scatter.data(), scatter.size() * sizeof(GIScatterRecord));

        const auto parity = frame.frameIndex & 1;
        const auto constantsOffset = uploads.reserve(256, (sizeof(GIFrameConstants) + 255) / 256);
        auto constants = constantsOf(frame, scatterCount, instanceCount);
        if (terrain)
        {
            const auto& t = *frame.terrain;
            constants.terrainScale = {t.scale[0], t.scale[1], t.scale[2], 1.0f};
            constants.terrainOffset = {t.offset[0], t.offset[1], t.offset[2], 0.0f};
            constants.terrainUvConverter = {t.uvConverter[0], t.uvConverter[1], t.uvConverter[2], t.uvConverter[3]};
            constants.terrainTexelCorrect = {t.texelCorrect[0], t.texelCorrect[1], t.texelCorrect[2], t.texelCorrect[3]};
            for (std::uint32_t r = 0; r < GI_TERRAIN_RINGS; ++r)
            {
                const auto& ring = s.terrain[r];
                constants.terrainRings[r] = {ring.corner[0], ring.corner[1], TerrainCellSize * std::pow(4.0f, static_cast<float>(r)), static_cast<float>(GI_TERRAIN_CELLS)};
                (&constants.terrainRingOffsets.x)[r] = static_cast<std::uint32_t>(ring.poolOffset);
            }
        }
        if (traceLight) s.light->fill(constants, frame.verticalFov, parity);
        if (traceShadows) s.shadows->fill(constants);
        std::memcpy(uploads.data + constantsOffset, &constants, sizeof(constants));

        // ---- Descriptors ----
        Descriptors descriptors(s.device.Get(), s.heap.Get(), s.descriptorSize, slot);
        descriptors.texture(0, depth, DXGI_FORMAT_R32_FLOAT);
        descriptors.texture(1, normal, DXGI_FORMAT_R10G10B10A2_UNORM);
        descriptors.texture(2, albedo, DXGI_FORMAT_R8G8B8A8_UNORM);
        descriptors.texture(3, transfer, DXGI_FORMAT_R32G32B32A32_FLOAT);
        descriptors.buffer(4, s.upload[slot].Get(), scatterOffset, sizeof(GIScatterRecord), scatterCount);
        descriptors.buffer(5, scene.instanceBuffer(), 0, sizeof(GIInstanceDesc), instanceCount);
        descriptors.rawBuffer(6, scene.pool(), gi_scene::Scene::PoolBytes);
        descriptors.buffer(16, scene.geometryTable(), 0, sizeof(GIGeometry), MaxGeometries);
        descriptors.outputTexture(0, output, DXGI_FORMAT_R16G16B16A16_FLOAT);
        descriptors.outputRawBuffer(1, scene.pool(), gi_scene::Scene::PoolBytes);
        descriptors.outputBuffer(2, s.entryBounds.Get(), 4, std::uint64_t{MaxEntries} * 6);
        descriptors.outputBuffer(3, scene.instanceBuffer(), sizeof(GIInstanceDesc), MaxInstances);
        descriptors.outputBuffer(4, s.counters.Get(), 4, GI_COUNTER_COUNT);
        descriptors.outputBuffer(5, s.dispatchArguments.Get(), 16, GI_DISPATCH_SLOTS);
        descriptors.outputBuffer(6, s.geometryAlbedo.Get(), 16, MaxGeometries);
        descriptors.texture(10, s.depthPyramid.Get(), DXGI_FORMAT_R32_FLOAT, s.pyramidLevels);
        if (terrain)
        {
            descriptors.texture(14, terrainPages, frame.terrain->pagesFormat);
            descriptors.texture(15, terrainHeights, frame.terrain->heightsFormat);
        }
        descriptors.outputBuffer(76, s.instanceDebug.Get(), 16, MaxInstances);
        if (debugShadows)
        {
            // Each captured mesh's state for the probe (GI_ENTRY_* in gi_shared.h), and its instance.
            const auto statesOffset = uploads.reserve(8, MaxEntries);
            auto* states = reinterpret_cast<std::uint32_t*>(uploads.data + statesOffset);
            for (std::uint32_t index = 0; index < MaxEntries; ++index)
            {
                const auto& entry = s.entries[index];
                const auto instance = s.entryInstance[index];
                const bool built = entry.blas && scene.ready(*entry.blas);
                std::uint32_t bits = (entry.live ? GI_ENTRY_LIVE : 0u) | (built ? GI_ENTRY_BUILT : 0u) | (entry.alphaTested ? GI_ENTRY_ALPHA_TESTED : 0u)
                    | (instance != GI_REASON_NONE ? GI_ENTRY_INSTANCED : 0u);
                if (index == DynamicEntry && skinnedVertices) bits = GI_ENTRY_LIVE | GI_ENTRY_BUILT | GI_ENTRY_INSTANCED;
                const auto age = entry.live ? std::min<std::uint32_t>(frame.frameIndex - entry.lastDrawn, 0xFFF) : 0u;
                states[2 * index] = bits | (age << 8) | (std::min<std::uint32_t>(entry.framesDrawn, 0xFFF) << 20);
                states[2 * index + 1] = instance;
            }

            descriptors.buffer(11, s.upload[slot].Get(), statesOffset, 8, MaxEntries);
            descriptors.outputBuffer(77, s.probe.Get(), sizeof(GIShadowProbe), 1);
            descriptors.outputTexture(78, shadowDebug, DXGI_FORMAT_R8G8B8A8_UNORM);
            descriptors.outputBuffer(79, s.probeCrossings.Get(), 16, GI_PROBE_CROSSINGS);
        }
        for (UINT level = 0; level < s.pyramidLevels; ++level)
            descriptors.outputTexture(PyramidSlot + level, s.depthPyramid.Get(), DXGI_FORMAT_R32_FLOAT, level);
        if (traceLight) s.light->describe(descriptors, parity);
        if (traceShadows) s.shadows->describe(descriptors, parity, shadow);

        ID3D12DescriptorHeap* heaps[]{s.heap.Get()};
        list.SetDescriptorHeaps(1, heaps);
        list.SetComputeRootSignature(s.rootSignature.Get());
        list.SetComputeRootConstantBufferView(0, uploads.address + constantsOffset);
        list.SetComputeRootDescriptorTable(2, descriptors.srvTable());
        list.SetComputeRootDescriptorTable(3, descriptors.uavTable());
        list.SetComputeRootShaderResourceView(4, scene.topLevel()->GetGPUVirtualAddress());
        // The game's textures stay in the common state between frames: the shaders' reads promote them.
        auto textureRange = s.heap->GetGPUDescriptorHandleForHeapStart();
        textureRange.ptr += std::uint64_t{TableSize} * FramesInFlight * s.descriptorSize;
        list.SetComputeRootDescriptorTable(5, textureRange);
        Recorder recorder(list, s.pipelines, s.dispatchSignature.Get(), s.dispatchArguments.Get());
        auto& marks = s.marks[slot];
        marks.clear();
        recorder.marker = [&](const char* phase) {
            if (marks.size() >= MaxMarks) return;

            list.EndQuery(s.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * MaxMarks + static_cast<UINT>(marks.size()));
            marks.push_back(phase ? phase : "");
        };

        recorder.mark("scatter");
        D3D12_RESOURCE_BARRIER begin[]{
            transition(depth, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            transition(normal, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            transition(albedo, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            transition(transfer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            transition(output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        };
        list.ResourceBarrier(static_cast<UINT>(std::size(begin)), begin);
        if (traceShadows) recorder.transition(shadow, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (debugShadows) recorder.transition(shadowDebug, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (terrain)
        {
            recorder.transition(terrainPages, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            recorder.transition(terrainHeights, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }

        // The pool's uploads and the table's rows, then the geometry the GPU makes: the captures, the terrain.
        scene.recordWrites(list);
        if (scatterCount)
        {
            recorder.dispatch(Kernel::ClearBounds, groups(scatterCount, 64));
            recorder.barrier();
            recorder.dispatch(Kernel::Scatter, groups(scatterVertices, 64), 1, {scatterVertices});
        }

        for (std::uint32_t r = 0; r < GI_TERRAIN_RINGS; ++r)
            if (remadeRing[r]) recorder.dispatch(Kernel::TerrainGrid, groups(GI_TERRAIN_CELLS, 8), groups(GI_TERRAIN_CELLS, 8), {r});

        recorder.mark("bottom-level structures");
        scene.recordBottomLevels(list);

        // The captured meshes' culling reads the depth pyramid and their bounds; the top level, the
        // bottom levels and the instances.
        recorder.mark("instances and top level");
        for (UINT level = 0; level < s.pyramidLevels; ++level)
        {
            recorder.dispatch(Kernel::BuildDepthPyramid, groups(std::max(s.pyramidWidth >> level, 1u), 8), groups(std::max(s.pyramidHeight >> level, 1u), 8),
                {level});
            recorder.barrier();
        }

        scene.recordInstances(list);
        recorder.transition(s.depthPyramid.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        recorder.dispatch(Kernel::CullCaptured, groups(instanceCount, 64));
        recorder.transition(s.depthPyramid.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        scene.recordTopLevel(list);

        // ---- The shadows, then the light or the debug view ----
        if (traceShadows) s.shadows->record(recorder);
        if (debugShadows)
        {
            recorder.mark("debug view and probe");
            recorder.dispatch(Kernel::DebugShadows, groups(frame.width, 8), groups(frame.height, 8));
            recorder.dispatch(Kernel::ProbeShadows, 1, 1, {MaxEntries});
            recorder.barrier();
            D3D12_RESOURCE_BARRIER copy[]{
                transition(s.probe.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
                transition(s.probeCrossings.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
            };
            list.ResourceBarrier(static_cast<UINT>(std::size(copy)), copy);
            list.CopyBufferRegion(s.probeReadback[slot].Get(), 0, s.probe.Get(), 0, sizeof(GIShadowProbe));
            list.CopyBufferRegion(s.probeReadback[slot].Get(), sizeof(GIShadowProbe), s.probeCrossings.Get(), 0, std::uint64_t{GI_PROBE_CROSSINGS} * 16);
            for (auto& barrier : copy) std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            list.ResourceBarrier(static_cast<UINT>(std::size(copy)), copy);
            s.probeWritten[slot] = true;
        }
        if (frame.showScene)
        {
            recorder.mark("scene view");
            recorder.dispatch(Kernel::DebugScene, groups(frame.width, 8), groups(frame.height, 8));
        }
        else if (traceLight) s.light->record(recorder, parity, depth, normal);

        // Diagnostics: every ten seconds, a copy of the output to read back.
        const auto now = GetTickCount64();
        if (!s.statsFence && now - s.lastStats > 10'000 && !frame.showScene && traceLight)
        {
            s.lastStats = now;
            const auto desc = output->GetDesc();
            UINT64 bytes{};
            s.device->GetCopyableFootprints(&desc, 0, 1, 0, &s.statsFootprint, nullptr, nullptr, &bytes);
            if (!s.statsReadback || s.statsReadback->GetDesc().Width < bytes)
                s.statsReadback = createBuffer(s.device.Get(), bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_FLAG_NONE, "Creating the light's readback");
            if (!s.statsCounters)
                s.statsCounters = createBuffer(s.device.Get(), GI_COUNTER_COUNT * 4, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_FLAG_NONE, "Creating the counters' readback");

            recorder.transition(s.counters.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list.CopyBufferRegion(s.statsCounters.Get(), 0, s.counters.Get(), 0, GI_COUNTER_COUNT * 4);
            recorder.transition(s.counters.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            recorder.transition(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION from{output, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
            from.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION to{s.statsReadback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
            to.PlacedFootprint = s.statsFootprint;
            list.CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            recorder.transition(output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            s.statsFence = frame.signalValue;
        }

        D3D12_RESOURCE_BARRIER end[]{
            uavBarrier(),
            transition(depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
            transition(normal, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
            transition(albedo, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
            transition(transfer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
            transition(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON),
        };
        list.ResourceBarrier(static_cast<UINT>(std::size(end)), end);
        if (traceShadows) recorder.transition(shadow, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        if (debugShadows) recorder.transition(shadowDebug, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        if (terrain)
        {
            recorder.transition(terrainPages, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            recorder.transition(terrainHeights, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        }
        recorder.mark(nullptr);
        list.ResolveQueryData(s.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * MaxMarks, static_cast<UINT>(marks.size()), s.timestampReadback.Get(),
            std::uint64_t{slot} * MaxMarks * 8);
        check(list.Close(), "Recording the global illumination");
    }
    catch (...)
    {
        // The next frame resets the list, which it can only once closed.
        list.Close();
        throw;
    }

    check(s.queue->Wait(s.fence.Get(), frame.waitValue), "Waiting for the global illumination's inputs");
    ID3D12CommandList* lists[]{s.list.Get()};
    s.queue->ExecuteCommandLists(1, lists);
    check(s.queue->Signal(s.fence.Get(), frame.signalValue), "Signalling the global illumination");
    s.submitted[slot] = frame.signalValue;
    lap(RecordStep);

    LARGE_INTEGER finished{};
    QueryPerformanceCounter(&finished);
    const double cpu = static_cast<double>(finished.QuadPart - started.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
    const auto stats = scene.stats();
    auto& t = s.tally;
    ++t.frames;
    t.cpuMilliseconds += cpu;
    t.mostCpuMilliseconds = std::max(t.mostCpuMilliseconds, cpu);
    t.builds += stats.builtThisFrame;
    t.mostBuilds = std::max(t.mostBuilds, stats.builtThisFrame);
    t.builtTriangles += stats.builtTriangles;
    t.waiting += stats.waiting;
    t.compactions += stats.compactedThisFrame;
    t.skinnedTriangles += skinnedVertices / 3;

    s.evictedByFrame[frame.frameIndex % s.evictedByFrame.size()] = static_cast<std::uint32_t>(result.evicted.size());
    return result;
}

std::string shadowProbe()
{
    std::lock_guard lock(probeMutex);
    return probeText;
}

std::string status()
{
    if (!state) return {};

    const auto& s = *state;
    const auto stats = s.scene->stats();
    const auto milliseconds = [](double value) {
        auto text = std::to_string(value);
        return text.substr(0, text.find('.') + 3) + " ms";
    };

    const auto aged = [&](int bucket) {
        return std::to_string(s.entriesByAge[bucket]) + " (" + std::to_string(s.trianglesByAge[bucket] / 1000) + "k triangles)";
    };

    const auto compactedShare = stats.compactedFrom ? std::to_string(stats.compactedTo * 100 / stats.compactedFrom) + "%" : std::string("-");
    return std::to_string(s.instances) + " instances, " + std::to_string(s.triangles / 1000) + "k triangles; geometry pool "
        + percentOf(stats.poolUsed, stats.poolCapacity) + ", structures " + percentOf(stats.blasUsed, stats.blasCapacity) + " (" + std::to_string(stats.structures)
        + ", " + std::to_string(stats.compacted) + " compacted to " + compactedShare + " of their size, " + std::to_string(stats.waiting) + " waiting), "
        + std::to_string(stats.geometriesUsed) + " geometries; GPU: scene " + milliseconds(s.sceneMilliseconds) + ", light " + milliseconds(s.lightMilliseconds)
        + ", shadows " + milliseconds(s.shadowMilliseconds) + "; the game's models: " + std::to_string(s.modelInstanceCount) + " instances ("
        + std::to_string(s.modelTriangles / 1000) + "k triangles) of " + std::to_string(s.models.size()) + " models, " + std::to_string(s.modelsWaiting)
        + " waiting for room" + (s.modelsTooLarge ? ", " + std::to_string(s.modelsTooLarge) + " too large" : "") + ", "
        + std::to_string(GI_MAX_TEXTURES - s.freeTextureIndices.size()) + " of the game's textures held; terrain rings "
        + std::to_string(s.terrainRings) + " of " + std::to_string(GI_TERRAIN_RINGS) + "; captured meshes " + std::to_string(s.liveEntries)
        + ": drawn this frame " + aged(0) + ", within a second " + aged(1) + ", within ten seconds " + aged(2) + ", before " + aged(3)
        + "; drawn in one earlier frame only " + std::to_string(s.drawnOnce) + " (" + std::to_string(s.drawnOnceTriangles / 1000) + "k triangles)";
}
}
