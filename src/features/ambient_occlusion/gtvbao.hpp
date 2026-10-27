#pragma once

#include "phantom_fx_settings.hpp"
#include "ui_redirect.hpp"

#include <d3d11.h>
#include <string>

// GTVBAO, ground-truth visibility bitmask ambient occlusion (docs/DEVELOPMENT.md, "Ambient occlusion",
// "GTVBAO"), replaces the game's SSAO with the visibility bitmask march's occlusion
// (visibility_bitmask.hpp). Fox Engine multiplies all of a pixel's light by its occlusion in the
// deferred resolve, sunlight included. GTVBAO multiplies the indirect light only: the diffuse light
// buffer right after the engine upscales its probe and sky lighting into it, before the sun and the
// local lights add to it. While it runs, the resolve reads a white texture in place of the game's
// occlusion, and the game's own SSAO passes are left out (indirect_light_pass.hpp).
namespace phantom_fx::gtvbao
{
void attach(ID3D11Device* device) noexcept;
// Releases everything; only when no Direct3D work can run (device teardown, unload).
void detach() noexcept;
// Applies from the frame recorded after the next present.
void configure(const Configuration& configuration) noexcept;
// For the budget (fine while off).
Health health();
void present() noexcept;

// ---- Recording, on the game's render and worker threads (indirect_light_pass.cpp) ----
// The occlusion from the upscale's inputs; false leaves the game's upscale alone this frame.
bool compute(ID3D11DeviceContext* context, ID3D11ShaderResourceView* depth, ID3D11ShaderResourceView* normals, ID3D11ShaderResourceView* specular,
    ID3D11RenderTargetView* light, ID3D11Buffer* scene);
// Draws the upscale once more, multiplying the occlusion into what the game's drew.
void apply(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call);
// The last occlusion, for the diagnostics; null before the first.
ID3D11ShaderResourceView* occlusion() noexcept;
}
