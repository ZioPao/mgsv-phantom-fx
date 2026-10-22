// Captures the game's meshes for the global illumination; gi_capture.hpp describes the route.
#include "gi_capture.hpp"

#include "log.hpp"
#include "performance.hpp"
#include "range_allocator.hpp"
#include "shader_bytecode.hpp"
#include "shaders/gi_shared.h"

#include <Windows.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace phantom_fx::gi_capture
{
namespace
{
using Microsoft::WRL::ComPtr;

// What a vertex shader draws, from its source name (docs/DEVELOPMENT.md, "Capture").
enum class Kind : std::uint8_t
{
    None,
    Mesh,    // A rigid mesh, its own or instanced: captured once
    Terrain, // A terrain patch: a mesh placed by m_localParam as well
    Skinned, // Reads the bones (b6): captured each frame, into the frame's skinned meshes
};

constexpr UINT SceneSlot = 2, ObjectSlot = 5, BonesSlot = 6;
// The constant buffers whose contents are kept: scene (b2), object (b5), light (b3), probes (b7).
constexpr UINT SceneConstantsSize = 480, ObjectConstantsSize = 208, LightConstantsSize = 176, ProbeConstantsSize = 576;
constexpr UINT LargestConstants = ProbeConstantsSize / 4;
// The stream output ring, in vertices of 16 bytes (clip positions).
constexpr std::uint32_t RingVertices = 2u << 20;
// A draw with more vertices than this is left out.
constexpr std::uint32_t MaxDrawVertices = 1u << 20;

// Turns the captured clip positions into world positions in the transfer texture, one thread per
// vertex. Each record's inverse is that of the projection-view its draw was made with.
constexpr char ConvertSource[] = R"(
struct Record
{
    uint ringOffset;
    uint vertexCount;
    uint transferOffset;
    uint padding;
    row_major float4x4 inverse;
};

StructuredBuffer<Record> g_Records : register(t0);
Buffer<float4> g_Ring : register(t1);
RWTexture2D<float4> g_Transfer : register(u0);

cbuffer Parameters : register(b0)
{
    uint g_RecordCount;
    uint g_VertexCount;
    uint2 g_Padding;
};

[numthreads(64, 1, 1)]
void main(uint id : SV_DispatchThreadID)
{
    if (id >= g_VertexCount) return;

    uint low = 0, high = g_RecordCount;
    while (high - low > 1)
    {
        uint middle = (low + high) / 2;
        if (g_Records[middle].transferOffset <= id) low = middle;
        else high = middle;
    }

    Record record = g_Records[low];
    float4 clip = g_Ring[record.ringOffset + id - record.transferOffset];
    float4 world = mul(record.inverse, clip);
    g_Transfer[uint2(id % 4096, id / 4096)] = float4(world.xyz / world.w, 1);
}
)";
static_assert(GI_TRANSFER_WIDTH == 4096);

struct ConvertRecord
{
    std::uint32_t ringOffset, vertexCount, transferOffset, padding;
    float inverse[16];
};

struct VertexShader
{
    Kind kind{};
    bool alphaTested{}; // See alphaTestedSource
    std::vector<std::uint8_t> code;
    ComPtr<ID3D11GeometryShader> streamOutput;
    bool failed{};
};

// A draw captured on a deferred context, waiting for its list to execute and the next kick.
struct Record
{
    std::uint32_t entry{};
    std::uint64_t ringOffset{};
    std::uint32_t vertexCount{};
    std::array<float, 16> inverse{};
    bool dynamic{};
};

struct Pending
{
    std::vector<Record> records;
    std::vector<std::uint32_t> drawn;
    std::vector<std::uint32_t> alphaTested;
    std::optional<camera::SceneConstants> camera;
};

enum class EntryState : std::uint8_t { Free, Capturing, Live };

struct Entry
{
    std::uint64_t key{};
    EntryState state{};
    bool alphaTested{};
    // Diagnostics (nearCopies): a mesh's draw without its placement, and its origin in worldOf's units.
    std::uint64_t mesh{};
    std::array<std::int64_t, 3> origin{};
};

std::atomic<bool> active{}, capturing{true};

// Known vertex shaders, by the object the game binds. Holding them keeps their addresses from being reused.
std::shared_mutex shaderMutex;
std::unordered_map<IUnknown*, VertexShader> shaders;
std::vector<ComPtr<IUnknown>> shaderOwners;

// The shader the thread's context bound last, and what it draws.
thread_local ID3D11DeviceContext* boundContext{};
thread_local Kind boundKind{};
thread_local bool boundAlphaTested{};
thread_local IUnknown* boundShader{};

// What the thread last wrote to each kept constant buffer; for scene constants, whether they are the
// main camera's, worked out once a write.
struct Written
{
    std::array<float, LargestConstants> values{};
    bool mainCamera{};
    std::array<double, 16> viewInverse{}; // Scene constants: the inverse of m_view (c8 to c11)
};

thread_local ID3D11Resource* mappedBuffer{};
thread_local void* mappedData{};
thread_local UINT mappedSize{};
thread_local std::unordered_map<ID3D11Resource*, Written> contents;

// Captures and draws by deferred context while recorded, by command list once finished, and those
// of the lists executed since the last kick.
std::mutex recordMutex;
std::unordered_map<std::uint64_t, Pending> pending;
Pending executed;

std::mutex entryMutex;
std::unordered_map<std::uint64_t, std::uint32_t> byKey;
std::vector<Entry> entries(gi_renderer::MaxEntries);
std::vector<std::uint32_t> freeIds;
RangeAllocator ring{RingVertices};
std::uint32_t liveEntries{};
// The skinned meshes captured since the last kick, by key: a mesh the main view draws twice is
// captured once.
std::unordered_set<std::uint64_t> skinnedThisFrame;

std::mutex resourceMutex;
ComPtr<ID3D11Device> device;
ComPtr<ID3D11Buffer> ringBuffer, recordBuffer, parameterBuffer;
ComPtr<ID3D11ShaderResourceView> ringView, recordView;
ComPtr<ID3D11ComputeShader> convertShader;
UINT recordCapacity{};

std::atomic<std::uint64_t> captures{}, skinnedCaptures{}, missingScene{}, missingObject{}, dynamicInstances{}, ringFull{};

// Diagnostics: the hooks' time in performance counter ticks, and the draws and captures, since takeWork.
std::atomic<std::int64_t> hookTicks{};
std::atomic<std::uint64_t> workDraws{}, workCaptures{}, workSkinned{}, workVertices{};

// Adds the time from its construction to its destruction to hookTicks.
struct HookTimer
{
    LARGE_INTEGER start{};

    HookTimer() { QueryPerformanceCounter(&start); }
    ~HookTimer()
    {
        LARGE_INTEGER end{};
        QueryPerformanceCounter(&end);
        hookTicks += end.QuadPart - start.QuadPart;
    }
};

void check(HRESULT result, const char* what)
{
    if (FAILED(result)) throw std::runtime_error(std::string(what) + " failed (" + hex(static_cast<std::uint32_t>(result)) + ").");
}

Kind kindOf(std::string_view name, const void* code, std::size_t size)
{
    const bool terrain = name.starts_with("NewTerrain_Depth") || name.starts_with("NewTerrain_Shadow");
    // Lamps' paraboloid shadows are left out: no matrix takes their positions back to the world.
    const bool model = (name.starts_with("fox3ddf_") || name.starts_with("tpp3ddf_") || name.starts_with("ModelForward_Shadow"))
        && name.find("decal") == name.npos && name.find("Paraboloid") == name.npos;
    if (!terrain && !model) return Kind::None;
    if (terrain) return Kind::Terrain;

    ComPtr<ID3D11ShaderReflection> reflection;
    if (FAILED(D3DReflect(code, size, IID_PPV_ARGS(&reflection)))) return Kind::Mesh;

    D3D11_SHADER_DESC desc{};
    reflection->GetDesc(&desc);
    for (UINT i = 0; i < desc.BoundResources; ++i)
    {
        D3D11_SHADER_INPUT_BIND_DESC binding{};
        if (SUCCEEDED(reflection->GetResourceBindingDesc(i, &binding)) && binding.Type == D3D_SIT_CBUFFER && binding.BindPoint == BonesSlot)
            return Kind::Skinned;
    }

    return Kind::Mesh;
}

// Whether a vertex shader draws meshes whose texture cuts holes in them, which their captured
// triangles lack (docs/DEVELOPMENT.md, "Ray-traced shadows"). The sun's shadow map draws those with
// ModelForward_Shadow, whose pixel shader discards, and the rest with ModelForward_ShadowOpaque and
// ModelForward_ShadowOpaqueRigid, which do not; the G-buffer draws foliage and hair with these. Every
// G-buffer pixel shader discards, so the pixel shaders tell nothing.
bool alphaTestedSource(std::string_view name)
{
    if (name == "ModelForward_Shadow") return true;

    for (const auto part : {"leaf", "wave", "translucent", "hair", "Hair"})
        if (name.find(part) != name.npos) return true;

    return false;
}

// FNV-1a over the draw's identity.
struct Hasher
{
    std::uint64_t value = 14695981039346656037ull;

    void add(const void* data, std::size_t size)
    {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        for (std::size_t i = 0; i < size; ++i) value = (value ^ bytes[i]) * 1099511628211ull;
    }

    template<class T> void add(const T& value) { add(&value, sizeof(value)); }
};

std::array<double, 16> inverseOf(const float* rows)
{
    std::array<double, 16> matrix{}, result{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    for (int i = 0; i < 16; ++i) matrix[i] = rows[i];

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

    return result;
}

std::array<float, 16> inverse(const float* rows)
{
    const auto result = inverseOf(rows);
    std::array<float, 16> single{};
    for (int i = 0; i < 16; ++i) single[i] = static_cast<float>(result[i]);

    return single;
}

// Where a mesh is, for its key: its world matrix (the inverse of the pass's m_view times the
// object's m_viewWorld, b5 c0 to c3), rows 0 to 2, rounded to 1/512 and to 2 cm so that the rounding
// of the game's own products does not change it from frame to frame.
std::array<std::int64_t, 12> worldOf(const std::array<double, 16>& viewInverse, const float* viewWorld)
{
    std::array<std::int64_t, 12> rounded{};
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 4; ++column)
        {
            double value{};
            for (int k = 0; k < 4; ++k) value += viewInverse[row * 4 + k] * viewWorld[k * 4 + column];
            rounded[row * 4 + column] = std::llround(value * (column == 3 ? 50.0 : 512.0));
        }

    return rounded;
}

// The stream output shader for a vertex shader: Direct3D 11 takes the vertex shader's own code for
// one without a geometry stage. Null when it cannot be made.
ID3D11GeometryShader* streamOutputOf(IUnknown* shader)
{
    {
        std::shared_lock lock(shaderMutex);
        const auto found = shaders.find(shader);
        if (found == shaders.end() || found->second.failed) return nullptr;
        if (found->second.streamOutput) return found->second.streamOutput.Get();
    }

    std::unique_lock lock(shaderMutex);
    auto& known = shaders[shader];
    if (known.streamOutput || known.failed) return known.streamOutput.Get();

    const D3D11_SO_DECLARATION_ENTRY position{0, "SV_Position", 0, 0, 4, 0};
    const UINT stride = 16;
    ComPtr<ID3D11Device> creator;
    {
        std::lock_guard resources(resourceMutex);
        creator = device;
    }

    if (!creator
        || FAILED(creator->CreateGeometryShaderWithStreamOutput(known.code.data(), known.code.size(), &position, 1, &stride, 1,
            D3D11_SO_NO_RASTERIZED_STREAM, nullptr, known.streamOutput.ReleaseAndGetAddressOf())))
    {
        known.failed = true;
        return nullptr;
    }

    return known.streamOutput.Get();
}

const Written* writtenTo(ID3D11Buffer* buffer)
{
    if (!buffer) return nullptr;

    const auto found = contents.find(buffer);
    return found != contents.end() ? &found->second : nullptr;
}

// The main camera's constants, with the add-on's jitter taken back out for the test.
bool mainCamera(const std::array<float, LargestConstants>& scene)
{
    camera::SceneConstants unjittered;
    std::memcpy(&unjittered, scene.data(), sizeof(unjittered));
    const float x = unjittered.c[28][0], y = unjittered.c[28][1];
    camera::applyJitter(unjittered, {-x, -y});
    unjittered.c[28][0] = unjittered.c[28][1] = 0;

    return camera::classify(unjittered) == camera::SceneView::MainCamera;
}

bool isDynamic(ID3D11Buffer* buffer)
{
    D3D11_BUFFER_DESC desc{};
    buffer->GetDesc(&desc);
    return desc.Usage == D3D11_USAGE_DYNAMIC;
}

// Frees an entry nothing on the GPU side holds; call with entryMutex held.
void freeEntry(std::uint32_t index)
{
    auto& entry = entries[index];
    if (entry.state == EntryState::Free) return;
    if (entry.state == EntryState::Live) --liveEntries;

    byKey.erase(entry.key);
    entry = {};
    freeIds.push_back(index);
}

// Forgets records whose ring space can go: their capture will not be converted. Call with
// entryMutex held.
void dropRecords(const std::vector<Record>& records)
{
    for (const auto& record : records)
    {
        ring.release(record.ringOffset, record.vertexCount);
        if (!record.dynamic && entries[record.entry].state == EntryState::Capturing) freeEntry(record.entry);
    }
}

void ensureConverter()
{
    if (convertShader) return;

    ComPtr<ID3DBlob> code, errors;
    const auto result = D3DCompile(ConvertSource, std::strlen(ConvertSource), "gi-convert", nullptr, nullptr, "main", "cs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(result))
        throw std::runtime_error("The capture converter did not compile: "
            + (errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : hex(static_cast<std::uint32_t>(result))));

    check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, convertShader.GetAddressOf()), "Creating the capture converter");

    D3D11_BUFFER_DESC parameters{};
    parameters.ByteWidth = 16;
    parameters.Usage = D3D11_USAGE_DYNAMIC;
    parameters.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    parameters.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    check(device->CreateBuffer(&parameters, nullptr, parameterBuffer.GetAddressOf()), "Creating the converter's constants");
}

