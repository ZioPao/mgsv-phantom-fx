// Path tracing on Direct3D 11; path_tracing.hpp describes it.
#include "features/path_tracing/path_tracing.hpp"

#include "d3d11_helpers.hpp"
#include "features/path_tracing/path_tracing_shaders.hpp"
#include "frame_latch.hpp"
#include "log.hpp"
#include "passes/ray_tracing/ray_tracing.hpp"
#include "performance.hpp"

#include <wrl/client.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace phantom_fx::path_tracing
{
namespace
{
using Microsoft::WRL::ComPtr;
using game_shaders::GamePass;

constexpr UINT MaskDepthSlot = 0;                     // The reflections' mask's inDepthImage
constexpr UINT SceneDepthSlot = 3, SurfaceDepthSlot = 4; // The blend's additions (shaders::ReflectionBlend)
constexpr ComputeSlots PassSlots{.buffers = 1, .views = 2, .accesses = 1};

struct Parameters
{
    std::uint32_t width, height;
    float reflections, padding;
};

// Path tracing is asked for, with the global illumination whose cache it reads, and how strongly the
// reflections are added.
struct Wanted
{
    bool on{};
    float reflections{};
};

FrameLatch<Wanted> latch;
std::atomic<bool> wanted{}, pipelineFailed{};

// The scene's depth each context's reflection mask read, for the blend it records next.
std::mutex depthMutex;
std::unordered_map<ID3D11DeviceContext*, ComPtr<ID3D11ShaderResourceView>> sceneDepths;

// ---- Direct3D, guarded by resourceMutex ----
std::mutex resourceMutex;
ComPtr<ID3D11Device> device;
ComPtr<ID3D11ComputeShader> addShader;
ComPtr<ID3D11PixelShader> blendShader;
ComPtr<ID3D11Buffer> parameterBuffer;
ComputeTexture specularWithReflections;

// Compiles the shaders once; call with resourceMutex held. Returns false when they are unavailable.
bool ensurePipeline()
{
    if (blendShader) return true;
    if (!device || pipelineFailed) return false;

    try
    {
        parameterBuffer = makeConstantBuffer(device.Get(), sizeof(Parameters), "Creating the reflections' constants");
        addShader = computeShader(device.Get(), shaders::AddReflections, "pt-add-reflections");
        // Last, so it marks a complete pipeline.
        blendShader = pixelShader(device.Get(), shaders::ReflectionBlend, "pt-reflection-blend");
        return true;
    }
    catch (const std::exception& error)
    {
        pipelineFailed = true;
        log(std::string("Path tracing is unavailable: ") + error.what());
        return false;
    }
}

bool runs()
{
    return wanted.load(std::memory_order_relaxed) && latch.frame().on && ray_tracing::pathTracingRuns();
}
}

void attach(ID3D11Device* gameDevice) noexcept
{
    std::lock_guard lock(resourceMutex);
    device = gameDevice;
    if (wanted) ensurePipeline();
}

void detach() noexcept
{
    {
        std::lock_guard lock(depthMutex);
        sceneDepths.clear();
    }

    std::lock_guard lock(resourceMutex);
    addShader.Reset();
    blendShader.Reset();
    parameterBuffer.Reset();
    specularWithReflections = {};
    device.Reset();
    pipelineFailed = false;
}

void configure(const Configuration& configuration) noexcept
{
    const auto& settings = configuration.settings.pathTracing;
    const bool on = settings.on && configuration.effects.runs(Effect::GlobalIllumination);
    latch.set({on, settings.reflections});
    wanted = on;
    if (!on) return;

    std::lock_guard lock(resourceMutex);
    ensurePipeline();
}

void present() noexcept
{
    latch.present();
}

ID3D11ShaderResourceView* addReflections(ID3D11DeviceContext* context, ID3D11ShaderResourceView* specular, std::pair<UINT, UINT> size)
{
    if (!runs() || !specular || sizeOf(specular) != size) return nullptr;

    auto* reflection = ray_tracing::reflection(size.first, size.second);
    if (!reflection) return nullptr;

    try
    {
        std::lock_guard lock(resourceMutex);
        if (!ensurePipeline()) return nullptr;

        const auto [width, height] = size;
        if (specularWithReflections.width != width || specularWithReflections.height != height)
            specularWithReflections = makeComputeTexture(device.Get(), width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the specular light with reflections");

        check(write(context, parameterBuffer.Get(), Parameters{width, height, latch.frame().reflections}), "Writing the reflections' constants");
        const performance::Timed timed(context, performance::Pass::GiCombine);
        const SavedComputeState saved(context, PassSlots);
        ID3D11ShaderResourceView* inputs[]{specular, reflection};
        auto* output = specularWithReflections.access.Get();
        context->CSSetConstantBuffers(0, 1, parameterBuffer.GetAddressOf());
        context->CSSetShaderResources(0, 2, inputs);
        context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
        context->CSSetShader(addShader.Get(), nullptr, 0);
        context->Dispatch(groups(width), groups(height), 1);
        logOnce("Path tracing: the resolve adds the traced reflections.");

        return specularWithReflections.view.Get();
    }
    catch (const std::exception& error)
    {
        logOnce(std::string("Path tracing: the resolve keeps the game's specular light: ") + error.what());
        return nullptr;
    }
}

void note(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept
{
    if (!wanted.load(std::memory_order_relaxed) || call.indexed || call.count != 3 || call.instances != 1) return;

    try
    {
        const auto* row = shader.row();
        if (!row || row->pass != GamePass::ReflectionMask) return;

        ComPtr<ID3D11ShaderResourceView> depth;
        context->PSGetShaderResources(MaskDepthSlot, 1, depth.GetAddressOf());
        std::lock_guard lock(depthMutex);
        sceneDepths[context] = std::move(depth);
    }
    catch (...)
    {
    }
}

bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept
{
    if (!wanted.load(std::memory_order_relaxed) || call.indexed || call.count != 3 || call.instances != 1) return false;

    try
    {
        const auto* row = shader.row();
        if (!row || row->pass != GamePass::ReflectionAdd || !runs()) return false;

        ComPtr<ID3D11RenderTargetView> target;
        context->OMGetRenderTargets(1, target.GetAddressOf(), nullptr);
        const auto [width, height] = sizeOf(target.Get());
        const auto surfaceDepth = ray_tracing::surfaceDepth(width, height);
        if (!surfaceDepth) return false;

        // Without the mask's depth, the traced reflections take every surface.
        ComPtr<ID3D11ShaderResourceView> sceneDepth;
        {
            std::lock_guard lock(depthMutex);
            if (const auto found = sceneDepths.find(context); found != sceneDepths.end()) sceneDepth = std::move(found->second);
        }

        std::lock_guard lock(resourceMutex);
        if (!ensurePipeline()) return false;

        ComPtr<ID3D11PixelShader> gameShader;
        ComPtr<ID3D11ShaderResourceView> gameViews[2];
        context->PSGetShader(gameShader.GetAddressOf(), nullptr, nullptr);
        context->PSGetShaderResources(SceneDepthSlot, 1, gameViews[0].GetAddressOf());
        context->PSGetShaderResources(SurfaceDepthSlot, 1, gameViews[1].GetAddressOf());
        ID3D11ShaderResourceView* depths[]{sceneDepth.Get(), surfaceDepth.Get()};
        context->PSSetShader(blendShader.Get(), nullptr, 0);
        context->PSSetShaderResources(SceneDepthSlot, 2, depths);
        context->DrawInstanced(call.count, call.instances, call.first, call.firstInstance);
        ID3D11ShaderResourceView* restored[]{gameViews[0].Get(), gameViews[1].Get()};
        context->PSSetShaderResources(SceneDepthSlot, 2, restored);
        context->PSSetShader(gameShader.Get(), nullptr, 0);
        logOnce(sceneDepth ? "Path tracing: the screen-space reflections stay on water and glass." : "Path tracing: the screen-space reflections are left out.");

        return true;
    }
    catch (const std::exception& error)
    {
        logOnce(std::string("Path tracing: the screen-space reflections stay the game's: ") + error.what());
        return false;
    }
}
}
