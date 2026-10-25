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
#include "ClassifyShadowTiles.h"
#include "ClearCounters.h"
#include "ClearNewGeometries.h"
#include "ClearOutput.h"
#include "CompositeLight.h"
#include "DebugScene.h"
#include "DebugShadows.h"
#include "FilterLocalLightShadows.h"
#include "FilterShadows.h"
#include "GenerateDispatch.h"
#include "LearnAlbedo.h"
#include "PopulateCells.h"
#include "PopulateMultibounceCells.h"
#include "PrepareGuides.h"
#include "ProbeShadows.h"
#include "PurgeTiles.h"
#include "ResolveLight.h"
#include "Skin.h"
#include "TerrainGrid.h"
#include "TraceLight.h"
#include "TraceLocalLightShadows.h"
#include "TraceSunShadows.h"
#include "UpdateFoliageMemory.h"
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

// The renderer's own upload buffer holds the frame's constants, its new geometries' rows, and its
// characters' skin records and palettes.
constexpr std::uint64_t UploadBytes = 8ull << 20;
// Where no albedo is known yet: MGSV's materials are dark.
constexpr float DefaultAlbedo = 0.2f;
// Tangent of the sun's angular radius, 0.27 degrees.
constexpr float SunRadius = 0.0047f;
// The sun's ray-traced shadows: how much of the sun cut-out geometry without its texture (not copied
// yet, or a mesh without texture coordinates) blocks beyond the game's shadow map, and how far the rays
// look for what blocks the sun.
constexpr float AlphaTestedBlocking = 0.5f, ShadowRayLength = 5000;
// Diagnostics: the most phases a frame's list times.
constexpr std::uint32_t MaxMarks = 32;
// A model the game has not placed for this many frames is let go.
constexpr std::uint32_t ModelRetentionFrames = 600;
// A character's structure is refitted to each pose and built again this often; the frame's palettes
// take this much of the upload buffer at most (characters past it wait for the next frame).
constexpr std::uint32_t CharacterRebuildFrames = 30;
constexpr std::uint64_t MaxPaletteBytes = 2ull << 20;
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
constexpr std::span<const unsigned char> Bytecode[]{GI_ClearNewGeometries, GI_DebugScene, GI_ClearOutput, GI_TerrainGrid, GI_Skin,
    GI_ClearCounters, GI_GenerateDispatch, GI_PrepareGuides, GI_TraceLight, GI_LearnAlbedo, GI_PurgeTiles, GI_PopulateMultibounceCells,
    GI_PopulateCells, GI_UpdateTiles, GI_UpdateMultibounceCells, GI_ResolveLight, GI_UpdateFoliageMemory, GI_CompositeLight, GI_TraceSunShadows, GI_ClassifyShadowTiles,
    GI_FilterShadows, GI_DebugShadows, GI_ProbeShadows, GI_TraceLocalLightShadows, GI_FilterLocalLightShadows};
static_assert(std::size(Bytecode) == static_cast<std::size_t>(Kernel::Count));

// One of the game's models on the GPU side: its meshes in the pool from `poolOffset`, made once it is
// first placed; the frame it was last placed. A rigid model has a structure, a geometry a mesh; a
// skinned model's characters pose its meshes (`parts`: where each one's bind pose and indices lie, and
// its material; `skins`: where each one's skin lies) into their own structures.
struct ModelRecord
{
    std::shared_ptr<const Model> geometry;
    std::optional<std::uint64_t> poolOffset;
    std::uint64_t poolBytes{};
    std::optional<BlasId> blas;
    std::vector<gi_scene::Geometry> parts;
    std::vector<std::uint64_t> skins;
    std::uint32_t lastPlaced{};
    std::array<float, 6> bounds{}; // Diagnostics (the shadow probe): object space, lowest then highest
};

// One of the game's characters on the GPU side: its model's meshes it shows (`parts`, in the model's
// order), posed into the pool from `poolOffset` in that order, a geometry each of its structure; where
// it is this frame; whether its structure matches its pose this frame, and the frame it was last built.
struct CharacterRecord
{
    std::uint64_t hash{};
    std::vector<std::uint32_t> parts;
    std::uint64_t poolOffset{}, poolBytes{};
    BlasId blas{};
    Transform transform{};
    bool traced{};
    std::uint32_t built{};
};

// Diagnostics (the probe): one of a frame's instances of the game's models, and where it is.
struct InstancedModel
{
    std::uint32_t instance{};
    std::uint64_t hash{};
    std::array<float, 3> position{};
    std::uint64_t character{}; // A character's id (CharacterInstance::id); 0 for a placed model
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
    ComPtr<ID3D12Resource> geometryAlbedo, counters, dispatchArguments;
    std::array<ComPtr<ID3D12Resource>, FramesInFlight> upload;
    std::array<std::uint8_t*, FramesInFlight> uploadData{};

    // The game's models (engine_scene.hpp) by hash, and the latest placements; the characters by id,
    // and the latest list.
    std::unordered_map<std::uint64_t, ModelRecord> models;
    std::vector<ModelInstance> modelInstances;
    std::unordered_map<std::uint64_t, CharacterRecord> characters;
    std::vector<CharacterInstance> characterList;
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
    // Diagnostics: the frame's characters traced, those waiting for their model or for room, and their
    // posed vertices and triangles.
    std::uint32_t charactersTraced{}, charactersWaiting{};
    std::uint64_t characterVertices{}, characterTriangles{};
    std::uint32_t instances{};
    std::uint64_t triangles{};

    std::unique_ptr<gi_light::Light> light;
    std::unique_ptr<gi_shadow::Shadows> shadows;
    // The point and spot lights' shadows before and between the filter's passes (TraceLocalLightShadows,
    // FilterLocalLightShadows), R16G16_FLOAT at the render size.
    ComPtr<ID3D12Resource> localShadowRays, localShadowFirst;
    UINT localShadowWidth{}, localShadowHeight{};
    // Diagnostics (the shadow debug view and probe): the probe, read back when the frame's slot comes
    // round again, and each frame's instances of the game's models.
    ComPtr<ID3D12Resource> probe;
    std::array<ComPtr<ID3D12Resource>, FramesInFlight> probeReadback;
    std::array<bool, FramesInFlight> probeWritten{};
    std::array<std::vector<InstancedModel>, FramesInFlight> instancedModels; // Each slot's frame's, by instance

