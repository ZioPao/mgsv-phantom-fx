#pragma once

#include "passes/ray_tracing/gi_d3d12.hpp"

#include <cstdint>

// The point and spot lights' shadow rays on the ray-traced scene's Direct3D 12 device
// (docs/DEVELOPMENT.md, "Ray-traced lights"): a ray-traced share of the lights a pixel, then a filter no
// wider than the penumbra (light_shadows.hlsl). The light pass reads the result (local_lights.hpp).
namespace phantom_fx::light_shadow_rays
{
// The rays, then the filter's three passes, at the render size; the rays alone for DLSS Ray Reconstruction
// (`forReconstruction`), which denoises itself. Needs the frame's scene built.
void record(gi_d3d12::Recorder& recorder, std::uint32_t width, std::uint32_t height, bool forReconstruction);
}
