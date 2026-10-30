// Path tracing on Direct3D 12; path_tracing_rays.hpp describes it.
#include "features/path_tracing/path_tracing_rays.hpp"

#include "performance.hpp"

#include <cmath>

namespace phantom_fx::path_tracing_rays
{
namespace
{
using gi_d3d12::groups;
using gi_d3d12::Kernel;

// A reservoir a render pixel: its light and samples, contribution weight, surface normal and view depth
// (PackReservoir in direct_light.hlsl).
constexpr DXGI_FORMAT ReservoirFormat = DXGI_FORMAT_R32G32B32A32_UINT;
// How far a light the game moves (a flashlight) may go in a frame and still be found again.
constexpr float MovingReach = 1;
// A light of the last frame not in this one's (CurrentLampOf in direct_light.hlsl).
constexpr std::uint32_t Gone = GI_MAX_LOCAL_LIGHTS;

float distanceBetween(const std::array<float, 3>& a, const std::array<float, 3>& b)
{
    return std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}
}

Rays::Rays(ID3D12Device* device, UINT width, UINT height) : renderWidth(width), renderHeight(height)
{
    const auto texture = [&](const char* what) {
        return gi_d3d12::createTexture(device, width, height, ReservoirFormat, 1, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, what);
    };
    reservoirs = texture("Creating the lamps' reservoirs");
    history = texture("Creating the lamps' last reservoirs");
}

void Rays::fill(gi::GIFrameConstants& constants, std::span<const light_choice::LocalLight> lights, bool traceLamps, bool cameraCut)
{
    // Each of the last frame's lights' place in this frame's list: the same light by its key, else one the game
    // moves (a flashlight, whose key has its place) as the nearest of its colour that no other took.
    std::array<std::uint32_t, GI_MAX_LOCAL_LIGHTS> current;
    current.fill(Gone);
    std::vector<bool> isTaken(lights.size());
    for (std::size_t last = 0; last < lastLights.size(); ++last)
        for (std::size_t now = 0; now < lights.size(); ++now)
            if (!isTaken[now] && lights[now].key == lastLights[last].key)
            {
                current[last] = static_cast<std::uint32_t>(now);
                isTaken[now] = true;
                break;
            }

    for (std::size_t last = 0; last < lastLights.size(); ++last)
    {
        if (current[last] != Gone) continue;

        float nearest = MovingReach;
        for (std::size_t now = 0; now < lights.size(); ++now)
        {
            const float distance = distanceBetween(lights[now].position, lastLights[last].position);
            if (isTaken[now] || lights[now].color != lastLights[last].color || distance >= nearest) continue;

            nearest = distance;
            current[last] = static_cast<std::uint32_t>(now);
        }

        if (current[last] != Gone) isTaken[current[last]] = true;
    }

    for (std::size_t i = 0; i < current.size(); ++i)
    {
        auto& row = constants.lampHistory[i / 4];
        (i % 4 == 0 ? row.x : i % 4 == 1 ? row.y : i % 4 == 2 ? row.z : row.w) = current[i];
    }

    constants.localLightCount.z = hasHistory && traceLamps && !cameraCut ? 1 : 0;

    lastLights.clear();
    for (const auto& light : lights) lastLights.push_back({light.key, light.position, light.color});
    hasHistory = traceLamps;
}

void Rays::describe(gi_d3d12::Descriptors& d, const Textures& textures) const
{
    if (textures.guideNormals) d.texture(18, textures.guideNormals, DXGI_FORMAT_R16G16B16A16_FLOAT);
    if (textures.guideSpecular) d.texture(19, textures.guideSpecular, DXGI_FORMAT_R8G8B8A8_UNORM);
    if (textures.lampMasks) d.texture(20, textures.lampMasks, DXGI_FORMAT_R8G8B8A8_UNORM);
    if (textures.guideMaterial) d.texture(21, textures.guideMaterial, DXGI_FORMAT_R8G8B8A8_UNORM);
    if (textures.guideTranslucency) d.texture(22, textures.guideTranslucency, DXGI_FORMAT_R8_UNORM);
    if (textures.reflection) d.outputTexture(93, textures.reflection, DXGI_FORMAT_R16G16B16A16_FLOAT);
    if (textures.reflectionDistance) d.outputTexture(94, textures.reflectionDistance, DXGI_FORMAT_R16_FLOAT);
    if (textures.lampDiffuse) d.outputTexture(95, textures.lampDiffuse, DXGI_FORMAT_R16G16B16A16_FLOAT);
    if (textures.lampSpecular) d.outputTexture(96, textures.lampSpecular, DXGI_FORMAT_R16G16B16A16_FLOAT);
    d.outputTexture(97, reservoirs.Get(), ReservoirFormat);
    d.outputTexture(98, history.Get(), ReservoirFormat);
}

void Rays::recordLamps(gi_d3d12::Recorder& r) const
{
    r.mark("path-traced lamps", performance::Pass::LampLight);
    r.dispatch(Kernel::SampleLamps, groups(renderWidth, 8), groups(renderHeight, 8));
    r.barrier();
    r.dispatch(Kernel::ShadeLamps, groups(renderWidth, 8), groups(renderHeight, 8));
    r.barrier();
}

void Rays::recordReflections(gi_d3d12::Recorder& r) const
{
    r.mark("path-traced reflections", performance::Pass::Reflections);
    r.dispatch(Kernel::TraceReflections, groups(renderWidth, 8), groups(renderHeight, 8));
    r.barrier();
}
}