// Call with resourceMutex held.
void ensureRing()
{
    if (ringBuffer || !device) return;

    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = RingVertices * 16;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_STREAM_OUTPUT | D3D11_BIND_SHADER_RESOURCE;
    check(device->CreateBuffer(&desc, nullptr, ringBuffer.GetAddressOf()), "Creating the capture ring");
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    view.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    view.Buffer.NumElements = RingVertices;
    check(device->CreateShaderResourceView(ringBuffer.Get(), &view, ringView.GetAddressOf()), "Creating the capture ring");
}

void ensureRecords(UINT count)
{
    if (recordBuffer && recordCapacity >= count) return;

    const UINT capacity = std::max(count, 4096u);
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = capacity * sizeof(ConvertRecord);
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    desc.StructureByteStride = sizeof(ConvertRecord);
    check(device->CreateBuffer(&desc, nullptr, recordBuffer.ReleaseAndGetAddressOf()), "Creating the capture records");
    check(device->CreateShaderResourceView(recordBuffer.Get(), nullptr, recordView.ReleaseAndGetAddressOf()), "Creating the capture records");
    recordCapacity = capacity;
}
}

void attach(ID3D11Device* gameDevice) noexcept
{
    std::lock_guard lock(resourceMutex);
    device = gameDevice;
}

