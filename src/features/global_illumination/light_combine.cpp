#include "features/global_illumination/light_combine.hpp"

#include "d3d11_helpers.hpp"
#include "features/global_illumination/light_combine_shaders.hpp"
#include "frame_latch.hpp"
#include "log.hpp"
#include "passes/indirect_light_pass.hpp"
#include "passes/ray_tracing/ray_tracing.hpp"
#include "performance.hpp"

#include <wrl/client.h>
#include <array>
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace phantom_fx::light_combine
{
namespace
{
using Microsoft::WRL::ComPtr;

// A texture one of the game's pixel shaders draws into.
struct DrawTarget
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    ComPtr<ID3D11RenderTargetView> target;
    UINT width{}, height{};
};

// The light's strength, and whether the resolve shows it alone (the diagnostics).
struct Wanted
{
    float strength{1};
    bool shownAlone{};
};

FrameLatch<Wanted> latch;
std::atomic<bool> wantedRunning{}, pipelineFailed{};

// ---- Direct3D, guarded by resourceMutex ----
std::mutex resourceMutex;
ComPtr<ID3D11Device> device;
ComPtr<ID3D11ComputeShader> addLightShader;
ComPtr<ID3D11Buffer> parameterBuffer;
DrawTarget gameProbeLight;
visibility_bitmask::Progress progress{"Global illumination"};

// Compiles the shader once; call with resourceMutex held. Returns false when it is unavailable.
bool ensurePipeline()
{
    if (addLightShader) return true;
    if (!device || pipelineFailed) return false;

    try
    {
        parameterBuffer = makeConstantBuffer(device.Get(), sizeof(visibility_bitmask::Parameters), "Creating the combine constants");
        // Last, so it marks a complete pipeline.
        addLightShader = computeShader(device.Get(), shaders::AddLight, "gi-add");

        return true;
    }
    catch (const std::exception& error)
    {
        pipelineFailed = true;
        log(std::string("The global illumination's combine is unavailable: ") + error.what());

        return false;
    }
}

// Call with resourceMutex held.
void ensureDrawTarget(DrawTarget& target, UINT width, UINT height, DXGI_FORMAT format, const char* what)
{
    if (target.texture && target.width == width && target.height == height) return;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    DrawTarget made{.width = width, .height = height};
    check(device->CreateTexture2D(&desc, nullptr, made.texture.GetAddressOf()), what);
    check(device->CreateShaderResourceView(made.texture.Get(), nullptr, made.view.GetAddressOf()), what);
    check(device->CreateRenderTargetView(made.texture.Get(), nullptr, made.target.GetAddressOf()), what);
    target = std::move(made);
}
}

void attach(ID3D11Device* gameDevice) noexcept
{
    std::lock_guard lock(resourceMutex);
    device = gameDevice;
    if (wantedRunning) ensurePipeline();
}

void detach() noexcept
{
    std::lock_guard lock(resourceMutex);
    addLightShader.Reset();
    parameterBuffer.Reset();
    gameProbeLight = {};
    device.Reset();
    pipelineFailed = false;
}

void configure(const Configuration& configuration) noexcept
{
    const auto& diagnostics = configuration.diagnostics;
    const bool running = configuration.effects.runs(Effect::GlobalIllumination);
    latch.set({configuration.settings.globalIllumination.strength, diagnostics.showGlobalIllumination || diagnostics.showTracedScene});
    wantedRunning = running;
    if (!running) return;

    std::lock_guard lock(resourceMutex);
    ensurePipeline();
}

void present() noexcept
{
    latch.present();
}

void drawProbeLightAside(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call)
{
    std::lock_guard lock(resourceMutex);
    if (!ensurePipeline()) return;

    ID3D11RenderTargetView* gameTargets[2]{};
    ComPtr<ID3D11DepthStencilView> gameDepth;
    context->OMGetRenderTargets(2, gameTargets, gameDepth.GetAddressOf());
    std::array<ComPtr<ID3D11RenderTargetView>, 2> heldTargets;
    for (int i = 0; i < 2; ++i) heldTargets[i].Attach(gameTargets[i]);

    // The game draws it with blending off, into the light targets.
    const auto size = sizeOf(gameTargets[0]);
    ensureDrawTarget(gameProbeLight, size.first, size.second, DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the game's probe light");
    const performance::Timed timed(context, performance::Pass::GiProbeLight);
    context->OMSetRenderTargets(1, gameProbeLight.target.GetAddressOf(), gameDepth.Get());
    context->DrawInstanced(call.count, call.instances, call.first, call.firstInstance);
    context->OMSetRenderTargets(2, gameTargets, gameDepth.Get());
}

Health health()
{
    if (!wantedRunning) return {};

    auto tracing = ray_tracing::health();
    if (tracing.kind != Health::Kind::Fine) return tracing;

    std::lock_guard lock(resourceMutex);
    return progress.health(pipelineFailed || indirect_light_pass::failed());
}

bool combine(ID3D11DeviceContext* context, ID3D11ShaderResourceView* diffuse, ID3D11ShaderResourceView* light, std::pair<UINT, UINT> size,
    visibility_bitmask::Target& lit)
{
    const auto settings = latch.frame();

    std::lock_guard lock(resourceMutex);
    if (!ensurePipeline()) return false;
    if (!light || !size.first || sizeOf(diffuse) != size) return progress.wait("the lighting textures differ in size");

    const auto [width, height] = size;
    visibility_bitmask::ensureTarget(device.Get(), lit, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, "Creating the lit diffuse light");
    // Before the first upscale at this size, nothing is taken out.
    const bool hasProbeLight = gameProbeLight.width == width && gameProbeLight.height == height;
    const visibility_bitmask::Parameters parameters{{width, height}, 0, visibility_bitmask::MaxRadiusShare * static_cast<float>(height), 1,
        settings.strength, settings.shownAlone ? 0.0f : 1.0f, 0};
    check(write(context, parameterBuffer.Get(), parameters), "Writing the combine constants");

    const performance::Timed timed(context, performance::Pass::GiCombine);
    const SavedComputeState saved(context, visibility_bitmask::PassSlots);
    context->CSSetConstantBuffers(0, 1, parameterBuffer.GetAddressOf());
    ID3D11ShaderResourceView* inputs[]{diffuse, light, hasProbeLight ? gameProbeLight.view.Get() : nullptr};
    auto* output = lit.access.Get();
    context->CSSetShaderResources(0, 3, inputs);
    context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
    context->CSSetShader(addLightShader.Get(), nullptr, 0);
    context->Dispatch(groups(width), groups(height), 1);
    progress.ran();

    return true;
}
}
