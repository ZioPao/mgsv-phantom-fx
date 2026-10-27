#include "passes/sun_light_pass.hpp"

#include "d3d11_helpers.hpp"
#include "features/upscaling/upscaler.hpp"
#include "features/sun_contact_shadows/sun_contact_shadows.hpp"
#include "frame_latch.hpp"
#include "log.hpp"
#include "passes/contact_shadow_trace.hpp"
#include "passes/ray_tracing/gi_inputs.hpp"
#include "passes/ray_tracing/ray_tracing.hpp"
#include "passes/sun_light_pass_shaders.hpp"
#include "performance.hpp"
#include "shader_bytecode.hpp"

#include <wrl/client.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>

namespace phantom_fx::sun_light_pass
{
namespace
{
using Microsoft::WRL::ComPtr;

// The game's sun light pixel shaders: inside the shadow range, with the shadow map, and beyond it.
// In rain the game draws a pair without the sky reflection, as its rain filter reflects the sky
// instead.
enum class SunShader : std::uint8_t { ShadowMap, Far, RainShadowMap, RainFar, Count, None = Count };
constexpr const char* SunChecksums[]{
    "56ed4db892a7940f038146eeb0df0d30", // SunLight_DL_CS_ES
    "92726e8d231c8ed85f0db90400ef2258", // SunLight_DL_ES
    "7e5c2fc99c9d84ddd32df4e789e7082a", // SunLight_DL_CS
    "853f56a63e5bc66e6ee301c85ebfeaa8", // SunLight_DL
};
constexpr const char* SunNames[]{"shadow map", "far", "shadow map, rain", "far, rain"};
static_assert(std::size(SunChecksums) == static_cast<std::size_t>(SunShader::Count) && std::size(SunNames) == std::size(SunChecksums));

bool shadowMapped(SunShader sun)
{
    return sun == SunShader::ShadowMap || sun == SunShader::RainShadowMap;
}

bool reflectsSky(SunShader sun)
{
    return sun == SunShader::ShadowMap || sun == SunShader::Far;
}

constexpr UINT DepthSlot = 2;             // t2 of the light passes
constexpr UINT RayTracedSlot = 6;         // t6 of the replacement shaders
constexpr UINT ContactShadowSlot = 7;     // t7 of the replacement shaders
constexpr UINT OptionsSlot = 6;           // b6 of the replacement shaders
constexpr std::uint32_t PatternTurns = 64; // Frames before the soft shadows' samples repeat

// cSunShadows (b6) of the replacement shaders.
struct ShaderOptions
{
    float soft, sunTangent, frame, screenSpace, rayTraced, padding[3];
};
static_assert(sizeof(ShaderOptions) % 16 == 0);

// The sun shadow effects the pass adds, by the effect plan, with their settings.
struct Composition
{
    bool softShadows{}, contactShadows{}, rayTracedShadows{};
    Sun sun;
    RayTracedShadows rayTraced;
    bool onlyContactShadows{}; // Diagnostics: the sun is shadowed by its contact shadows alone