void detach() noexcept
{
    active = false;
    {
        std::lock_guard lock(resourceMutex);
        ringBuffer.Reset();
        ringView.Reset();
        recordBuffer.Reset();
        recordView.Reset();
        parameterBuffer.Reset();
        convertShader.Reset();
        recordCapacity = 0;
        device.Reset();
    }

    {
        std::lock_guard lock(recordMutex);
        pending.clear();
        executed = {};
    }

    {
        std::lock_guard lock(entryMutex);
        byKey.clear();
        std::fill(entries.begin(), entries.end(), Entry{});
        freeIds.clear();
        ring.reset(RingVertices);
        liveEntries = 0;
    }

    std::unique_lock lock(shaderMutex);
    shaders.clear();
    shaderOwners.clear();
}

void setActive(bool on) noexcept
{
    if (on == active.load()) return;

    if (on)
    {
        try
        {
            std::lock_guard lock(resourceMutex);
            ensureRing();
        }
        catch (const std::exception& error)
        {
            log(std::string("The global illumination cannot capture the game's meshes: ") + error.what());
            return;
        }
    }

    // Off forgets the scene; on starts it over.
    std::scoped_lock lock(recordMutex, entryMutex);
    active = on;
    pending.clear();
    executed = {};
    byKey.clear();
    std::fill(entries.begin(), entries.end(), Entry{});
    freeIds.clear();
    for (std::uint32_t i = gi_renderer::DynamicEntry; i > 0; --i) freeIds.push_back(i - 1);
    ring.reset(RingVertices);
    liveEntries = 0;
    skinnedThisFrame.clear();
}

