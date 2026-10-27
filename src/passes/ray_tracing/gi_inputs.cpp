// The ray tracing's inputs from the game's draws; gi_inputs.hpp describes them.
#include "passes/ray_tracing/gi_inputs.hpp"

#include "camera.hpp"
#include "clock.hpp"
#include "game_loop.hpp"
#include "log.hpp"
#include "passes/ray_tracing/engine_scene.hpp"
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
using namespace game_constants;

// What a vertex shader draws, from its source name: the game's models (their G-buffer and shadow
// passes), its terrain, or neither.
enum class Kind : std::uint8_t
{
    None,
    Model,
    Terrain,
};

// The constant buffers whose contents are kept: scene (b2), light (b3) and, for the ray tracing, material
// (b4), object (b5), bones (b6) and probes (b7). The bones are the largest.
constexpr UINT LargestConstants = sizeof(BonesConstants) / 4;
// The skinned meshes noted between kicks at most.
constexpr std::size_t MaxDrawnBones = 2048;

std::atomic<bool> active{}, lightPassKept{};

// Known vertex shaders, by the object the game binds. Holding them keeps their addresses from being reused.
std::shared_mutex shaderMutex;
std::unordered_map<IUnknown*, Kind> shaders;
std::vector<ComPtr<IUnknown>> shaderOwners;

// The shader the thread's context bound last, and what it draws.
thread_local ID3D11DeviceContext* boundContext{};
thread_local Kind boundKind{};

// What the thread last wrote to each kept constant buffer, and its size in bytes; for scene constants,
// whether they are the main camera's, worked out once a write while active.
struct Written
{
    std::array<float, LargestConstants> values{};
    UINT size{};
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

// A write as C, whatever its size.
template<class C> const C& as(const Written& written)
{
    return *reinterpret_cast<const C*>(written.values.data());
}

UINT byteWidth(ID3D11Buffer* buffer)
{
    D3D11_BUFFER_DESC desc{};
    buffer->GetDesc(&desc);

    return desc.ByteWidth;
}

// Notes a skinned mesh's draw: its model's place in the world, the view's inverse (a rotation and a
// translation) times viewWorld's translation, as world is not kept current; the draw's index range; the
// bones it drew with.
void noteBones(ID3D11DeviceContext* context, const SceneConstants& scene, const ObjectConstants& object, const ui_redirect::DrawCall& call)
{
    ComPtr<ID3D11Buffer> buffer;
    context->VSGetConstantBuffers(BonesConstants::Slot, 1, buffer.GetAddressOf());
    if (!buffer || byteWidth(buffer.Get()) != sizeof(BonesConstants)) return;

    const auto* bones = writtenTo(buffer.Get());
    if (!bones) return;

    DrawnBones drawn{.firstIndex = call.first, .indexCount = call.count};
    const auto& view = scene.view;
    const float offset[3]{object.viewWorld[0][3] - view[0][3], object.viewWorld[1][3] - view[1][3], object.viewWorld[2][3] - view[2][3]};
    for (int i = 0; i < 3; ++i)
    {
        drawn.position[i] = view[0][i] * offset[0] + view[1][i] * offset[1] + view[2][i] * offset[2];
        drawn.eye[i] = scene.eye[i];
    }

    static_assert(sizeof(drawn.bones) == sizeof(BonesConstants::bones));
    std::memcpy(drawn.bones.data(), as<BonesConstants>(*bones).bones, sizeof(drawn.bones));
    std::lock_guard lock(bonesMutex);
    if (drawnBones.size() < MaxDrawnBones) drawnBones.push_back(drawn);
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

void keepLightPassConstants(bool keep) noexcept
{
    lightPassKept = keep;
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
    const bool tracing = active.load(std::memory_order_relaxed);
    if ((!tracing && !lightPassKept.load(std::memory_order_relaxed)) || !data) return;

    D3D11_RESOURCE_DIMENSION dimension{};
    buffer->GetType(&dimension);
    if (dimension != D3D11_RESOURCE_DIMENSION_BUFFER) return;

    D3D11_BUFFER_DESC desc{};
    static_cast<ID3D11Buffer*>(buffer)->GetDesc(&desc);
    const UINT size = desc.ByteWidth;
    const bool lightPass = size == sizeof(SceneConstants) || size == sizeof(SunLightConstants) || size == sizeof(LampLightConstants);
    const bool rayTracingOnly = size == sizeof(LampMaterialConstants) || size == sizeof(TerrainMaterialConstants) || size == sizeof(ObjectConstants)
        || size == sizeof(BonesConstants) || size == sizeof(ProbeConstants);
    if (!(lightPass || (tracing && rayTracingOnly)) || !(desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER)) return;

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
        written.size = mappedSize;
        if (mappedSize == sizeof(SceneConstants))
            written.mainCamera = active.load(std::memory_order_relaxed) && camera::isMainCamera(as<SceneConstants>(written));
    }
    catch (...)
    {
    }

    mappedBuffer = nullptr;
    mappedData = nullptr;
}

bool copyWritten(ID3D11Buffer* buffer, void* into, std::size_t size, bool exactly) noexcept
{
    const auto* found = writtenTo(buffer);
    if (!found || (exactly ? size != found->size : size > sizeof(found->values))) return false;

    std::memcpy(into, found->values.data(), size);
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
        context->VSGetConstantBuffers(SceneConstants::Slot, 4, rawConstants);
        for (int i = 0; i < 4; ++i) constants[i].Attach(rawConstants[i]);

        const auto* written = writtenTo(constants[0].Get());
        if (!written || !written->mainCamera) return;

        // The camera the rays trace from: the list's first main-camera draw's.
        const auto& scene = as<SceneConstants>(*written);
        {
            std::lock_guard lock(recordMutex);
            auto [noted, first] = pending.try_emplace(reinterpret_cast<std::uint64_t>(context));
            if (first)
            {
                noted->second.constants = scene;
                noted->second.gameFrame = game_loop::currentFrame();
            }
        }

        // The terrain's inputs, with the object and material constants the game wrote for the draw.
        const auto* material = writtenTo(constants[2].Get());
        const auto* object = writtenTo(constants[3].Get());
        if (boundKind == Kind::Terrain && object && material)
            engine_scene::noteTerrain(context, as<ObjectConstants>(*object), as<TerrainMaterialConstants>(*material));

        // A skinned mesh (useWeightCount set): its bones as drawn, for the scene's characters.
        if (boundKind == Kind::Model && object && as<ObjectConstants>(*object).useWeightCount[0] != 0)
            noteBones(context, scene, as<ObjectConstants>(*object), call);
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

std::vector<DrawnBones> takeDrawnBones(const SceneConstants& camera) noexcept
{
    std::vector<DrawnBones> taken;
    try
    {
        std::lock_guard lock(bonesMutex);
        for (const auto& drawn : drawnBones)
        {
            bool sameEye = true;
            for (int i = 0; i < 3; ++i) sameEye = sameEye && std::abs(drawn.eye[i] - camera.eye[i]) <= 1e-3f;
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
    return {clock::millisecondsOf(hookTicks.exchange(0)), workDraws.exchange(0)};
}
}
