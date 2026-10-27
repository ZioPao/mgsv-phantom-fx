#include "features/lamp_soft_shadows/lamp_soft_shadows.hpp"

#include "d3d11_helpers.hpp"
#include "features/lamp_soft_shadows/lamp_soft_shadows_shaders.hpp"
#include "log.hpp"

#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>

namespace phantom_fx::lamp_soft_shadows
{
namespace
{
using Microsoft::WRL::ComPtr;

// The game's light shaders the soft shadows replace (game_lights.hpp): the point light with a
// dual-paraboloid shadow map and the spot light's plain projection, both quartic, with specular and
// a shadow map.
struct ReplacedLight
{
    std::string_view checksum;
    const char* name;
    const char* source;
};
constexpr ReplacedLight ReplacedLights[]{
    {"afbc12167037e4e0d653da184c7e3e82", "SSLighting2_Point", shaders::PointLight},
    {"ac0947110cb0f17f4ffe930a6a11617d", "SSLighting2_Spot", shaders::SpotLight},
};

// cLampShadows (b6) of the replacements.
struct Options
{
    float soft, searchTexels, hardening, softness, darkness, padding[3];
};
static_assert(sizeof(Options) % 16 == 0);

// The penumbra's shape, in metres and texels: a 10 cm softness at a 30 cm blocker separation, 24
// texels of search, as renodx-mgsv's point lights start.
constexpr Options LampOptions{.soft = 1, .searchTexels = 24, .hardening = 20, .softness = 0.1f, .darkness = 1};

std::atomic<bool> wantedRunning{}, pipelineFailed{};

// ---- Direct3D, guarded by resourceMutex ----
std::mutex resourceMutex;
ComPtr<ID3D11Device> device;
std::array<ComPtr<ID3D11PixelShader>, std::size(ReplacedLights)> replacements;
ComPtr<ID3D11Buffer> optionsBuffer;

// Compiles the shaders once; call with resourceMutex held. Returns false when they are unavailable.
bool ensurePipeline()
{
    if (optionsBuffer) return true;
    if (!device || pipelineFailed) return false;

    try
    {
        for (std::size_t i = 0; i < replacements.size(); ++i) replacements[i] = pixelShader(device.Get(), ReplacedLights[i].source, ReplacedLights[i].name);

        // Last, so it marks a complete pipeline. The options never change.
        const D3D11_BUFFER_DESC desc{.ByteWidth = sizeof(Options), .Usage = D3D11_USAGE_IMMUTABLE, .BindFlags = D3D11_BIND_CONSTANT_BUFFER};
        const D3D11_SUBRESOURCE_DATA data{.pSysMem = &LampOptions};
        check(device->CreateBuffer(&desc, &data, optionsBuffer.GetAddressOf()), "Creating the lamp shadow options");
        log("Lamp soft shadow shaders ready.");

        return true;
    }
    catch (const std::exception& error)
    {
        pipelineFailed = true;
        for (auto& replacement : replacements) replacement.Reset();
        log(std::string("Lamp soft shadows unavailable: ") + error.what());

        return false;
    }
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
    for (auto& replacement : replacements) replacement.Reset();
    optionsBuffer.Reset();
    device.Reset();
    pipelineFailed = false;
}

void configure(const Configuration& configuration) noexcept
{
    const bool running = configuration.effects.runs(Effect::LampSoftShadows);
    wantedRunning = running;
    if (!running) return;

    std::lock_guard lock(resourceMutex);
    ensurePipeline();
}

Health health()
{
    if (pipelineFailed) return Health::unavailable("Lamp soft shadows are unavailable (see PhantomFX.log).");

    return {};
}

std::optional<Replacement> replacementFor(const game_lights::GameLight& light)
{
    const auto replaced = std::ranges::find(ReplacedLights, std::string_view(light.checksum), &ReplacedLight::checksum);
    if (replaced == std::end(ReplacedLights)) return std::nullopt;

    std::lock_guard lock(resourceMutex);
    if (!ensurePipeline()) return std::nullopt;

    return Replacement{replacements[static_cast<std::size_t>(replaced - std::begin(ReplacedLights))].Get(), optionsBuffer.Get()};
}
}