    bool any() const { return softShadows || contactShadows || rayTracedShadows; }
};

FrameLatch<Composition> latch;
std::atomic<bool> active{}, pipelineFailed{};
// Counts presents, to turn the soft shadows' samples while DLSS or FSR runs: they accumulate frames.
std::atomic<std::uint32_t> frame{};
std::atomic<bool> turnPattern{};

// Identified sun shaders, read lock-free on every full-screen draw. The game can create a shader more
// than once; holding the copies keeps their addresses from being reused.
constexpr std::size_t CopiesPerShader = 4;
std::mutex shaderMutex;
std::array<std::array<std::atomic<IUnknown*>, CopiesPerShader>, std::size(SunChecksums)> sunShaders{};
std::array<std::array<ComPtr<IUnknown>, CopiesPerShader>, std::size(SunChecksums)> shaderOwners;

// ---- Direct3D, guarded by resourceMutex ----
std::mutex resourceMutex;
ComPtr<ID3D11Device> device;
std::array<ComPtr<ID3D11PixelShader>, std::size(SunChecksums)> replacements;
ComPtr<ID3D11Buffer> optionsBuffer;

// Compiles the shaders once; call with resourceMutex held. Returns false when they are unavailable.
bool ensurePipeline()
{
    if (optionsBuffer) return true;
    if (!device || pipelineFailed) return false;

    try
    {
        for (std::size_t index = 0; index < replacements.size(); ++index)
        {
            const auto sun = static_cast<SunShader>(index);
            const D3D_SHADER_MACRO defines[]{{"SHADOW_MAP", shadowMapped(sun) ? "1" : "0"}, {"SKY_REFLECTION", reflectsSky(sun) ? "1" : "0"}, {}};
            replacements[index] = pixelShader(device.Get(), shaders::SunLight, "sun-light", defines);
        }

        // Last, so it marks a complete pipeline.
        optionsBuffer = makeConstantBuffer(device.Get(), sizeof(ShaderOptions), "Creating the sun shadow options");
        log("Sun light pass shaders ready.");

        return true;
    }
    catch (const std::exception& error)
    {
        pipelineFailed = true;
        for (auto& replacement : replacements) replacement.Reset();
        log(std::string("Sun shadows unavailable: ") + error.what());

        return false;
    }
}

SunShader sunOf(IUnknown* shader)
{
    if (!shader) return SunShader::None;

    for (std::size_t sun = 0; sun < sunShaders.size(); ++sun)
        for (const auto& copy : sunShaders[sun])
            if (copy.load(std::memory_order_relaxed) == shader) return static_cast<SunShader>(sun);

    return SunShader::None;
}

// Writes the replacement shaders' options on `context`; call with resourceMutex held. With the
// ray-traced shadow, the shadow map's part is always soft.
void writeOptions(ID3D11DeviceContext* context, const Composition& composition, bool rayTraced)
{
    constexpr float DegreesToRadians = 3.14159265f / 180;
    const ShaderOptions values{composition.softShadows || rayTraced ? 1.0f : 0.0f, std::tan(composition.sun.diameterDegrees * 0.5f * DegreesToRadians),
        turnPattern.load(std::memory_order_relaxed) ? static_cast<float>(frame.load(std::memory_order_relaxed) % PatternTurns) : 0.0f,
        composition.contactShadows ? 1.0f : 0.0f, rayTraced ? 1.0f : 0.0f, {}};
    check(write(context, optionsBuffer.Get(), values), "Writing the sun shadow options");
}

// The sun pass, drawn with the replacement shaders.
bool drawSun(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, SunShader sun, ID3D11PixelShader* gameShader, const Composition& composition)
{
    ComPtr<ID3D11ShaderResourceView> depth;
    context->PSGetShaderResources(DepthSlot, 1, depth.GetAddressOf());

    // The ray-traced shadow at the depth's size, once Direct3D 12 has traced one; until then the
    // game's shadows stay. The rays trace the main view only, not a smaller camera's (a mirror's).
    ID3D11ShaderResourceView* rayTraced{};
    if (composition.rayTracedShadows && depth && viewportFillsTarget(context))
    {
        const auto size = contact_shadow_trace::depthSize(depth.Get());
        rayTraced = ray_tracing::sunShadow(static_cast<UINT>(size[0]), static_cast<UINT>(size[1]));
    }

    // Beyond the shadow range there is no shadow map to soften.
    if (!composition.contactShadows && !(composition.softShadows && shadowMapped(sun)) && !rayTraced) return false;

    const auto scene = gi_inputs::pixelShaderReads<game_constants::SceneConstants>(context);
    const auto light = gi_inputs::pixelShaderReads<game_constants::SunLightConstants>(context);

    std::lock_guard lock(resourceMutex);
    if (!ensurePipeline()) return false;

    ID3D11ShaderResourceView* contactShadow{};
    if (composition.contactShadows)
    {
        if (!depth || !scene || !light)
        {
            logOnce("Shadows: a sun pass without its depth or constants keeps the game's shadows.");
            return false;
        }

        contactShadow = sun_contact_shadows::trace(context, depth.Get(), *scene, *light);
        if (!contactShadow) return false;

        // The diagnostics view lights everything as beyond the shadow range, with the trace's shadow.
        if (composition.onlyContactShadows) sun = reflectsSky(sun) ? SunShader::Far : SunShader::RainFar;
    }

    // The rays' shadow alone, unless the shadow map's joins it: everything lit as beyond the shadow range.
    if (rayTraced && !composition.rayTraced.withGameShadows) sun = reflectsSky(sun) ? SunShader::Far : SunShader::RainFar;

    writeOptions(context, composition, rayTraced != nullptr);
    ComPtr<ID3D11ShaderResourceView> gameView, gameRayTracedView;
    ComPtr<ID3D11Buffer> gameOptions;
    context->PSGetShaderResources(ContactShadowSlot, 1, gameView.GetAddressOf());
    context->PSGetShaderResources(RayTracedSlot, 1, gameRayTracedView.GetAddressOf());
    context->PSGetConstantBuffers(OptionsSlot, 1, gameOptions.GetAddressOf());
    context->PSSetShader(replacements[static_cast<std::size_t>(sun)].Get(), nullptr, 0);
    context->PSSetShaderResources(ContactShadowSlot, 1, &contactShadow);
    context->PSSetShaderResources(RayTracedSlot, 1, &rayTraced);
    context->PSSetConstantBuffers(OptionsSlot, 1, optionsBuffer.GetAddressOf());
    performance::Timed timed(context, performance::Pass::SunLight);
    context->DrawInstanced(call.count, call.instances, call.first, call.firstInstance);
    timed.end();
    context->PSSetShader(gameShader, nullptr, 0);
    context->PSSetShaderResources(ContactShadowSlot, 1, gameView.GetAddressOf());
    context->PSSetShaderResources(RayTracedSlot, 1, gameRayTracedView.GetAddressOf());
    context->PSSetConstantBuffers(OptionsSlot, 1, gameOptions.GetAddressOf());
    if (composition.softShadows) logOnce("Shadows: the sun pass draws with soft shadows.");
    if (rayTraced) logOnce("Shadows: the sun pass draws with ray-traced shadows.");

    return true;
}
}

void attach(ID3D11Device* gameDevice) noexcept
{
    const bool wantedOn = latch.latest().any();

    std::lock_guard lock(resourceMutex);
    device = gameDevice;
    if (wantedOn) ensurePipeline();
}

void detach() noexcept
{
    {
        std::lock_guard lock(resourceMutex);
        for (auto& replacement : replacements) replacement.Reset();
        optionsBuffer.Reset();
        device.Reset();
        pipelineFailed = false;
    }

    std::lock_guard shaderLock(shaderMutex);
    for (std::size_t sun = 0; sun < sunShaders.size(); ++sun)
        for (std::size_t i = 0; i < CopiesPerShader; ++i)
        {
            sunShaders[sun][i] = nullptr;
            shaderOwners[sun][i].Reset();
        }
}

void configure(const Configuration& configuration) noexcept
{
    const Composition composition{
        .softShadows = configuration.effects.runs(Effect::SoftShadows),
        .contactShadows = configuration.effects.runs(Effect::SunContactShadows),
        .rayTracedShadows = configuration.effects.runs(Effect::RayTracedShadows),
        .sun = configuration.settings.sun,
        .rayTraced = configuration.settings.rayTracedShadows,
        .onlyContactShadows = configuration.diagnostics.onlyScreenSpaceShadows,
    };

    latch.set(composition);

    if (!composition.any()) return;

    std::lock_guard lock(resourceMutex);
    ensurePipeline();
}

bool failed() noexcept
{
    return pipelineFailed;
}

Health softShadowHealth()
{
    if (pipelineFailed) return Health::unavailable("Soft shadows are unavailable (see PhantomFX.log).");

    return {};
}

Health rayTracedShadowHealth()
{
    if (pipelineFailed) return Health::unavailable("Ray-traced shadows are unavailable (see PhantomFX.log).");

    return ray_tracing::health();
}

void shaderCreated(const void* code, std::size_t size, IUnknown* shader) noexcept
{
    const auto checksum = dxbcHashOf(code, size);
    if (!checksum || !shader) return;

    const auto text = toHex(*checksum);
    for (std::size_t sun = 0; sun < std::size(SunChecksums); ++sun)
    {
        if (text != SunChecksums[sun]) continue;

        std::lock_guard lock(shaderMutex);
        for (std::size_t i = 0; i < CopiesPerShader; ++i)
        {
            if (sunShaders[sun][i].load()) continue;

            shaderOwners[sun][i] = shader;
            sunShaders[sun][i] = shader;
            log(std::string("Identified the sun light pass (") + SunNames[sun] + ").");
            return;
        }
    }
}

bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call) noexcept
{
    if (!active.load(std::memory_order_relaxed) || call.indexed || call.instances != 1 || call.count != 3) return false;

    try
    {
        ComPtr<ID3D11PixelShader> gameShader;
        context->PSGetShader(gameShader.GetAddressOf(), nullptr, nullptr);
        const auto sun = sunOf(gameShader.Get());
        if (sun == SunShader::None) return false;

        const auto composition = latch.frame();

        return drawSun(context, call, sun, gameShader.Get(), composition);
    }
    catch (const std::exception& error)
    {
        std::lock_guard lock(resourceMutex);
        logOnce(std::string("Shadows: a sun pass kept the game's shadows: ") + error.what());
    }
    catch (...)
    {
    }

    return false;
}

void present() noexcept
{
    frame.fetch_add(1, std::memory_order_relaxed);
    turnPattern = upscaler::replaces();
    const auto current = latch.present();
    active = current.any() && !pipelineFailed;
    contact_shadow_trace::tracesThisFrame(contact_shadow_trace::Tracer::Sun, active && current.contactShadows);
}
}
