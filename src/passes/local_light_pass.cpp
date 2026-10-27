#include "passes/local_light_pass.hpp"

#include "d3d11_helpers.hpp"
#include "features/lamp_contact_shadows/lamp_contact_shadows.hpp"
#include "features/lamp_soft_shadows/lamp_soft_shadows.hpp"
#include "frame_latch.hpp"
#include "game_lights.hpp"
#include "log.hpp"
#include "passes/contact_shadow_trace.hpp"
#include "performance.hpp"
#include "shader_bytecode.hpp"

#include <wrl/client.h>
#include <atomic>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace phantom_fx::local_light_pass
{
namespace
{
using Microsoft::WRL::ComPtr;
using namespace game_lights;

// The lamp shadow effects the pass adds, by the effect plan.
struct Composition
{
    bool contactShadows{}, softShadows{};

    bool any() const { return contactShadows || softShadows; }
};

FrameLatch<Composition> latch;
std::atomic<bool> active{};

// An identified light shader, and whether the log has named its first lighting draw.
struct LightShader
{
    const GameLight* light;
    std::atomic<bool> named{};
};

// Identified light shaders, looked up on every indexed draw. Holding them keeps their addresses from
// being reused.
std::shared_mutex lightShaderMutex;
std::unordered_map<IUnknown*, LightShader> lightShaders;
std::vector<ComPtr<IUnknown>> lightShaderOwners;

// Null for any other shader.
LightShader* lightShaderOf(IUnknown* shader)
{
    std::shared_lock lock(lightShaderMutex);
    const auto found = shader ? lightShaders.find(shader) : lightShaders.end();

    return found == lightShaders.end() ? nullptr : &found->second;
}

// A light shader's kind, as the log names it.
std::string kindOf(const GameLight& light)
{
    return std::string(light.shape == Shape::Point ? "point light" : light.flashlight ? "flashlight" : "spot light")
        + (light.falloff == Falloff::Quartic ? ", quartic falloff" : ", sixteenth falloff") + (light.specular ? ", specular" : ", no specular")
        + (light.shadowMap ? ", shadow map" : ", no shadow map");
}

// A light's draws: the marking draw (colour writes off) takes the contact shadow; the lighting draw
// the contact shadow's blend and the soft shadows' shader.
bool drawLight(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, ID3D11PixelShader* gameShader, LightShader& shader,
    const Composition& composition)
{
    const auto gameBlend = boundBlend(context);
    if (!gameBlend.state) return false;

    D3D11_BLEND_DESC blend{};
    gameBlend.state->GetDesc(&blend);
    if (blend.RenderTarget[0].RenderTargetWriteMask == 0)
        return composition.contactShadows && lamp_contact_shadows::drawMarking(context, call, shader.light->falloff, gameBlend);

    const auto replacement = composition.softShadows ? lamp_soft_shadows::replacementFor(*shader.light) : std::nullopt;

    if (!shader.named.exchange(true)) log("Shadows: the game draws a " + kindOf(*shader.light) + (replacement ? ", with the soft shadows." : "."));

    auto* shadowed = composition.contactShadows ? lamp_contact_shadows::takeShadowedBlend(context, gameBlend.state.Get()) : nullptr;
    if (!replacement && !shadowed) return false;

    if (shadowed) context->OMSetBlendState(shadowed, gameBlend.factor, gameBlend.sampleMask);

    constexpr UINT OptionsSlot = lamp_soft_shadows::Replacement::OptionsSlot;
    ComPtr<ID3D11Buffer> gameOptions;
    if (replacement)
    {
        context->PSGetConstantBuffers(OptionsSlot, 1, gameOptions.GetAddressOf());
        context->PSSetShader(replacement->shader, nullptr, 0);
        context->PSSetConstantBuffers(OptionsSlot, 1, &replacement->options);
    }

    performance::Timed timed(context, replacement ? performance::Pass::LampSoftLight : performance::Pass::LampShadows);
    context->DrawIndexedInstanced(call.count, call.instances, call.first, call.vertexOffset, call.firstInstance);
    timed.end();

    if (shadowed) context->OMSetBlendState(gameBlend.state.Get(), gameBlend.factor, gameBlend.sampleMask);

    if (replacement)
    {
        context->PSSetShader(gameShader, nullptr, 0);
        context->PSSetConstantBuffers(OptionsSlot, 1, gameOptions.GetAddressOf());
        logOnce("Shadows: the lamps draw with soft shadows.");
    }

    return true;
}
}

void detach() noexcept
{
    std::unique_lock lock(lightShaderMutex);
    lightShaders.clear();
    lightShaderOwners.clear();
}

void configure(const Configuration& configuration) noexcept
{
    latch.set({.contactShadows = configuration.effects.runs(Effect::LampContactShadows), .softShadows = configuration.effects.runs(Effect::LampSoftShadows)});
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
        lightShaders.try_emplace(shader, light);
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
        auto* shader = lightShaderOf(gameShader.Get());
        if (!shader) return false;

        return drawLight(context, call, gameShader.Get(), *shader, latch.frame());
    }
    catch (const std::exception& error)
    {
        logOnce(std::string("Shadows: a light pass kept the game's shadows: ") + error.what());
    }
    catch (...)
    {
    }

    return false;
}

void present() noexcept
{
    const auto current = latch.present();
    active = current.any();
    contact_shadow_trace::tracesThisFrame(contact_shadow_trace::Tracer::Lamps, current.contactShadows);
}
}