    // GPU timestamps between the named phases of each frame's list (Recorder::mark), read when the
    // frame's slot comes round again: into the settings window's costs, by pass. The scene's, the light's
    // and the shadows' times are averaged for the overlay; each phase's are summed for the log, with the
    // frame's builds and CPU time.
    struct Mark
    {
        const char* phase{};
        std::optional<performance::Pass> pass;
    };
    ComPtr<ID3D12QueryHeap> timestamps;
    ComPtr<ID3D12Resource> timestampReadback;
    std::uint64_t timestampFrequency{};
    std::array<std::vector<Mark>, FramesInFlight> marks;
    double sceneMilliseconds{}, lightMilliseconds{}, shadowMilliseconds{};
    struct Tally
    {
        std::vector<std::pair<const char*, double>> phases; // Milliseconds, in the order they first ran
        std::uint32_t timedFrames{}, frames{}, builds{}, mostBuilds{}, waiting{}, compactions{};
        std::uint64_t builtTriangles{};
        double cpuMilliseconds{}, mostCpuMilliseconds{};
        std::array<double, CpuSteps> cpuSteps{};
        std::uint64_t since{};
    } tally;

    // Diagnostics: a copy of the output every ten seconds, read once the GPU is past it.
    ComPtr<ID3D12Resource> statsReadback, statsCounters;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT statsFootprint{};
    std::uint64_t statsFence{}, lastStats{};
    Shared depth, normal, albedo, output, shadow, shadowDebug, velocity, terrainPages, terrainHeights, localLightShadow, lightDebug;

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
    s.geometryAlbedo = createStorage(device, std::uint64_t{MaxGeometries} * 16, "Creating the geometry albedo");
    s.counters = createStorage(device, GI_COUNTER_COUNT * 4, "Creating the counters");
    s.probe = createStorage(device, sizeof(GIShadowProbe), "Creating the shadow probe");
    for (auto& readback : s.probeReadback)
        readback = createBuffer(device, sizeof(GIShadowProbe), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE,
            "Creating the shadow probe's readback");
    s.dispatchArguments = createStorage(device, GI_DISPATCH_SLOTS * 16, "Creating the dispatch arguments");

    for (std::uint32_t slot = 0; slot < FramesInFlight; ++slot)
    {
        s.upload[slot] = createBuffer(device, UploadBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE,
            "Creating an upload buffer");
        void* data{};
        check(s.upload[slot]->Map(0, nullptr, &data), "Mapping an upload buffer");
        s.uploadData[slot] = static_cast<std::uint8_t*>(data);
    }