void setCapturing(bool on) noexcept
{
    capturing = on;
}

void vertexShaderCreated(const void* code, std::size_t size, IUnknown* shader) noexcept
{
    if (!code || !shader) return;

    const auto name = sourceName(std::string_view(static_cast<const char*>(code), size), "_vs.hlsl");
    const auto kind = kindOf(name, code, size);
    if (kind == Kind::None) return;

    try
    {
        std::unique_lock lock(shaderMutex);
        auto& known = shaders[shader];
        known.kind = kind;
        known.alphaTested = alphaTestedSource(name);
        known.code.assign(static_cast<const std::uint8_t*>(code), static_cast<const std::uint8_t*>(code) + size);
        known.streamOutput.Reset();
        known.failed = false;
        shaderOwners.emplace_back(shader);
    }
    catch (...)
    {
    }
}

void vertexShaderBound(ID3D11DeviceContext* context, IUnknown* shader) noexcept
{
    Kind kind{};
    bool alphaTested{};
    if (shader)
    {
        std::shared_lock lock(shaderMutex);
        const auto found = shaders.find(shader);
        if (found != shaders.end())
        {
            kind = found->second.kind;
            alphaTested = found->second.alphaTested;
        }
    }

    boundContext = context;
    boundShader = shader;
    boundKind = kind;
    boundAlphaTested = alphaTested;
}

