#include "features/lamp_contact_shadows/lamp_contact_shadows.hpp"

#include "constant_capture.hpp"
#include "d3d11_helpers.hpp"
#include "features/lamp_contact_shadows/lamp_contact_shadows_shaders.hpp"
#include "game_lights.hpp"
#include "log.hpp"
#include "passes/contact_shadow_trace.hpp"
#include "performance.hpp"

#include <wrl/client.h>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace phantom_fx::lamp_contact_shadows
{
namespace
{
using Microsoft::WRL::ComPtr;
using namespace game_lights;

constexpr UINT DepthSlot = 2;    // t2 of the light passes
constexpr UINT ShadowSlot = 7;   // t7 of the light shadow shader
constexpr UINT BoundsSlot = 6;   // b6 of the light shadow shader: the trace's constants
// A light this close to the camera plane is moved off it, so its place on screen stays finite.
constexpr float MinLightDepth = 0.01f;

std::atomic<bool> wantedRunning{}, pipelineFailed{};

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

// The light's position in view space.
std::array<float, 3> positionOf(const game_constants::LampLightConstants& light)
{
    return {light.position[0], light.position[1], light.position[2]};
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
ID3D11BlendState* scaledByAlpha(ID3D11BlendState* game)
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

    std::lock_guard lock(markedMutex);
    marked.clear();
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

// At the marking draw (colour writes off), the light's shadow is traced within the light's reach, and
// the light's volume drawn once more, both faces and without depth test, to put the shadow in the
// alpha of both light targets wherever the light can reach.
bool drawMarking(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, Falloff falloff, const BoundBlend& gameBlend)
{
    const auto scene = constant_capture::pixelShaderReads<game_constants::SceneConstants>(context);
    const auto light = constant_capture::pixelShaderReads<game_constants::LampLightConstants>(context);
    if (!scene || !light)
    {
        logOnce("Shadows: a light without its constants keeps the game's shadows.");
        return false;
    }

    const auto position = positionOf(*light);

    ComPtr<ID3D11ShaderResourceView> depth;
    context->PSGetShaderResources(DepthSlot, 1, depth.GetAddressOf());
    std::lock_guard lock(resourceMutex);
    if (!ensurePipeline()) return false;

    if (!depth)
    {
        logOnce("Shadows: a light without its depth keeps the game's shadows.");
        return false;
    }

    // A light that cannot reach the screen keeps the game's draws.
    const float range = eitherShapeRangeOf(*light, falloff);
    const auto bounds = contact_shadow_trace::boundsOf(*scene, position, range, contact_shadow_trace::depthSize(depth.Get()));
    if (!bounds) return false;

    float point[4]{position[0], position[1], position[2], 1};
    if (std::abs(point[2]) < MinLightDepth) point[2] = std::copysign(MinLightDepth, point[2]);
    performance::Timed timed(context, performance::Pass::LampTraces);
    const auto dispatched = contact_shadow_trace::trace(context, depth.Get(), *scene, contact_shadow_trace::project(*scene, point), bounds, lightShadow);
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
    context->OMSetBlendState(alphaOnly.Get(), gameBlend.factor, gameBlend.sampleMask);
    context->OMSetDepthStencilState(noDepth.Get(), 0);
    context->RSSetState(bothFaces.Get());
    context->DrawIndexedInstanced(call.count, call.instances, call.first, call.vertexOffset, call.firstInstance);
    timed.end();
    context->PSSetShader(gameShader.Get(), nullptr, 0);
    context->PSSetShaderResources(ShadowSlot, 1, gameView.GetAddressOf());
    context->PSSetConstantBuffers(BoundsSlot, 1, gameOptions.GetAddressOf());
    context->OMSetBlendState(gameBlend.state.Get(), gameBlend.factor, gameBlend.sampleMask);
    context->OMSetDepthStencilState(gameDepth.Get(), stencilReference);
    context->RSSetState(gameRasterizer.Get());
    noteMarked(context, position);

    return true;
}

ID3D11BlendState* takeShadowedBlend(ID3D11DeviceContext* context, ID3D11BlendState* gameBlend)
{
    const auto light = constant_capture::pixelShaderReads<game_constants::LampLightConstants>(context);
    if (!light || !takeMarked(context, positionOf(*light))) return nullptr;

    std::lock_guard lock(resourceMutex);
    auto* shadowed = scaledByAlpha(gameBlend);
    if (!shadowed) logOnce("Shadows: a light with another blend than (one, one) keeps the game's shadows.");

    return shadowed;
}

void contextFinished(ID3D11DeviceContext* context) noexcept
{
    std::lock_guard lock(markedMutex);
    marked.erase(context);
}

void present() noexcept
{
    std::lock_guard lock(markedMutex);
    marked.clear();
}
}
