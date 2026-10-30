#pragma once

#include "features/ray_traced_lights/light_choice.hpp"
#include "passes/ray_tracing/gi_d3d12.hpp"
#include "passes/ray_tracing/shaders/gi_shared.h"

#include <d3d12.h>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

// Path tracing on the ray-traced scene's Direct3D 12 device (docs/DEVELOPMENT.md, "Path tracing"): the
// reflections' rays (reflections.hlsl), and the point and spot lights' (direct_light.hlsl) with their ReSTIR
// reservoirs at one render size and the light history that carries them from frame to frame.
namespace phantom_fx::path_tracing_rays
{
// The shared textures the passes read and write, as gi_renderer.cpp opened them; null for a pass not traced.
struct Textures
{
    ID3D12Resource* guideNormals{};
    ID3D12Resource* guideSpecular{};
    ID3D12Resource* guideMaterial{};
    ID3D12Resource* guideTranslucency{};
    ID3D12Resource* lampMasks{}; // Null without masked spots
    ID3D12Resource* reflection{};
    ID3D12Resource* reflectionDistance{};
    ID3D12Resource* lampDiffuse{};
    ID3D12Resource* lampSpecular{};
};

class Rays
{
public:
    // Makes the lamps' reservoirs for a render size. Throws on Direct3D 12 errors.
    Rays(ID3D12Device* device, UINT width, UINT height);

    UINT width() const { return renderWidth; }
    UINT height() const { return renderHeight; }

    // The light history for this frame's `lights`: each of the last frame's lights' place among them, and
    // whether the last frame's reservoirs hold (lamps traced then, at this size, without a camera cut).
    void fill(gi::GIFrameConstants& constants, std::span<const light_choice::LocalLight> lights, bool traceLamps, bool cameraCut);
    // Their views in the frame's tables, by the slots reflections.hlsl and direct_light.hlsl give them.
    void describe(gi_d3d12::Descriptors& descriptors, const Textures& textures) const;
    // The lamps' passes: their reservoirs, then their light. Needs the frame's scene built.
    void recordLamps(gi_d3d12::Recorder& recorder) const;
    // The reflections, which read the light's cache: after its passes.
    void recordReflections(gi_d3d12::Recorder& recorder) const;

private:
    // A light the last frame's reservoirs name, to find it again in this frame's list.
    struct Seen
    {
        std::uint64_t key;
        std::array<float, 3> position, color;
    };

    UINT renderWidth, renderHeight;
    std::vector<Seen> lastLights;
    bool hasHistory{};

    // This frame's reservoirs after the first pass (SampleLamps); after the second (ShadeLamps), for the next
    // frame's first.
    gi_d3d12::ComPtr<ID3D12Resource> reservoirs, history;
};
}