void bufferMapped(ID3D11Resource* buffer, void* data) noexcept
{
    if (!active.load(std::memory_order_relaxed) || !data) return;

    D3D11_RESOURCE_DIMENSION dimension{};
    buffer->GetType(&dimension);
    if (dimension != D3D11_RESOURCE_DIMENSION_BUFFER) return;

    D3D11_BUFFER_DESC desc{};
    static_cast<ID3D11Buffer*>(buffer)->GetDesc(&desc);
    const bool kept = desc.ByteWidth == SceneConstantsSize || desc.ByteWidth == ObjectConstantsSize || desc.ByteWidth == LightConstantsSize
        || desc.ByteWidth == ProbeConstantsSize;
    if (!kept || !(desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER)) return;

    mappedBuffer = buffer;
    mappedData = data;
    mappedSize = desc.ByteWidth;
}

void bufferUnmapped(ID3D11Resource* buffer) noexcept
{
    if (!mappedBuffer || buffer != mappedBuffer) return;

    const HookTimer timer;
    try
    {
        auto& written = contents[buffer];
        std::memcpy(written.values.data(), mappedData, mappedSize);
        if (mappedSize == SceneConstantsSize)
        {
            written.mainCamera = mainCamera(written.values);
            written.viewInverse = inverseOf(written.values.data() + 32);
        }
    }
    catch (...)
    {
    }

    mappedBuffer = nullptr;
    mappedData = nullptr;
}

bool written(ID3D11Buffer* buffer, std::span<float> into) noexcept
{
    const auto* found = writtenTo(buffer);
    if (!found || into.size() > found->values.size()) return false;

    std::copy_n(found->values.begin(), into.size(), into.begin());
    return true;
}

