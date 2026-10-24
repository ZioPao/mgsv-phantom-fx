// The ray tracing's inputs from the game's draws; gi_inputs.hpp describes them.
#include "gi_inputs.hpp"

#include "engine_scene.hpp"
#include "game_loop.hpp"
#include "log.hpp"
#include "shader_bytecode.hpp"

#include <Windows.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace phantom_fx::gi_inputs
{
namespace
{
using Microsoft::WRL::ComPtr;

// What a vertex shader draws, from its source name: the game's models (their G-buffer and shadow
// passes), its terrain, or neither.
enum class Kind : std::uint8_t
{
    None,
    Model,
    Terrain,
};

constexpr UINT SceneSlot = 2, BonesSlot = 6;
// The constant buffers whose contents are kept: scene (b2), object (b5), light (b3), probes (b7), material
// (b4: the terrain's page table converter).
constexpr UINT SceneConstantsSize = 480, ObjectConstantsSize = 208, LightConstantsSize = 176, ProbeConstantsSize = 576, MaterialConstantsSize = 128;
// VSBones (b6): g_vsBone, 32 3x4 rows, then g_vsPrevBone.
constexpr UINT BonesConstantsSize = 3072;
constexpr UINT LargestConstants = BonesConstantsSize / 4;
// cVSObject (b5): m_viewWorld c0 to c3, m_useWeightCount c8.x (set on skinned draws).
constexpr std::size_t UseWeightCount = 8 * 4;
// The skinned meshes noted between kicks at most.
constexpr std::size_t MaxDrawnBones = 2048;

std::atomic<bool> active{};

// Known vertex shaders, by the object the game binds. Holding them keeps their addresses from being reused.
std::shared_mutex shaderMutex;
std::unordered_map<IUnknown*, Kind> shaders;
std::vector<ComPtr<IUnknown>> shaderOwners;

// The shader the thread's context bound last, and what it draws.
thread_local ID3D11DeviceContext* boundContext{};
thread_local Kind boundKind{};

// What the thread last wrote to each kept constant buffer; for scene constants, whether they are the
// main camera's, worked out once a write.
struct Written
{
    std::array<float, LargestConstants> values{};
    bool mainCamera{};
};

thread_local ID3D11Resource* mappedBuffer{};
thread_local void* mappedData{};
thread_local UINT mappedSize{};
thread_local std::unordered_map<ID3D11Resource*, Written> contents;

// The camera by deferred context while recorded, by command list once finished, and that of the lists
// executed since the last kick.
std::mutex recordMutex;
std::unordered_map<std::uint64_t, Camera> pending;
std::optional<Camera> executed;

// Diagnostics: the hooks' time in performance counter ticks, and the draws, since takeWork.
std::atomic<std::int64_t> hookTicks{};
std::atomic<std::uint64_t> workDraws{};

// Adds the time from its construction to its destruction to hookTicks; nothing without the diagnostic
// log, as the game's threads would share the counter on every draw.
struct HookTimer
{
    LARGE_INTEGER start{};

    HookTimer()
    {
        if constexpr (DiagnosticLog) QueryPerformanceCounter(&start);
    }

    ~HookTimer()
    {
        if constexpr (DiagnosticLog)
        {
            LARGE_INTEGER end{};
            QueryPerformanceCounter(&end);
            hookTicks += end.QuadPart - start.QuadPart;
        }
    }
};

Kind kindOf(std::string_view name)
{
    if (name.starts_with("NewTerrain_Depth") || name.starts_with("NewTerrain_Shadow")) return Kind::Terrain;
    if (name.starts_with("fox3ddf_") || name.starts_with("tpp3ddf_") || name.starts_with("ModelForward_Shadow")) return Kind::Model;

    return Kind::None;
}

// The draws' skinned meshes since the last kick (takeDrawnBones).
std::mutex bonesMutex;
std::vector<DrawnBones> drawnBones;

const Written* writtenTo(ID3D11Buffer* buffer)
{
    if (!buffer) return nullptr;

    const auto found = contents.find(buffer);
    return found != contents.end() ? &found->second : nullptr;
}

UINT byteWidth(ID3D11Buffer* buffer)
{
    D3D11_BUFFER_DESC desc{};
    buffer->GetDesc(&desc);

    return desc.ByteWidth;
}

// Notes a skinned mesh's draw: its model's place in the world, the view's inverse (cVSScene m_view, c8
// to c10, a rotation and a translation) times m_viewWorld's translation, as m_world is not kept current
// (gi_capture's worldOf until stage 5); the draw's index range; the bones it drew with.
void noteBones(ID3D11DeviceContext* context, const float* scene, const float* object, const ui_redirect::DrawCall& call)
{
    ComPtr<ID3D11Buffer> buffer;
    context->VSGetConstantBuffers(BonesSlot, 1, buffer.GetAddressOf());
    if (!buffer || byteWidth(buffer.Get()) != BonesConstantsSize) return;

    const auto* bones = writtenTo(buffer.Get());
    if (!bones) return;

    DrawnBones drawn{.firstIndex = call.first, .indexCount = call.count};
    const float offset[3]{object[3] - scene[8 * 4 + 3], object[7] - scene[9 * 4 + 3], object[11] - scene[10 * 4 + 3]};
    for (int i = 0; i < 3; ++i)
    {
        drawn.position[i] = scene[8 * 4 + i] * offset[0] + scene[9 * 4 + i] * offset[1] + scene[10 * 4 + i] * offset[2];
        drawn.eye[i] = scene[20 * 4 + i];
    }

    std::copy_n(bones->values.begin(), drawn.bones.size(), drawn.bones.begin());
    std::lock_guard lock(bonesMutex);
    if (drawnBones.size() < MaxDrawnBones) drawnBones.push_back(drawn);
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
}

void detach() noexcept
{
    active = false;
    {
        std::lock_guard lock(recordMutex);
        pending.clear();
        executed.reset();
    }

    std::unique_lock lock(shaderMutex);
    shaders.clear();
    shaderOwners.clear();
}

void setActive(bool on) noexcept
{
    if (on == active.load()) return;

    std::lock_guard lock(recordMutex);
    active = on;
    pending.clear();
    executed.reset();
}

void vertexShaderCreated(const void* code, std::size_t size, IUnknown* shader) noexcept
{
    if (!code || !shader) return;

    const auto kind = kindOf(sourceName(std::string_view(static_cast<const char*>(code), size), "_vs.hlsl"));
    if (kind == Kind::None) return;

    try
    {
        std::unique_lock lock(shaderMutex);
        shaders[shader] = kind;
        shaderOwners.emplace_back(shader);
    }
    catch (...)
    {
    }
}

void vertexShaderBound(ID3D11DeviceContext* context, IUnknown* shader) noexcept
{
    Kind kind{};
    if (shader)
    {
        std::shared_lock lock(shaderMutex);
        if (const auto found = shaders.find(shader); found != shaders.end()) kind = found->second;
    }

    boundContext = context;
    boundKind = kind;
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
        || desc.ByteWidth == ProbeConstantsSize || desc.ByteWidth == MaterialConstantsSize || desc.ByteWidth == BonesConstantsSize;
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
        if (mappedSize == SceneConstantsSize) written.mainCamera = mainCamera(written.values);
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
    if constexpr (DiagnosticLog) ++workDraws;
    try
    {
        std::array<ComPtr<ID3D11Buffer>, 4> constants; // b2 to b5
        ID3D11Buffer* rawConstants[4]{};
        context->VSGetConstantBuffers(SceneSlot, 4, rawConstants);
        for (int i = 0; i < 4; ++i) constants[i].Attach(rawConstants[i]);

        const auto* scene = writtenTo(constants[0].Get());
        if (!scene || !scene->mainCamera) return;

        // The camera the rays trace from: the list's first main-camera draw's.
        {
            std::lock_guard lock(recordMutex);
            auto [noted, first] = pending.try_emplace(reinterpret_cast<std::uint64_t>(context));
            if (first)
            {
                std::memcpy(&noted->second.constants, scene->values.data(), sizeof(camera::SceneConstants));
                noted->second.gameFrame = game_loop::currentFrame();
            }
        }

        // The terrain's inputs, with the object and material constants the game wrote for the draw.
        const auto* material = writtenTo(constants[2].Get());
        const auto* object = writtenTo(constants[3].Get());
        if (boundKind == Kind::Terrain && object && material) engine_scene::noteTerrain(context, object->values.data(), material->values.data());

        // A skinned mesh (m_useWeightCount set): its bones as drawn, for the scene's characters.
        if (boundKind == Kind::Model && object && object->values[UseWeightCount] != 0) noteBones(context, scene->values.data(), object->values.data(), call);
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

        const auto noted = found->second;
        pending.erase(found);
        pending[list] = noted;
    }
    catch (...)
    {
    }
}

void forget(std::uint64_t list) noexcept
{
    std::lock_guard lock(recordMutex);
    pending.erase(list);
}

void executing(std::uint64_t list) noexcept
{
    std::lock_guard lock(recordMutex);
    const auto found = pending.find(list);
    if (found == pending.end()) return;

    executed = found->second;
    pending.erase(found);
}

std::vector<DrawnBones> takeDrawnBones(const camera::SceneConstants& camera) noexcept
{
    std::vector<DrawnBones> taken;
    try
    {
        std::lock_guard lock(bonesMutex);
        for (const auto& drawn : drawnBones)
        {
            bool sameEye = true;
            for (int i = 0; i < 3; ++i) sameEye = sameEye && std::abs(drawn.eye[i] - camera.c[20][i]) <= 1e-3f;
            if (sameEye) taken.push_back(drawn);
        }

        drawnBones.clear();
    }
    catch (...)
    {
    }

    return taken;
}

std::optional<Camera> takeCamera() noexcept
{
    std::lock_guard lock(recordMutex);
    return std::exchange(executed, std::nullopt);
}

Work takeWork() noexcept
{
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    return {static_cast<double>(hookTicks.exchange(0)) * 1000.0 / static_cast<double>(frequency.QuadPart), workDraws.exchange(0)};
}
}