    // The terrain's room in the pool and its structures, kept while the renderer runs.
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

// A model mesh's row of the geometry table, with its base texture once the game's is copied: its
// colour for the hits' albedo, and a cut-out mesh's alpha for the shadow rays' test.
gi_scene::Geometry materialOf(const State& s, const ModelPart& part)
{
    gi_scene::Geometry geometry;
    geometry.flags = (part.alphaTested ? GI_GEOMETRY_CUT_OUT : 0u) | (part.uvs.empty() ? 0u : GI_GEOMETRY_UV) | (part.leaf ? GI_GEOMETRY_LEAF : 0u);
    const auto texture = part.uvs.empty() ? s.textures.end() : s.textures.find(part.baseTexture);
    if (texture == s.textures.end() || !texture->second.shared.resource) return geometry;

    geometry.texture = texture->second.index + 1;
    if (part.textureIsMask) geometry.flags |= GI_GEOMETRY_MASK;
    if (part.alphaTested)
    {
        geometry.flags |= GI_GEOMETRY_ALPHA_TEXTURE;
        geometry.alphaThreshold = CutOutThreshold;
    }

    return geometry;
}

// The bytes of a model's meshes in the pool: each mesh's vertices (16 bytes: the position, then the
// texture coordinates), then its indices, from 16-byte boundaries; a skinned model's then its skin
// (GISkinRecord: 16 bytes a vertex).
std::uint64_t modelBytes(const Model& model)
{
    std::uint64_t bytes{};
    for (std::size_t i = 0; i < model.parts.size() && i < MaxModelParts; ++i)
    {
        const auto& part = model.parts[i];
        bytes += part.vertices.size() * 16 + (part.indices.size() * 2 + 15) / 16 * 16 + (model.bones ? part.vertices.size() * 16 : 0);
    }

    return bytes;
}

// The model's meshes laid out from `poolOffset` into `bytes`, and their geometries; a skinned model's
// skins' places into `skins`.
std::vector<gi_scene::Geometry> stageModel(const State& s, const Model& model, std::uint64_t poolOffset, std::vector<std::uint8_t>& bytes,
    std::vector<std::uint64_t>& skins)
{
    std::vector<gi_scene::Geometry> geometries;
    skins.clear();
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
        if (!model.bones) continue;

        skins.push_back(poolOffset + at);
        for (std::size_t v = 0; v < part.vertices.size(); ++v)
        {
            const std::uint64_t bones = v < part.bones.size() ? part.bones[v] : 0;
            const std::uint32_t weights = v < part.weights.size() ? part.weights[v] : 0xFF;
            std::memcpy(bytes.data() + at, &bones, 8);
            std::memcpy(bytes.data() + at + 8, &weights, 4);
            at += 16;
        }
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
            if (model == s.models.end()) continue;

            auto& modelRecord = model->second;
            const auto& parts = modelRecord.geometry->parts;
            for (std::uint32_t i = 0; i < parts.size() && i < MaxModelParts; ++i)
            {
                if (parts[i].baseTexture != made.path) continue;

                const auto geometry = materialOf(s, parts[i]);
                if (modelRecord.blas) s.scene->setMaterial(*modelRecord.blas, i, geometry.flags, geometry.texture, geometry.alphaThreshold);
                if (i >= modelRecord.parts.size()) continue;

                // A skinned model's mesh: its characters' structures have it among the meshes they show.
                modelRecord.parts[i].flags = geometry.flags;
                modelRecord.parts[i].texture = geometry.texture;
                modelRecord.parts[i].alphaThreshold = geometry.alphaThreshold;
                for (const auto& [id, character] : s.characters)
                {
                    const auto shown = std::find(character.parts.begin(), character.parts.end(), i);
                    if (character.hash == user && shown != character.parts.end())
                        s.scene->setMaterial(character.blas, static_cast<std::uint32_t>(shown - character.parts.begin()), geometry.flags, geometry.texture,
                            geometry.alphaThreshold);
                }
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
        // Kept ModelRetentionFrames from now, placed or not: counted from frame 0, a model the frame's
        // lists did not place was let go as it came, and its file read again the next snapshot.
        auto& record = s.models[model->hash];
        record.geometry = model;
        record.lastPlaced = frame.frameIndex;
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

    if (frame.modelInstancesUpdated)
    {
        s.modelInstances = frame.modelInstances;
        s.characterList = frame.characters;
    }

    // Placements come grouped by model (an instance group's, a brush plugin's): one lookup a run.
    auto found = s.models.end();
    std::uint64_t lastHash{};
    for (const auto& placement : s.modelInstances)
    {
        if (placement.hash != lastHash) found = s.models.find(lastHash = placement.hash);
        if (found != s.models.end()) found->second.lastPlaced = frame.frameIndex;
    }

    for (const auto& character : s.characterList)
        if (const auto model = s.models.find(character.hash); model != s.models.end()) model->second.lastPlaced = frame.frameIndex;

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

        // Made once first placed; a model larger than a frame's upload never is. A skinned model is
        // only laid out in the pool: its characters make their own structures (updateCharacters).
        const bool skinned = record.geometry->bones != 0;
        const bool made = skinned ? record.poolOffset.has_value() : record.blas.has_value();
        const auto needed = modelBytes(*record.geometry);
        if (record.lastPlaced == frame.frameIndex && !made && needed && needed <= gi_scene::GeometryUploadBytes)
        {
            // What has no room this frame waits for the next; an upload whose structure has no room
            // lands in the range given back, which nothing uses before this frame's copy.
            const auto offset = needed <= s.scene->uploadRoom() ? s.scene->allocate(needed) : std::nullopt;
            std::vector<std::uint64_t> skins;
            auto geometries = offset ? stageModel(s, *record.geometry, *offset, bytes, skins) : std::vector<gi_scene::Geometry>{};
            const bool uploaded = offset && s.scene->upload(*offset, bytes);
            const auto blas = uploaded && !skinned ? s.scene->createBlas(gi_scene::Use::Static, geometries) : std::nullopt;
            if (uploaded && (skinned || blas))
            {
                record.poolOffset = offset;
                record.poolBytes = needed;
                record.blas = blas;
                if (skinned)
                {
                    record.parts = std::move(geometries);
                    record.skins = std::move(skins);
                }
            }
            else
            {
                if (offset) s.scene->release(*offset, needed);
                ++s.modelsWaiting;
            }
        }
        else if (needed > gi_scene::GeometryUploadBytes && record.lastPlaced == frame.frameIndex && !made)
            ++s.modelsTooLarge;

        ++it;
    }
}

// The frame's characters (Frame::characters): each one's shown meshes posed into its own room of the
// pool (a record a mesh into `skins` for Skin in gi_scene.hlsl, its palette onto `palettes`), its
// structure refitted to the pose, or built again when new and every CharacterRebuildFrames (a refit's
// boxes loosen as limbs move). One that shows other meshes is made again; one gone from the list is
// let go. Without a newer list the last poses stand.
void updateCharacters(State& s, const Frame& frame, std::vector<GISkinRecord>& skins, std::vector<float>& palettes)
{
    if (!frame.modelInstancesUpdated) return;

    auto& scene = *s.scene;
    std::unordered_set<std::uint64_t> seen;
    std::uint32_t vertices{};
    s.charactersWaiting = 0;
    for (const auto& character : s.characterList)
    {
        const auto model = s.models.find(character.hash);
        if (model != s.models.end() && model->second.geometry->parts.empty()) continue; // Nothing the scene takes

        const auto bones = std::uint64_t{character.palette.size() / 12};
        if (model == s.models.end() || model->second.parts.empty() || bones < model->second.geometry->bones
            || (palettes.size() + character.palette.size()) * sizeof(float) > MaxPaletteBytes)
        {
            // One made already keeps its room, out of this frame.
            if (const auto kept = s.characters.find(character.id); kept != s.characters.end())
            {
                kept->second.traced = false;
                seen.insert(character.id);
            }

            ++s.charactersWaiting;
            continue;
        }

        const auto& modelRecord = model->second;
        const auto& parts = modelRecord.geometry->parts;
        std::vector<std::uint32_t> shown;
        for (std::uint32_t i = 0; i < modelRecord.parts.size(); ++i)
            if (parts[i].group >= character.shownGroups.size() || character.shownGroups[parts[i].group]) shown.push_back(i);

        if (shown.empty()) continue;

        auto found = s.characters.find(character.id);
        if (found != s.characters.end() && (found->second.hash != character.hash || found->second.parts != shown))
        {
            scene.destroyBlas(found->second.blas);
            scene.release(found->second.poolOffset, found->second.poolBytes);
            s.characters.erase(found);
            found = s.characters.end();
        }

        const bool fresh = found == s.characters.end();
        if (fresh)
        {
            std::uint64_t bytes{};
            for (const auto i : shown) bytes += std::uint64_t{modelRecord.parts[i].vertexCount} * 16;
            const auto offset = scene.allocate(bytes);
            std::vector<gi_scene::Geometry> geometries;
            std::uint64_t at{};
            for (const auto i : shown)
            {
                auto& geometry = geometries.emplace_back(modelRecord.parts[i]);
                geometry.vertexOffset = offset.value_or(0) + at;
                at += std::uint64_t{geometry.vertexCount} * 16;
            }

            const auto blas = offset ? scene.createBlas(gi_scene::Use::Refit, geometries) : std::nullopt;
            if (!blas)
            {
                if (offset) scene.release(*offset, bytes);
                ++s.charactersWaiting;
                continue;
            }

            found = s.characters.emplace(character.id, CharacterRecord{character.hash, std::move(shown), *offset, bytes, *blas}).first;
        }

        auto& record = found->second;
        seen.insert(character.id);
        record.transform = character.transform;
        const auto firstBone = static_cast<std::uint32_t>(palettes.size() / 12);
        palettes.insert(palettes.end(), character.palette.begin(), character.palette.end());
        std::uint64_t at{};
        for (const auto i : record.parts)
        {
            const auto& part = modelRecord.parts[i];
            skins.push_back({vertices, part.vertexCount, static_cast<std::uint32_t>(part.vertexOffset), static_cast<std::uint32_t>(modelRecord.skins[i]),
                static_cast<std::uint32_t>(record.poolOffset + at), firstBone, {}});
            vertices += part.vertexCount;
            at += std::uint64_t{part.vertexCount} * 16;
        }

        // Not refitted (the frame's scratch is full), the structure no longer matches the pose: left
        // out of the frame.
        const bool rebuild = fresh || frame.frameIndex - record.built >= CharacterRebuildFrames;
        record.traced = rebuild ? scene.rebuild(record.blas) : scene.refit(record.blas);
        if (rebuild && record.traced) record.built = frame.frameIndex;
    }

    for (auto it = s.characters.begin(); it != s.characters.end();)
    {
        if (seen.contains(it->first))
        {
            ++it;
            continue;
        }

        scene.destroyBlas(it->second.blas);
        scene.release(it->second.poolOffset, it->second.poolBytes);
        it = s.characters.erase(it);
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

GIFrameConstants constantsOf(const Frame& frame, std::uint32_t newGeometries, std::uint32_t instanceCount)
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
    constants.counts = {newGeometries, 0, frame.showScene ? GI_VIEW_SCENE : GI_VIEW_OFF, 0};
    constants.shadowRange = {frame.shadowRange[0], frame.shadowRange[1], frame.shadowRange[2], frame.shadowRange[3]};
    constants.shadowParams = {frame.sunTangent, AlphaTestedBlocking, ShadowRayLength, 0};
    constants.rayCone = {2 * std::tan(frame.verticalFov / 2) / static_cast<float>(std::max(frame.height, 1u)), 0, 0, 0};
    constants.jitter = {frame.jitter[0], frame.jitter[1], frame.jitter[2], frame.jitter[3]};
    const auto localLights = std::min<std::size_t>(frame.localLights.size(), GI_MAX_LOCAL_LIGHTS);
    for (std::size_t i = 0; i < localLights; ++i)
    {
        const auto& light = frame.localLights[i];
        constants.localLights[i] = {{light.position[0], light.position[1], light.position[2], light.range},
            {light.axis[0], light.axis[1], light.axis[2], light.cosOuter}, {light.clearance, light.penumbraScale, light.coneExponent, light.maxFalloff},
            {light.color[0], light.color[1], light.color[2], light.factor}, {light.sixteenth ? 1.0f : 0.0f, light.addedDistance, light.leastDistance, light.radius}};
    }
    constants.localLightCount = {static_cast<std::uint32_t>(localLights), frame.localLightRays, 0, 0};

    return constants;
}

// The camera for NRD: column-vector matrices stored by columns, the projections without the upscaler's
// jitter and the jitter apart, the engine's x mirror moved from the projection into the view
// (gi_denoiser::Camera). NRD requires the projections without the jitter: with it in, the denoiser
// followed each frame's jittered samples, and grass and fine detail shimmered (2026-10-25).
gi_denoiser::Camera denoiserCamera(const Frame& frame)
{
    const auto columns = [](const Matrix& rows, bool mirrorColumn0, bool mirrorRow0) {
        std::array<float, 16> stored{};
        for (int row = 0; row < 4; ++row)
            for (int column = 0; column < 4; ++column)
            {
                const bool mirrored = (mirrorColumn0 && column == 0) || (mirrorRow0 && row == 0);
                stored[column * 4 + row] = mirrored ? -rows[row * 4 + column] : rows[row * 4 + column];
            }

        return stored;
    };

    // The jitter is the projection's third column in its first two rows (camera::applyJitter).
    const auto unjittered = [](Matrix projection, float x, float y) {
        projection[2] -= x;
        projection[6] -= y;
        return projection;
    };
    // A light pixel's sample is one of its 2 x 2's render pixels, by depth (RenderPixelOf): at its centre on
    // average, moved the jitter back (x right, y down).
    const float width = static_cast<float>(frame.width), height = static_cast<float>(frame.height);
    const auto sampleOffset = [&](float x, float y) { return std::array<float, 2>{-x * width / 4, y * height / 4}; };

    gi_denoiser::Camera camera;
    camera.viewToClip = columns(unjittered(frame.projection, frame.jitter[0], frame.jitter[1]), true, false);
    camera.previousViewToClip = columns(unjittered(frame.previousProjection, frame.jitter[2], frame.jitter[3]), true, false);
    camera.jitter = sampleOffset(frame.jitter[0], frame.jitter[1]);
    camera.previousJitter = sampleOffset(frame.jitter[2], frame.jitter[3]);
    camera.worldToView = columns(frame.view, false, true);
    camera.previousWorldToView = columns(frame.previousView, false, true);
    camera.restart = frame.cameraCut;
    return camera;
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

// Diagnostics: every ten seconds, a copy of the output and the counters for logStats.
void copyStats(State& s, Recorder& recorder, ID3D12Resource* output, const Frame& frame)
{
    const auto now = GetTickCount64();
    if (s.statsFence || now - s.lastStats <= (frame.logOften ? 0u : 10'000u)) return;

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
    s.list->CopyBufferRegion(s.statsCounters.Get(), 0, s.counters.Get(), 0, GI_COUNTER_COUNT * 4);
    recorder.transition(s.counters.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    recorder.transition(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION from{output, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    from.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION to{s.statsReadback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
    to.PlacedFootprint = s.statsFootprint;
    s.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    recorder.transition(output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    s.statsFence = frame.signalValue;
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
    log("Global illumination's rays: " + value(GI_COUNTER_HITS) + " hits (" + value(GI_COUNTER_NEAR_HITS) + " within 25 cm), "
        + value(GI_COUNTER_BOUNCE_HITS) + " bounces that hit (" + value(GI_COUNTER_VISIBILITY0) + " into the cache, " + value(GI_COUNTER_FULL_BUCKETS)
        + " without room in it), " + value(GI_COUNTER_VISIBILITY1) + " multibounce hits, " + value(GI_COUNTER_LIGHTS) + " cache hits lit, "
        + value(GI_COUNTER_COPIES) + " copies of a surface skipped, " + value(GI_COUNTER_UPDATE_TILES) + " cache tiles updated, "
        + value(GI_COUNTER_FOLIAGE) + " light pixels on foliage (" + value(GI_COUNTER_FOLIAGE_MEMORY_FULL) + " without room in the foliage memory).");

    constexpr const char* MaximumNames[GI_COUNTER_MAXIMA]{"a hit's albedo", "a hit's direct light", "a cell's direct light", "a cell's indirect light",
        "a multibounce", "a ray's light", "a denoised light pixel", "a render pixel"};
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
    performance::Milliseconds passes{};
    for (std::size_t i = 0; i + 1 < marks.size(); ++i)
    {
        const double milliseconds = ticks[i + 1] >= ticks[i] ? static_cast<double>(ticks[i + 1] - ticks[i]) * toMilliseconds : 0.0;
        if (const auto pass = marks[i].pass) passes[static_cast<std::size_t>(*pass)] += static_cast<float>(milliseconds);
        auto& phases = s.tally.phases;
        const auto found = std::find_if(phases.begin(), phases.end(), [&](const auto& phase) { return std::strcmp(phase.first, marks[i].phase) == 0; });
        if (found != phases.end()) found->second += milliseconds;
        else phases.emplace_back(marks[i].phase, milliseconds);
    }

    const D3D12_RANGE none{};
    s.timestampReadback->Unmap(0, &none);
    marks.clear();
    performance::timedOnDirect3D12(passes);

    using enum performance::Pass;
    const auto sum = [&](std::initializer_list<performance::Pass> of) {
        double total{};
        for (const auto pass : of) total += passes[static_cast<std::size_t>(pass)];
        return total;
    };
    const double scene = sum({RayTracedScene});
    const double light = sum({GiRays, GiDenoiser});
    const double shadow = sum({SunShadowRays, SunShadowDenoiser, LightShadowRays, LightShadowFilter});
    s.sceneMilliseconds += (scene - s.sceneMilliseconds) * 0.05;
    s.lightMilliseconds += (light - s.lightMilliseconds) * 0.05;
    s.shadowMilliseconds += (shadow - s.shadowMilliseconds) * 0.05;
    ++s.tally.timedFrames;
}

std::string percentOf(std::uint64_t used, std::uint64_t capacity)
{
    return std::to_string(capacity ? used * 100 / capacity : 0) + "%";
}

// One of a character model's meshes (Model::parts) as its file has it: its group and that group's state,
// its alpha and shadow bytes, whether the character shows it and whether this frame's draws posed it.
std::string characterMesh(const ModelPart& part, const CharacterInstance& character, bool shown, std::size_t partIndex)
{
    const bool drawn = partIndex < character.drawnParts.size() && character.drawnParts[partIndex];
    const auto groupState = part.group < character.groupStates.size() ? std::format("{:#04x}", character.groupStates[part.group]) : std::string("unread");
    return std::format("file mesh {} (group {} state {}, alpha {:#04x} shadow {:#04x}, {}, {} triangles, {}, {})", part.mesh, part.group, groupState,
        part.alphaBits, part.shadowBits, part.alphaTested ? "cut-out" : "opaque", part.indices.size() / 3, shown ? "shown" : "hidden",
        drawn ? "drawn" : "not drawn");
}

// Diagnostics: the character `id`'s shown mesh `shownIndex` (a geometry of its structure) as its model file has it.
std::string describeCharacterMesh(const State& s, std::uint64_t id, std::uint32_t shownIndex)
{
    const auto record = s.characters.find(id);
    const auto listed = std::ranges::find(s.characterList, id, &CharacterInstance::id);
    if (record == s.characters.end() || listed == s.characterList.end() || shownIndex >= record->second.parts.size()) return "";

    const auto model = s.models.find(record->second.hash);
    const auto partIndex = record->second.parts[shownIndex];
    if (model == s.models.end() || partIndex >= model->second.geometry->parts.size()) return "";

    return ", " + characterMesh(model->second.geometry->parts[partIndex], *listed, true, partIndex);
}

// Diagnostics: every character the game has within 2 cm of `position` (a character's parts share one
// place), each with all its model's meshes.
std::string charactersAt(const State& s, const std::array<float, 3>& position)
{
    std::string text;
    for (const auto& character : s.characterList)
    {
        const auto& t = character.transform;
        if (std::abs(t[3] - position[0]) > 0.02f || std::abs(t[7] - position[1]) > 0.02f || std::abs(t[11] - position[2]) > 0.02f) continue;

        const auto model = s.models.find(character.hash);
        const auto record = s.characters.find(character.id);
        text += std::format("\n  character {:x} (id {:x}){}:", character.hash, character.id,
            record == s.characters.end() ? ", not in the scene" : record->second.traced ? "" : ", not traced this frame");
        if (model == s.models.end())
        {
            text += " its model not read yet";
            continue;
        }

        const auto& parts = model->second.geometry->parts;
        for (std::size_t i = 0; i < parts.size(); ++i)
        {
            const bool shown = record != s.characters.end() && std::ranges::find(record->second.parts, static_cast<std::uint32_t>(i)) != record->second.parts.end();
            text += (i ? "; " : " ") + characterMesh(parts[i], character, shown, i);
        }

        for (const auto& mesh : model->second.geometry->leftOut)
            text += std::format("; file mesh {} left out (alpha {:#04x} shadow {:#04x})", mesh.mesh, mesh.alphaBits, mesh.shadowBits);
    }

    return text;
}

// The frame's instance of one of the game's models, or null.
const InstancedModel* instancedAt(std::span<const InstancedModel> instancedModels, std::uint32_t instance)
{
    const auto placed = std::lower_bound(instancedModels.begin(), instancedModels.end(), instance,
        [](const InstancedModel& model, std::uint32_t value) { return model.instance < value; });
    return placed == instancedModels.end() || placed->instance != instance ? nullptr : &*placed;
}

// A hit the probe lists (GIShadowProbe::lightNearest) as text: what it hit, which side, how far.
std::string describeHit(const State& s, std::span<const InstancedModel> instancedModels, const uint4& hit)
{
    if (!hit.x) return "nothing";

    const auto instance = hit.x - 1, geometry = hit.y & 0x7FFFFFFFu, kind = hit.z;
    std::string what = "the terrain";
    if (kind & GI_INSTANCE_MODEL)
    {
        const auto placed = instancedAt(instancedModels, instance);
        if (!placed) what = "one of the game's models";
        else if (kind & GI_INSTANCE_CHARACTER)
            // A character's geometries are the meshes it shows, not its model's.
            what = std::format("character {:x} at ({:.1f} {:.1f} {:.1f}), its shown mesh {}{}", placed->hash, placed->position[0], placed->position[1],
                placed->position[2], geometry, describeCharacterMesh(s, placed->character, geometry));
        else
        {
            what = std::format("model {:x} placed at ({:.1f} {:.1f} {:.1f})", placed->hash, placed->position[0], placed->position[1], placed->position[2]);
            const auto found = s.models.find(placed->hash);
            if (found != s.models.end() && geometry < found->second.geometry->parts.size())
            {
                const auto& part = found->second.geometry->parts[geometry];
                what += std::format(", mesh {} ({}, {} triangles)", part.mesh, part.leaf ? "leaves" : part.alphaTested ? "cut-out" : "opaque",
                    part.indices.size() / 3);
            }
        }
    }

    return std::format("{} at {:.2f} m, its {} face (instance {})", what, std::bit_cast<float>(hit.w), (hit.y >> 31) ? "front" : "back", instance);
}

// The probe as text, and a key that changes when what blocks its ray changes.
std::pair<std::string, std::string> describeProbe(const State& s, std::span<const InstancedModel> instancedModels, const GIShadowProbe& probe)
{
    const auto stats = s.scene->stats();
    const auto scene = std::format("scene: {} instances; geometry pool {}, structures {}, {} structures waiting to be built", s.instances,
        percentOf(stats.poolUsed, stats.poolCapacity), percentOf(stats.blasUsed, stats.blasCapacity), stats.waiting);
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

    // Where the rays start: the surface they leave, how it faces the sun, which side of it they leave
    // and how far off it, and whatever that lift passes through.
    const auto dot = [](const float4& a, const float4& b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
    const auto& start = probe.start;
    const auto& lift = probe.lift;
    const auto& sh = probe.startHit;
    const bool exact = start.w > 0.5f, farSide = lift.w > 0.5f;
    text += std::format("the rays start on {}, its normal ({:.2f} {:.2f} {:.2f}) towards the eye, the sun at cosine {:.2f} to it (to the G-buffer's "
        "normal {:.2f}); they leave its {} side, lifted {:.1f} mm ({:.1f} mm along its normal); the lift passes through {}\n",
        exact ? std::format("the scene's own surface (instance {}, geometry {}, triangle {})", sh.x - 1, sh.y, sh.z)
              : std::string("the depth's point (the scene's surface not found there)"),
        start.x, start.y, start.z, dot(start, probe.ray), dot(probe.normal, probe.ray), farSide ? "far" : "sunward",
        std::sqrt(dot(lift, lift)) * 1000.0f, dot(lift, start) * 1000.0f,
        probe.crossed.x ? std::format("{} ({:.1f} mm from the start)", describeHit(s, instancedModels, probe.crossed), std::bit_cast<float>(probe.crossed.w) * 1000.0f)
                        : std::string("nothing"));

    std::string key = std::format("{}|{}|{}|{}", h.y || h.z ? "blocked" : "lit", h.w ? std::to_string(probe.hitGeometry.x) : "-", farSide, probe.crossed.x);
    if (h.w)
    {
        const auto& g = probe.hitGeometry;
        const auto kind = g.y & 0xFFFFu, flags = g.y >> 16;
        const bool startsMesh = exact && sh.x == h.w && sh.y == (g.z & 0x7FFFFFFFu);
        text += std::format("with every instance: hits {}, geometry {} ({}{}{}), triangle {}{}", describeHit(s, instancedModels, uint4{h.w, g.z, kind, g.w}), g.x,
            (flags & GI_GEOMETRY_CUT_OUT) ? "cut-out" : "opaque", (flags & GI_GEOMETRY_ALPHA_TEXTURE) ? " with the game's alpha" : "",
            (flags & GI_GEOMETRY_LEAF) ? ", leaves" : "", sh.w,
            !startsMesh ? "" : sh.w == sh.z ? ", the start's own triangle" : ", of the start's own mesh");
        if (kind & GI_INSTANCE_TERRAIN)
            text += std::format("; its triangle's highest corner at {:.1f} m as its ring was made, the heightmap {:.1f} m there now (level of detail {:.0f})",
                probe.terrain.x, probe.terrain.y, probe.terrain.z);
        text += "\n";
    }
    else
        text += "with every instance: nothing in the scene blocks it\n";

    const auto& g = probe.lightRays;
    text += std::format("its global illumination rays ({}): {} hit within 1 m, {} within 10 m, {} get away, {} hit a back face; the nearest hit: {}\n",
        GI_PROBE_LIGHT_RAYS, g.x, g.y, g.z, g.w, describeHit(s, instancedModels, probe.lightNearest));
    text += std::format("the camera's ray (the scene view's): {}; the game's surface at {:.2f} m\n", describeHit(s, instancedModels, probe.view), probe.normal.w);
    key += std::format("|{}", probe.view.x);

    // The characters where the sun ray's blocker is one, else where the camera's ray meets one: a
    // character's parts are models of their own in one place.
    const auto blocker = h.w && (probe.hitGeometry.y & GI_INSTANCE_CHARACTER) ? instancedAt(instancedModels, h.w - 1) : nullptr;
    const auto viewed = probe.view.x && (probe.view.z & GI_INSTANCE_CHARACTER) ? instancedAt(instancedModels, probe.view.x - 1) : nullptr;
    if (const auto character = blocker ? blocker : viewed)
        text += std::format("the characters at {}'s place, every mesh of each:{}\n", blocker ? "the blocker" : "the camera ray's hit",
            charactersAt(s, character->position));

    text += std::format("frame {}; the terrain rings last made at frames", static_cast<std::uint32_t>(probe.terrain.w));
    for (const auto& ring : s.terrain) text += " " + std::to_string(ring.refreshed);
    text += "\n";

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
    text += std::format("the game's models whose bounds the ray crosses: {}{}", crossed.size(),
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
    const D3D12_RANGE none{};
    s.probeReadback[slot]->Unmap(0, &none);

    auto [text, key] = describeProbe(s, s.instancedModels[slot], probe);
    const auto now = GetTickCount64();
    std::lock_guard lock(probeMutex);
    probeText = text;
    if constexpr (DiagnosticLog)
    {
        if (key != lastProbeKey && now - lastProbeLog >= 200)
        {
            lastProbeKey = key;
            lastProbeLog = now;
            log("Shadow probe:\n" + text);
        }
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
        + std::to_string(t.waiting / t.frames) + " waiting, " + std::to_string(t.compactions) + " compacted in all. CPU a frame: " + milliseconds(t.cpuMilliseconds / t.frames)
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
    if constexpr (DiagnosticLog) logStats(s);
    readTimes(s, slot);
    readProbe(s, slot);
    lap(ProbeStep);
    if constexpr (DiagnosticLog) logTally(s);

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

    const bool traceLight = frame.traceLight && s.light;
    const bool traceShadows = frame.traceShadows && s.shadows && frame.textures.shadow;
    const bool debugShadows = traceShadows && frame.debugShadows && frame.textures.shadowDebug;
    const bool traceLocalLights = !frame.localLights.empty() && frame.textures.localLightShadow;
    if (traceLocalLights && (s.localShadowWidth != frame.width || s.localShadowHeight != frame.height))
    {
        waitForAll();
        const auto texture = [&](const char* what) {
            return createTexture(s.device.Get(), frame.width, frame.height, DXGI_FORMAT_R16G16_FLOAT, 1, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, what);
        };
        s.localShadowRays = texture("Creating the lights' shadow rays");
        s.localShadowFirst = texture("Creating the lights' shadow filter");
        s.localShadowWidth = frame.width;
        s.localShadowHeight = frame.height;
    }
    if (!debugShadows)
    {
        std::lock_guard lock(probeMutex);
        probeText.clear();
    }

    // ---- The game's models and characters ----
    updateModels(s, frame, result);
    std::vector<GISkinRecord> skins;
    std::vector<float> palettes;
    updateCharacters(s, frame, skins, palettes);
    lap(ModelsStep);

    // ---- The game's terrain ----
    std::array<bool, GI_TERRAIN_RINGS> remadeRing{};
    const bool terrain = frame.terrain.has_value();
    if (terrain) planTerrain(s, frame, remadeRing);

    // New geometry starts what was learnt about it over (ClearNewGeometries).
    const auto newGeometries = scene.takeNewGeometries();

    scene.planBuilds();

    check(s.allocators[slot]->Reset(), "Resetting a command allocator");
    check(s.list->Reset(s.allocators[slot].Get(), nullptr), "Resetting the command list");
    auto& list = *s.list.Get();
    try
    {
        auto* depth = openShared(s, s.depth, frame.textures.depth, "Opening the depth").Get();
        auto* normal = openShared(s, s.normal, frame.textures.normal, "Opening the normals").Get();
        auto* albedo = openShared(s, s.albedo, frame.textures.albedo, "Opening the albedo").Get();
        auto* output = openShared(s, s.output, frame.textures.output, "Opening the output").Get();
        auto* shadow = traceShadows ? openShared(s, s.shadow, frame.textures.shadow, "Opening the shadow").Get() : nullptr;
        auto* shadowDebug = debugShadows ? openShared(s, s.shadowDebug, frame.textures.shadowDebug, "Opening the shadow debug view").Get() : nullptr;
        auto* lightDebug = traceLight && frame.textures.lightDebug
            ? openShared(s, s.lightDebug, frame.textures.lightDebug, "Opening the light's debug view").Get()
            : nullptr;
        auto* velocity = (traceShadows || traceLight) && frame.textures.velocity
            ? openShared(s, s.velocity, frame.textures.velocity, "Opening the velocity").Get()
            : nullptr;
        auto* localLightShadow = traceLocalLights
            ? openShared(s, s.localLightShadow, frame.textures.localLightShadow, "Opening the lights' shadow").Get()
            : nullptr;
        auto* terrainPages = terrain ? openShared(s, s.terrainPages, frame.terrain->pages, "Opening the terrain's page table").Get() : nullptr;
        auto* terrainHeights = terrain ? openShared(s, s.terrainHeights, frame.terrain->heights, "Opening the terrain's heights").Get() : nullptr;

        // ---- The frame's instances: the game's models where the game has them (on screen or not), its
        // characters, its terrain ----
        std::uint64_t triangles{};
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
        std::uint32_t charactersTraced{};
        std::uint64_t characterVertices{}, characterTriangles{};
        for (const auto& [id, character] : s.characters)
        {
            if (!character.traced) continue;

            const auto instance = scene.addInstance(character.blas, character.transform.data(), GI_INSTANCE_MODEL | GI_INSTANCE_CHARACTER);
            if (!instance) continue;

            if (debugShadows)
                instancedModels.push_back({*instance, character.hash, {character.transform[3], character.transform[7], character.transform[11]}, id});

            ++charactersTraced;
            const auto& parts = s.models.at(character.hash).parts;
            for (const auto i : character.parts)
            {
                characterVertices += parts[i].vertexCount;
                characterTriangles += parts[i].indexCount / 3;
            }
        }

        triangles += characterTriangles;
        s.charactersTraced = charactersTraced;
        s.characterVertices = characterVertices;
        s.characterTriangles = characterTriangles;
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

        const auto instanceCount = scene.instanceCount();
        s.instances = instanceCount;
        s.triangles = triangles;
        lap(InstancesStep);

        Uploads uploads{s.uploadData[slot], s.upload[slot]->GetGPUVirtualAddress()};
        const auto newGeometryCount = static_cast<std::uint32_t>(newGeometries.size());
        const auto newGeometriesOffset = uploads.reserve(4, newGeometryCount);
        std::memcpy(uploads.data + newGeometriesOffset, newGeometries.data(), newGeometries.size() * 4);
        const auto skinCount = static_cast<std::uint32_t>(skins.size());
        const auto skinVertices = skins.empty() ? 0u : skins.back().firstVertex + skins.back().vertexCount;
        const auto skinOffset = uploads.reserve(sizeof(GISkinRecord), skinCount);
        std::memcpy(uploads.data + skinOffset, skins.data(), skins.size() * sizeof(GISkinRecord));
        const auto paletteRows = palettes.size() / 4;
        const auto paletteOffset = uploads.reserve(16, paletteRows);
        std::memcpy(uploads.data + paletteOffset, palettes.data(), palettes.size() * sizeof(float));

        const auto parity = frame.frameIndex & 1;
        const auto constantsOffset = uploads.reserve(256, (sizeof(GIFrameConstants) + 255) / 256);
        auto constants = constantsOf(frame, newGeometryCount, instanceCount);
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
        constants.objectMotion = {velocity ? 1u : 0u, 0, 0, 0};
        constants.shadowDiagnostics = {frame.shadowsFromDepth ? 1u : 0u, 0, 0, 0};
        constants.lightDiagnostics = {frame.opaqueLeaves ? 1u : 0u, 0, lightDebug ? 1u : 0u, 0};
        constants.lightRays = {frame.lightRays, 0, 0, 0};
        std::memcpy(uploads.data + constantsOffset, &constants, sizeof(constants));

        // ---- Descriptors ----
        Descriptors descriptors(s.device.Get(), s.heap.Get(), s.descriptorSize, slot);
        descriptors.texture(0, depth, DXGI_FORMAT_R32_FLOAT);
        descriptors.texture(1, normal, DXGI_FORMAT_R10G10B10A2_UNORM);
        descriptors.texture(2, albedo, DXGI_FORMAT_R8G8B8A8_UNORM);
        descriptors.buffer(4, s.upload[slot].Get(), newGeometriesOffset, 4, newGeometryCount);
        descriptors.buffer(12, s.upload[slot].Get(), skinOffset, sizeof(GISkinRecord), skinCount);
        descriptors.buffer(13, s.upload[slot].Get(), paletteOffset, 16, paletteRows);
        descriptors.buffer(5, scene.instanceBuffer(), 0, sizeof(GIInstanceDesc), instanceCount);
        descriptors.rawBuffer(6, scene.pool(), gi_scene::Scene::PoolBytes);
        descriptors.buffer(16, scene.geometryTable(), 0, sizeof(GIGeometry), MaxGeometries);
        descriptors.outputTexture(0, output, DXGI_FORMAT_R16G16B16A16_FLOAT);
        descriptors.outputRawBuffer(1, scene.pool(), gi_scene::Scene::PoolBytes);
        descriptors.outputBuffer(4, s.counters.Get(), 4, GI_COUNTER_COUNT);
        descriptors.outputBuffer(5, s.dispatchArguments.Get(), 16, GI_DISPATCH_SLOTS);
        descriptors.outputBuffer(6, s.geometryAlbedo.Get(), 16, MaxGeometries);
        if (terrain)
        {
            descriptors.texture(14, terrainPages, frame.terrain->pagesFormat);
            descriptors.texture(15, terrainHeights, frame.terrain->heightsFormat);
        }
        if (velocity) descriptors.texture(17, velocity, frame.textures.velocityFormat);
        if (debugShadows)
        {
            descriptors.outputBuffer(77, s.probe.Get(), sizeof(GIShadowProbe), 1);
            descriptors.outputTexture(78, shadowDebug, DXGI_FORMAT_R8G8B8A8_UNORM);
        }
        if (lightDebug) descriptors.outputTexture(79, lightDebug, DXGI_FORMAT_R8G8B8A8_UNORM);
        if (traceLight) s.light->describe(descriptors, parity);
        if (traceShadows) s.shadows->describe(descriptors, parity, shadow);
        if (traceLocalLights)
        {
            descriptors.outputTexture(89, localLightShadow, DXGI_FORMAT_R16_FLOAT);
            descriptors.outputTexture(90, s.localShadowRays.Get(), DXGI_FORMAT_R16G16_FLOAT);
            descriptors.outputTexture(91, s.localShadowFirst.Get(), DXGI_FORMAT_R16G16_FLOAT);
        }

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
        recorder.rebinder = [&] {
            list.SetDescriptorHeaps(1, heaps);
            list.SetComputeRootSignature(s.rootSignature.Get());
            list.SetComputeRootConstantBufferView(0, uploads.address + constantsOffset);
            list.SetComputeRootDescriptorTable(2, descriptors.srvTable());
            list.SetComputeRootDescriptorTable(3, descriptors.uavTable());
            list.SetComputeRootShaderResourceView(4, scene.topLevel()->GetGPUVirtualAddress());
            list.SetComputeRootDescriptorTable(5, textureRange);
        };
        recorder.marker = [&](const char* phase, std::optional<performance::Pass> pass) {
            if (marks.size() >= MaxMarks) return;

            list.EndQuery(s.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * MaxMarks + static_cast<UINT>(marks.size()));
            marks.push_back({phase ? phase : "", pass});
        };

        recorder.mark("scene writes", performance::Pass::RayTracedScene);
        D3D12_RESOURCE_BARRIER begin[]{
            transition(depth, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            transition(normal, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            transition(albedo, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            transition(output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        };
        list.ResourceBarrier(static_cast<UINT>(std::size(begin)), begin);
        if (traceShadows) recorder.transition(shadow, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (debugShadows) recorder.transition(shadowDebug, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (lightDebug) recorder.transition(lightDebug, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (velocity) recorder.transition(velocity, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (traceLocalLights) recorder.transition(localLightShadow, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (terrain)
        {
            recorder.transition(terrainPages, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            recorder.transition(terrainHeights, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }

        // The pool's uploads and the table's rows, then the geometry the GPU makes: the terrain, the
        // characters' poses.
        scene.recordWrites(list);
        if (newGeometryCount) recorder.dispatch(Kernel::ClearNewGeometries, groups(newGeometryCount, 64));

        for (std::uint32_t r = 0; r < GI_TERRAIN_RINGS; ++r)
            if (remadeRing[r]) recorder.dispatch(Kernel::TerrainGrid, groups(GI_TERRAIN_CELLS, 8), groups(GI_TERRAIN_CELLS, 8), {r});

        if (skinCount) recorder.dispatch(Kernel::Skin, groups(skinVertices, 64), 1, {skinVertices, skinCount});

        recorder.mark("bottom-level structures", performance::Pass::RayTracedScene);
        scene.recordBottomLevels(list);

        // The instances, then the top level over the bottom levels.
        recorder.mark("instances and top level", performance::Pass::RayTracedScene);
        scene.recordInstances(list);
        scene.recordTopLevel(list);

        // ---- The shadows, then the light or the debug view ----
        if (traceLocalLights)
        {
            recorder.mark("shadow rays for lights", performance::Pass::LightShadowRays);
            recorder.dispatch(Kernel::TraceLocalLightShadows, groups(frame.width, 8), groups(frame.height, 8));
            recorder.barrier();
            recorder.mark("shadow filter for lights", performance::Pass::LightShadowFilter);
            for (std::uint32_t pass = 0; pass < 3; ++pass)
            {
                recorder.dispatch(Kernel::FilterLocalLightShadows, groups(frame.width, 8), groups(frame.height, 8), {pass, 1u << pass});
                recorder.barrier();
            }
        }
        if (traceShadows) s.shadows->record(recorder);
        if (debugShadows)
        {
            recorder.mark("debug view and probe");
            recorder.dispatch(Kernel::DebugShadows, groups(frame.width, 8), groups(frame.height, 8));
            recorder.dispatch(Kernel::ProbeShadows, 1, 1);
            recorder.barrier();
            recorder.transition(s.probe.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list.CopyBufferRegion(s.probeReadback[slot].Get(), 0, s.probe.Get(), 0, sizeof(GIShadowProbe));
            recorder.transition(s.probe.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            s.probeWritten[slot] = true;
        }
        if (frame.showScene)
        {
            recorder.mark("scene view");
            recorder.dispatch(Kernel::DebugScene, groups(frame.width, 8), groups(frame.height, 8));
        }
        else if (traceLight) s.light->record(recorder, parity, denoiserCamera(frame), slot);

        if constexpr (DiagnosticLog)
        {
            if (traceLight && !frame.showScene) copyStats(s, recorder, output, frame);
        }

        D3D12_RESOURCE_BARRIER end[]{
            uavBarrier(),
            transition(depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
            transition(normal, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
            transition(albedo, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
            transition(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON),
        };
        list.ResourceBarrier(static_cast<UINT>(std::size(end)), end);
        if (traceShadows) recorder.transition(shadow, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        if (debugShadows) recorder.transition(shadowDebug, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        if (lightDebug) recorder.transition(lightDebug, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        if (velocity) recorder.transition(velocity, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        if (traceLocalLights) recorder.transition(localLightShadow, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
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

    const auto compactedShare = stats.compactedFrom ? std::to_string(stats.compactedTo * 100 / stats.compactedFrom) + "%" : std::string("-");
    return std::to_string(s.instances) + " instances, " + std::to_string(s.triangles / 1000) + "k triangles; geometry pool "
        + percentOf(stats.poolUsed, stats.poolCapacity) + ", structures " + percentOf(stats.blasUsed, stats.blasCapacity) + " (" + std::to_string(stats.structures)
        + ", " + std::to_string(stats.compacted) + " compacted to " + compactedShare + " of their size, " + std::to_string(stats.waiting) + " waiting), "
        + std::to_string(stats.geometriesUsed) + " geometries; GPU: scene " + milliseconds(s.sceneMilliseconds) + ", light " + milliseconds(s.lightMilliseconds)
        + ", shadows " + milliseconds(s.shadowMilliseconds) + "; the game's models: " + std::to_string(s.modelInstanceCount) + " instances ("
        + std::to_string(s.modelTriangles / 1000) + "k triangles) of " + std::to_string(s.models.size()) + " models, " + std::to_string(s.modelsWaiting)
        + " waiting for room" + (s.modelsTooLarge ? ", " + std::to_string(s.modelsTooLarge) + " too large" : "") + ", "
        + std::to_string(GI_MAX_TEXTURES - s.freeTextureIndices.size()) + " of the game's textures held; characters: " + std::to_string(s.charactersTraced)
        + " traced (" + std::to_string(s.characterVertices / 1000) + "k vertices posed, " + std::to_string(s.characterTriangles / 1000) + "k triangles), "
        + std::to_string(s.charactersWaiting) + " waiting; terrain rings " + std::to_string(s.terrainRings) + " of " + std::to_string(GI_TERRAIN_RINGS);
}
}