void drawIndexed(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call) noexcept
{
    if (!active.load(std::memory_order_relaxed) || boundContext != context || boundKind == Kind::None || !call.count) return;

    const HookTimer timer;
    ++workDraws;
    try
    {
        ComPtr<ID3D11Buffer> indices;
        DXGI_FORMAT indexFormat{};
        UINT indexOffset{};
        context->IAGetIndexBuffer(indices.GetAddressOf(), &indexFormat, &indexOffset);
        D3D11_PRIMITIVE_TOPOLOGY topology{};
        context->IAGetPrimitiveTopology(&topology);
        if (!indices || (topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST && topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP)) return;

        // The game draws its meshes without a geometry or tessellation stage; stream output replaces none.
        ComPtr<ID3D11GeometryShader> geometry;
        ComPtr<ID3D11DomainShader> domain;
        context->GSGetShader(geometry.GetAddressOf(), nullptr, nullptr);
        context->DSGetShader(domain.GetAddressOf(), nullptr, nullptr);
        if (geometry || domain) return;

        std::array<ComPtr<ID3D11Buffer>, 2> vertices;
        UINT strides[2]{}, offsets[2]{};
        ID3D11Buffer* rawVertices[2]{};
        context->IAGetVertexBuffers(0, 2, rawVertices, strides, offsets);
        for (int i = 0; i < 2; ++i) vertices[i].Attach(rawVertices[i]);

        std::array<ComPtr<ID3D11Buffer>, 4> constants; // b2 to b5
        ID3D11Buffer* rawConstants[4]{};
        context->VSGetConstantBuffers(SceneSlot, 4, rawConstants);
        for (int i = 0; i < 4; ++i) constants[i].Attach(rawConstants[i]);

        const auto* scene = writtenTo(constants[0].Get());
        if (!scene)
        {
            ++missingScene;
            return;
        }

        // Skinned meshes are captured from the main camera's pass only: the shadow passes draw them
        // again (and sometimes at another level of detail, a second copy in the scene).
        const bool dynamic = boundKind == Kind::Skinned;
        if (dynamic && !scene->mainCamera) return;

        // Alpha-tested skinned meshes (hair) are left out: the frame's skinned meshes share one
        // structure, so they cannot be told apart for the shadow rays, which leave alpha-tested
        // meshes to the game's shadow map.
        if (dynamic && boundAlphaTested) return;

        // Instances from a buffer the game rewrites each frame (grass) are not kept.
        if (call.instances > 1 && (!vertices[1] || isDynamic(vertices[1].Get())))
        {
            ++dynamicInstances;
            return;
        }

        const std::uint64_t triangles = topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST ? call.count / 3 : (call.count > 2 ? call.count - 2 : 0);
        const auto vertexCount = static_cast<std::uint32_t>(std::min<std::uint64_t>(triangles * 3 * std::max(call.instances, 1u), MaxDrawVertices + 1));
        if (!vertexCount || vertexCount > MaxDrawVertices) return;

        Hasher hasher;
        hasher.add(indices.Get());
        hasher.add(indexFormat);
        hasher.add(indexOffset);
        hasher.add(call.first);
        hasher.add(call.count);
        hasher.add(call.vertexOffset);
        hasher.add(call.instances);
        hasher.add(call.firstInstance);
        hasher.add(topology);
        for (int i = 0; i < 2; ++i)
        {
            hasher.add(vertices[i].Get());
            hasher.add(offsets[i]);
        }

        // Where this copy of the mesh is (worldOf), and whether it is skinned (m_useWeightCount, c8.x);
        // for a terrain patch, also its placement (m_localParam, c9 to c11). Not the object's m_world
        // (c4 to c7): it changed from frame to frame for meshes that stayed put (most of the game's
        // shaders do not read it), and every mesh got a new key each frame the camera moved. The same
        // mesh drawn for the shadows, with the light's view, gets the same key.
        const auto mesh = boundKind == Kind::Mesh ? hasher.value : 0;
        std::array<std::int64_t, 3> origin{};
        if (const auto* object = writtenTo(constants[3].Get()))
        {
            const auto world = worldOf(scene->viewInverse, object->values.data());
            origin = {world[3], world[7], world[11]};
            hasher.add(world);
            hasher.add(object->values[32]);
            if (boundKind == Kind::Terrain) hasher.add(object->values.data() + 36, 12 * sizeof(float));
        }
        else if (constants[3]) ++missingObject;

        const auto key = hasher.value;

        std::uint32_t index{};
        std::uint64_t ringOffset{};
        bool capture{}, tagged{};
        {
            std::lock_guard lock(entryMutex);
            if (dynamic)
            {
                // Each frame's skinned meshes are new: they share one entry, and are captured once a frame.
                if (!capturing.load(std::memory_order_relaxed) || !skinnedThisFrame.insert(key).second) return;

                index = gi_renderer::DynamicEntry;
                capture = true;
            }
            else if (const auto found = byKey.find(key); found != byKey.end()) index = found->second;
            else if (!freeIds.empty() && capturing.load(std::memory_order_relaxed))
            {
                index = freeIds.back();
                freeIds.pop_back();
                entries[index] = {key, EntryState::Capturing, false, mesh, origin};
                byKey.emplace(key, index);
                capture = true;
            }
            else return;

            // The same mesh's draw for the shadows has the same key, so its shader can tag it later.
            if (!dynamic && boundAlphaTested && !entries[index].alphaTested)
            {
                entries[index].alphaTested = true;
                tagged = !capture;
            }

            if (capture)
            {
                const auto offset = ring.allocate(vertexCount);
                if (!offset)
                {
                    ++ringFull;
                    if (!dynamic) freeEntry(index);
                    return;
                }

                ringOffset = *offset;
            }
        }

        Record record{index, ringOffset, vertexCount, {}, dynamic};
        if (capture)
        {
            auto* streamOutput = streamOutputOf(boundShader);
            ComPtr<ID3D11Buffer> target;
            {
                std::lock_guard lock(resourceMutex);
                target = ringBuffer;
            }

            if (!streamOutput || !target)
            {
                std::lock_guard lock(entryMutex);
                ring.release(ringOffset, vertexCount);
                if (!dynamic) freeEntry(index);
                return;
            }

            record.inverse = inverse(scene->values.data());
            const performance::Timed timed(context, performance::Pass::GiCapture);
            ID3D11Buffer* targets[]{target.Get()};
            const UINT targetOffsets[]{static_cast<UINT>(ringOffset * 16)};
            context->GSSetShader(streamOutput, nullptr, 0);
            context->SOSetTargets(1, targets, targetOffsets);
            context->DrawIndexedInstanced(call.count, std::max(call.instances, 1u), call.first, call.vertexOffset, call.firstInstance);
            ID3D11Buffer* none[]{nullptr};
            context->SOSetTargets(1, none, targetOffsets);
            context->GSSetShader(nullptr, nullptr, 0);
            ++captures;
            ++workCaptures;
            workVertices += vertexCount;
            if (dynamic)
            {
                ++skinnedCaptures;
                ++workSkinned;
            }
        }

        std::lock_guard lock(recordMutex);
        auto& list = pending[reinterpret_cast<std::uint64_t>(context)];
        if (capture) list.records.push_back(record);
        if (!dynamic) list.drawn.push_back(index);
        if (tagged) list.alphaTested.push_back(index);
        if (!list.camera && scene->mainCamera)
        {
            list.camera.emplace();
            std::memcpy(&*list.camera, scene->values.data(), sizeof(camera::SceneConstants));
        }
    }
    catch (...)
    {
    }
}

