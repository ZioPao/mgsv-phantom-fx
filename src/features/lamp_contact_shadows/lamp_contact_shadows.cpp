#include "features/lamp_contact_shadows/lamp_contact_shadows.hpp"

#include "d3d11_helpers.hpp"
#include "features/lamp_contact_shadows/lamp_contact_shadows_shaders.hpp"
#include "game_lights.hpp"
#include "log.hpp"
#include "passes/contact_shadow_trace.hpp"
#include "performance.hpp"
#include "shader_bytecode.hpp"

#include <wrl/client.h>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace phantom_fx::lamp_contact_shadows
{
namespace
{
using Microsoft::WRL::ComPtr;
using namespace game_lights;

constexpr UINT SpecularSlot = 1; // t1 of the light passes, whose z is the material
constexpr UINT DepthSlot = 2;    // t2 of the light passes
constexpr UINT ShadowSlot = 7;   // t7 of the light shadow shader
constexpr UINT BoundsSlot = 6;   // b6 of the light shadow shader: the trace's constants
// A light this close to the camera plane is moved off it, so its place on screen stays finite.
constexpr float MinLightDepth = 0.01f;

std::atomic<bool> wantedRunning{}, active{}, pipelineFailed{};

// Identified local light shaders, looked up on every indexed draw.
std::shared_mutex lightShaderMutex;
std::unordered_map<IUnknown*, Falloff> lightShaders;
std::vector<ComPtr<IUnknown>> lightShaderOwners;

// The light whose shadow a context's last stencil-marking draw put in the light targets' alpha, by
// its position, for the lighting draw that follows.
std::mutex markedMutex;
std::unordered_map<ID3D11DeviceContext*, std::array<float, 3>> marked;

// ---- Direct3D, guarded by resourceMutex ----
std::mutex resourceMutex;
ComPtr<ID3D11Device> device;
ComPtr<ID3D11PixelShader> lightShadowShader;
// The light shadow's draw: alpha only, over the whole light volume.
ComPtr<ID3D11BlendState> alphaOnly;
ComPtr<ID3D11DepthStencilState> noDepth;
ComPtr<ID3D11RasterizerState> bothFaces;
// The game's additive light blend, by the state it binds, and the same scaled by the target's alpha.
std::unordered_map<ID3D11BlendState*, std::pair<ComPtr<ID3D11BlendState>, ComPtr<ID3D11BlendState>>> shadowedBlends;
contact_shadow_trace::ShadowTarget lightShadow;
std::array<bool, 2> tracedOnce{}; // By falloff

// Compiles the shaders once; call with resourceMutex held. Returns false when they are unavailable.
bool ensurePipeline()
{
    if (lightShadowShader) return true;
    if (!device || pipelineFailed) return false;

    try
    {
        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0].SrcBlend = blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blend.RenderTarget[0].DestBlend = blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        blend.RenderTarget[0].BlendOp = blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALPHA;
        check(device->CreateBlendState(&blend, alphaOnly.ReleaseAndGetAddressOf()), "Creating the light shadow blend");

        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
        check(device->CreateDepthStencilState(&depth, noDepth.ReleaseAndGetAddressOf()), "Creating the light shadow depth state");

        D3D11_RASTERIZER_DESC rasterizer{};
        rasterizer.FillMode = D3D11_FILL_SOLID;
        rasterizer.CullMode = D3D11_CULL_NONE;
        rasterizer.DepthClipEnable = TRUE;
        check(device->CreateRasterizerState(&rasterizer, bothFaces.ReleaseAndGetAddressOf()), "Creating the light shadow rasterizer state");

        // Last, so it marks a complete pipeline.
        lightShadowShader = pixelShader(device.Get(), shaders::LightShadow, "light-shadow");
        log("Lamp shadow shaders ready.");

        return true;
    }
    catch (const std::exception& error)
    {
        pipelineFailed = true;
        log(std::string("Lamp contact shadows unavailable: ") + error.what());

        return false;
    }
}

// A local light shader's falloff; none for any other shader.
std::optional<Falloff> falloffOf(IUnknown* shader)
{
    std::shared_lock lock(lightShaderMutex);
    const auto found = shader ? lightShaders.find(shader) : lightShaders.end();
    if (found == lightShaders.end()) return std::nullopt;

    return found->second;
}

void noteMarked(ID3D11DeviceContext* context, const std::array<float, 3>& position)
{
    std::lock_guard lock(markedMutex);
    marked[context] = position;
}

// Whether `context`'s last marking draw was for the light at `position`; it is then used up.
bool takeMarked(ID3D11DeviceContext* context, const std::array<float, 3>& position)
{
    std::lock_guard lock(markedMutex);
    const auto found = marked.find(context);
    if (found == marked.end()) return false;

    const bool same = found->second == position;
    marked.erase(found);

    return same;
}

// The game's additive light blend (ONE, ONE) with its light scaled by the target's alpha, which it
// keeps; null for any other blend. Call with resourceMutex held.
ID3D11BlendState* shadowedBlend(ID3D11BlendState* game)
{
    if (!game) return nullptr;

    if (const auto found = shadowedBlends.find(game); found != shadowedBlends.end()) return found->second.second.Get();

    D3D11_BLEND_DESC desc{};
    game->GetDesc(&desc);
    const UINT targets = desc.IndependentBlendEnable ? 2 : 1;
    ComPtr<ID3D11BlendState> shadowed;
    for (UINT i = 0; i < targets; ++i)
    {
        auto& target = desc.RenderTarget[i];
        if (!target.BlendEnable || target.SrcBlend != D3D11_BLEND_ONE || target.DestBlend != D3D11_BLEND_ONE || target.BlendOp != D3D11_BLEND_OP_ADD)
        {
            shadowedBlends.emplace(game, std::pair{ComPtr<ID3D11BlendState>(game), ComPtr<ID3D11BlendState>()});
            return nullptr;
        }

        target.SrcBlend = D3D11_BLEND_DEST_ALPHA;
        target.SrcBlendAlpha = D3D11_BLEND_ZERO;
        target.DestBlendAlpha = D3D11_BLEND_ONE;
        target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    }

    check(device->CreateBlendState(&desc, shadowed.GetAddressOf()), "Creating a shadowed light blend");
    shadowedBlends.emplace(game, std::pair{ComPtr<ID3D11BlendState>(game), shadowed});

    return shadowed.Get();
}

// A point or spot light's draws. At the one that marks the stencil (colour writes off), the light's
// shadow is traced within the light's reach, and the light's volume drawn once more, both faces and
// without depth test, to put the shadow in the alpha of both light targets wherever the light can
// reach. The lighting draw that follows then scales its light by that alpha.
bool drawLocalLight(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, Falloff falloff)
{
    ComPtr<ID3D11BlendState> gameBlend;
    float blendFactor[4]{};
    UINT sampleMask{};
    context->OMGetBlendState(gameBlend.GetAddressOf(), blendFactor, &sampleMask);
    if (!gameBlend) return false;

    D3D11_BLEND_DESC blend{};
    gameBlend->GetDesc(&blend);
    const bool marking = blend.RenderTarget[0].RenderTargetWriteMask == 0;
    const auto constants = contact_shadow_trace::passConstants(context);
    if (!constants)
    {
        std::lock_guard lock(resourceMutex);
        logOnce("Shadows: a light without its constants keeps the game's shadows.");
        return false;
    }

    const float* scene = constants->scene.data();
    const float* light = constants->light.data();

    const std::array position{light[LightPosition], light[LightPosition + 1], light[LightPosition + 2]};
    if (!marking)
    {
        if (!takeMarked(context, position)) return false;

        std::lock_guard lock(resourceMutex);
        auto* shadowed = shadowedBlend(gameBlend.Get());
        if (!shadowed)
        {
            logOnce("Shadows: a light with another blend than (one, one) keeps the game's shadows.");
            return false;
        }

        context->OMSetBlendState(shadowed, blendFactor, sampleMask);
        context->DrawIndexedInstanced(call.count, call.instances, call.first, call.vertexOffset, call.firstInstance);
        context->OMSetBlendState(gameBlend.Get(), blendFactor, sampleMask);

        return true;
    }

    ComPtr<ID3D11ShaderResourceView> depth, specular;
    context->PSGetShaderResources(DepthSlot, 1, depth.GetAddressOf());
    context->PSGetShaderResources(SpecularSlot, 1, specular.GetAddressOf());
    std::lock_guard lock(resourceMutex);
    if (!ensurePipeline()) return false;

    if (!depth)
    {
        logOnce("Shadows: a light without its depth keeps the game's shadows.");
        return false;
    }

    // A light that cannot reach the screen keeps the game's draws.
    const float range = eitherShapeRangeOf(light, falloff);
    const auto bounds = contact_shadow_trace::boundsOf(scene, position, range, contact_shadow_trace::depthSize(depth.Get()));
    if (!bounds) return false;

    float point[4]{position[0], position[1], position[2], 1};
    if (std::abs(point[2]) < MinLightDepth) point[2] = std::copysign(MinLightDepth, point[2]);
    performance::Timed timed(context, performance::Pass::LampTraces);
    const auto dispatched = contact_shadow_trace::trace(context, depth.Get(), specular.Get(), contact_shadow_trace::project(scene, point), bounds, lightShadow);
    timed.end();
    if (!dispatched || dispatched->dispatches == 0) return false;

    if (auto& once = tracedOnce[static_cast<std::size_t>(falloff)]; !once)
    {
        once = true;
        log("Shadows: first local light trace (" + std::string(falloff == Falloff::Quartic ? "SSLighting2" : "Lighting_NA") + "), light at "
            + std::to_string(position[0]) + ", " + std::to_string(position[1]) + ", " + std::to_string(position[2]) + " (view space), range "
            + std::to_string(range) + " m, pixels " + std::to_string(bounds->min[0]) + ", " + std::to_string(bounds->min[1]) + " to "
            + std::to_string(bounds->max[0]) + ", " + std::to_string(bounds->max[1]) + ", depth " + std::to_string(bounds->depthMin) + " to "
            + std::to_string(bounds->depthMax) + ", " + contact_shadow_trace::describe(*dispatched) + ".");
    }

    ComPtr<ID3D11PixelShader> gameShader;
    ComPtr<ID3D11ShaderResourceView> gameView;
    ComPtr<ID3D11Buffer> gameOptions;
    ComPtr<ID3D11DepthStencilState> gameDepth;
    UINT stencilReference{};
    ComPtr<ID3D11RasterizerState> gameRasterizer;
    context->PSGetShader(gameShader.GetAddressOf(), nullptr, nullptr);
    context->PSGetShaderResources(ShadowSlot, 1, gameView.GetAddressOf());
    context->PSGetConstantBuffers(BoundsSlot, 1, gameOptions.GetAddressOf());
    context->OMGetDepthStencilState(gameDepth.GetAddressOf(), &stencilReference);
    context->RSGetState(gameRasterizer.GetAddressOf());

    context->DrawIndexedInstanced(call.count, call.instances, call.first, call.vertexOffset, call.firstInstance);
    timed.next(performance::Pass::LampShadows);
    auto* traceConstants = contact_shadow_trace::parameters();
    context->PSSetShader(lightShadowShader.Get(), nullptr, 0);
    context->PSSetShaderResources(ShadowSlot, 1, lightShadow.view.GetAddressOf());
    context->PSSetConstantBuffers(BoundsSlot, 1, &traceConstants);
    context->OMSetBlendState(alphaOnly.Get(), blendFactor, sampleMask);
    context->OMSetDepthStencilState(noDepth.Get(), 0);
    context->RSSetState(bothFaces.Get());
    context->DrawIndexedInstanced(call.count, call.instances, call.first, call.vertexOffset, call.firstInstance);
    timed.end();
    context->PSSetShader(gameShader.Get(), nullptr, 0);
    context->PSSetShaderResources(ShadowSlot, 1, gameView.GetAddressOf());
    context->PSSetConstantBuffers(BoundsSlot, 1, gameOptions.GetAddressOf());
    context->OMSetBlendState(gameBlend.Get(), blendFactor, sampleMask);
    context->OMSetDepthStencilState(gameDepth.Get(), stencilReference);
    context->RSSetState(gameRasterizer.Get());
    noteMarked(context, position);

    return true;
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
    {
        std::lock_guard lock(resourceMutex);
        lightShadowShader.Reset();
        alphaOnly.Reset();
        noDepth.Reset();
        bothFaces.Reset();
        shadowedBlends.clear();
        lightShadow = {};
        device.Reset();
        pipelineFailed = false;
    }

    {
        std::lock_guard lock(markedMutex);
        marked.clear();
    }

    std::unique_lock lock(lightShaderMutex);
    lightShaders.clear();
    lightShaderOwners.clear();
}

void configure(const Configuration& configuration) noexcept
{
    const bool running = configuration.effects.runs(Effect::LampContactShadows);
    wantedRunning = running;
    if (!running) return;

    std::lock_guard lock(resourceMutex);
    ensurePipeline();
}

Health health()
{
    if (pipelineFailed || contact_shadow_trace::failed()) return Health::unavailable("Contact shadows are unavailable (see PhantomFX.log).");

    return {};
}

void shaderCreated(const void* code, std::size_t size, IUnknown* shader) noexcept
{
    const auto checksum = dxbcHashOf(code, size);
    if (!checksum || !shader) return;

    const auto* light = findGameLight(toHex(*checksum));
    if (!light) return;

    try
    {
        std::unique_lock lock(lightShaderMutex);
        lightShaders.emplace(shader, light->falloff);
        lightShaderOwners.emplace_back(shader);
    }
    catch (...)
    {
    }
}

bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call) noexcept
{
    if (!active.load(std::memory_order_relaxed) || !call.indexed || call.instances != 1) return false;

    try
    {
        ComPtr<ID3D11PixelShader> gameShader;
        context->PSGetShader(gameShader.GetAddressOf(), nullptr, nullptr);
        const auto falloff = falloffOf(gameShader.Get());
        if (!falloff) return false;

        return drawLocalLight(context, call, *falloff);
    }
    catch (const std::exception& error)
    {
        std::lock_guard lock(resourceMutex);
        logOnce(std::string("Shadows: a light pass kept the game's shadows: ") + error.what());
    }
    catch (...)
    {
    }

    return false;
}

void contextFinished(ID3D11DeviceContext* context) noexcept
{
    std::lock_guard lock(markedMutex);
    marked.erase(context);
}

void present() noexcept
{
    {
        std::lock_guard lock(markedMutex);
        marked.clear();
    }

    active = wantedRunning && !pipelineFailed;
    contact_shadow_trace::tracesThisFrame(contact_shadow_trace::Tracer::Lamps, active);
}
}
