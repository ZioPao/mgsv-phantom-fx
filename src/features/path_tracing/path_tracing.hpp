#pragma once

#include "game_shaders.hpp"
#include "phantom_fx_settings.hpp"
#include "ui_redirect.hpp"

#include <d3d11.h>
#include <utility>

// Path tracing on Direct3D 11 (docs/DEVELOPMENT.md, "Path tracing"). While DLSS Ray Reconstruction denoises
// and the global illumination runs, the ray-traced scene's kick traces the reflections too
// (ray_tracing::reflection, reflections.hlsl), and the game's reflections make way for them:
//
// - the resolve adds them to the game's specular light (addReflections);
// - the sun pass leaves the sky's reflection out, and indoors the sky's specular pass is skipped
//   (sun_light_pass.cpp);
// - the screen-space reflections stay only where a surface was drawn in front of the G-buffer's, water and
//   glass, which the traced reflections leave out (draw).
namespace phantom_fx::path_tracing
{
void attach(ID3D11Device* device) noexcept;
// Releases everything; only when no Direct3D work can run (device teardown, unload).
void detach() noexcept;
// Applies from the frame recorded after the next present.
void configure(const Configuration& configuration) noexcept;
void present() noexcept;

// ---- Recording, on the game's render and worker threads ----
// The resolve (indirect_light_pass.cpp): the game's `specular` light with the traced reflections added, at
// `size`; null keeps the game's.
ID3D11ShaderResourceView* addReflections(ID3D11DeviceContext* context, ID3D11ShaderResourceView* specular, std::pair<UINT, UINT> size);
// Before a draw, whoever draws it: the scene's depth the screen-space reflections' mask reads.
void note(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept;
// The screen-space reflections' blend over the scene, drawn here while path tracing runs; returns true when
// the game's draw must be skipped.
bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept;
}