void recorded(std::uint64_t list, std::uint64_t context) noexcept
{
    try
    {
        std::lock_guard lock(recordMutex);
        const auto found = pending.find(context);
        if (found == pending.end()) return;

        auto from = std::move(found->second);
        pending.erase(found);
        auto& into = pending[list];
        into.records.insert(into.records.end(), from.records.begin(), from.records.end());
        into.drawn.insert(into.drawn.end(), from.drawn.begin(), from.drawn.end());
        into.alphaTested.insert(into.alphaTested.end(), from.alphaTested.begin(), from.alphaTested.end());
        if (from.camera) into.camera = from.camera;
    }
    catch (...)
    {
    }
}

void forget(std::uint64_t list) noexcept
{
    Pending dropped;
    {
        std::lock_guard lock(recordMutex);
        const auto found = pending.find(list);
        if (found == pending.end()) return;

        dropped = std::move(found->second);
        pending.erase(found);
    }

    std::lock_guard lock(entryMutex);
    dropRecords(dropped.records);
}

void executing(std::uint64_t list) noexcept
{
    try
    {
        std::lock_guard lock(recordMutex);
        const auto found = pending.find(list);
        if (found == pending.end()) return;

        executed.records.insert(executed.records.end(), found->second.records.begin(), found->second.records.end());
        executed.drawn.insert(executed.drawn.end(), found->second.drawn.begin(), found->second.drawn.end());
        executed.alphaTested.insert(executed.alphaTested.end(), found->second.alphaTested.begin(), found->second.alphaTested.end());
        if (found->second.camera) executed.camera = found->second.camera;
        pending.erase(found);
    }
    catch (...)
    {
    }
}

Converted convert(ID3D11DeviceContext* immediate, ID3D11UnorderedAccessView* transfer)
{
    Pending taken;
    {
        std::lock_guard lock(recordMutex);
        taken = std::move(executed);
        executed = {};
    }

    Converted converted;
    converted.camera = taken.camera;
    std::sort(taken.drawn.begin(), taken.drawn.end());
    taken.drawn.erase(std::unique(taken.drawn.begin(), taken.drawn.end()), taken.drawn.end());
    converted.drawn = std::move(taken.drawn);
    converted.alphaTested = std::move(taken.alphaTested);
    if (taken.records.empty()) return converted;

    std::lock_guard resources(resourceMutex);
    ensureConverter();
    if (!ringBuffer) return converted;

    // As many records as the transfer texture holds; the rest wait for the next kick.
    std::vector<ConvertRecord> records;
    std::vector<Record> done, waiting;
    std::uint32_t vertices{};
    std::lock_guard entryLock(entryMutex);
    for (auto& record : taken.records)
    {
        if (vertices + record.vertexCount > gi_renderer::TransferVertices)
        {
            waiting.push_back(record);
            continue;
        }

        ConvertRecord converting{static_cast<std::uint32_t>(record.ringOffset), record.vertexCount, vertices, 0, {}};
        std::memcpy(converting.inverse, record.inverse.data(), sizeof(converting.inverse));
        records.push_back(converting);
        converted.captures.push_back({record.entry, vertices, record.vertexCount, record.dynamic, !record.dynamic && entries[record.entry].alphaTested});
        done.push_back(record);
        vertices += record.vertexCount;
    }

    if (!waiting.empty())
    {
        std::lock_guard lock(recordMutex);
        executed.records.insert(executed.records.begin(), waiting.begin(), waiting.end());
    }

    ensureRecords(static_cast<UINT>(records.size()));
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(immediate->Map(recordBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "Writing the capture records");
    std::memcpy(mapped.pData, records.data(), records.size() * sizeof(ConvertRecord));
    immediate->Unmap(recordBuffer.Get(), 0);
    check(immediate->Map(parameterBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "Writing the converter's constants");
    const std::uint32_t parameters[4]{static_cast<std::uint32_t>(records.size()), vertices, 0, 0};
    std::memcpy(mapped.pData, parameters, sizeof(parameters));
    immediate->Unmap(parameterBuffer.Get(), 0);

    ID3D11ShaderResourceView* inputs[]{recordView.Get(), ringView.Get()};
    immediate->CSSetShader(convertShader.Get(), nullptr, 0);
    immediate->CSSetConstantBuffers(0, 1, parameterBuffer.GetAddressOf());
    immediate->CSSetShaderResources(0, 2, inputs);
    immediate->CSSetUnorderedAccessViews(0, 1, &transfer, nullptr);
    immediate->Dispatch((vertices + 63) / 64, 1, 1);
    ID3D11ShaderResourceView* noInputs[2]{};
    ID3D11UnorderedAccessView* noOutput{};
    immediate->CSSetShaderResources(0, 2, noInputs);
    immediate->CSSetUnorderedAccessViews(0, 1, &noOutput, nullptr);
    immediate->CSSetShader(nullptr, nullptr, 0);

    // The ring's space is free for draws recorded from now on: they execute after this dispatch.
    skinnedThisFrame.clear();
    for (const auto& record : done)
    {
        ring.release(record.ringOffset, record.vertexCount);
        if (record.dynamic) continue;

        auto& entry = entries[record.entry];
        if (entry.state == EntryState::Capturing)
        {
            entry.state = EntryState::Live;
            ++liveEntries;
        }
    }

    return converted;
}

void evicted(const std::vector<std::uint32_t>& indices) noexcept
{
    std::lock_guard lock(entryMutex);
    for (const auto index : indices)
        if (index < entries.size() && entries[index].state == EntryState::Live) freeEntry(index);
}

Work takeWork() noexcept
{
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    return {static_cast<double>(hookTicks.exchange(0)) * 1000.0 / static_cast<double>(frequency.QuadPart), workDraws.exchange(0),
        workCaptures.exchange(0), workSkinned.exchange(0), workVertices.exchange(0)};
}

std::uint32_t nearCopies() noexcept
{
    try
    {
        std::lock_guard lock(entryMutex);
        std::unordered_map<std::uint64_t, std::vector<const Entry*>> byMesh;
        for (const auto& entry : entries)
            if (entry.state == EntryState::Live && entry.mesh) byMesh[entry.mesh].push_back(&entry);

        // Within two of worldOf's 2 cm units on each axis.
        const auto sameSpot = [](const Entry& a, const Entry& b) {
            for (int axis = 0; axis < 3; ++axis)
                if (std::abs(a.origin[axis] - b.origin[axis]) > 2) return false;

            return true;
        };

        std::uint32_t copies{};
        for (const auto& [mesh, group] : byMesh)
            for (std::size_t i = 0; i < group.size(); ++i)
                for (std::size_t j = 0; j < group.size(); ++j)
                    if (i != j && sameSpot(*group[i], *group[j]))
                    {
                        ++copies;
                        break;
                    }

        return copies;
    }
    catch (...)
    {
        return 0;
    }
}

std::string status()
{
    std::uint32_t live{};
    std::uint64_t ringUsed{};
    {
        std::lock_guard lock(entryMutex);
        live = liveEntries;
        ringUsed = ring.usedSize();
    }

    std::string text = std::to_string(live) + " meshes captured (" + std::to_string(captures.load()) + " captures, "
        + std::to_string(skinnedCaptures.load()) + " of them skinned)";
    if (const auto skipped = dynamicInstances.load()) text += ", " + std::to_string(skipped) + " instanced draws left out";
    if (const auto missing = missingScene.load()) text += ", " + std::to_string(missing) + " draws without scene constants";
    if (const auto missing = missingObject.load()) text += ", " + std::to_string(missing) + " without object constants";
    if (const auto full = ringFull.load()) text += ", ring full " + std::to_string(full) + " times";
    if (ringUsed) text += ", ring " + std::to_string(ringUsed * 100 / RingVertices) + "%";

    return text;
}
}
