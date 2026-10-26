#pragma once

#include "camera.hpp"
#include "gi_renderer.hpp"
#include "ui_redirect.hpp"

#include <d3d11.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Ray-traced lights (docs/DEVELOPMENT.md, "Ray-traced lights"): the game's point and spot lights, the
// flashlights among them, shadowed by rays through the ray-traced scene (global_illumination.hpp) in
// place of their shadow maps or of none. A light's draws keep their stencil work but add no light;
// Direct3D 12 traces its shadow, and at the sun pass's list, which waits for that, it is drawn again
// with the game's own shader for its kind of light with specular and without a shadow map, scaled by
// its shadow, within the volume (a box) its draws had.
namespace phantom_fx::local_lights
{
// Diagnostics: which lights are drawn over all their reach, shadowed by the rays alone, rather than
// within the volume the game bounds each by: none, those the game gives a shadow map, or every one.
enum class BeyondVolumes : std::uint8_t { None, ShadowMapped, All };

struct Options
{
    bool enabled{};
    // Multiplies the game's flashlights' light; their reach grows with its square root, as a brighter light
    // falls to the same dimness further away.
    float flashlightBrightness{1};
    BeyondVolumes beyondVolumes{};
};

void attach(ID3D11Device* device, ID3D11DeviceContext* immediate) noexcept;
// Releases everything; only when no Direct3D work can run.
void detach() noexcept;
// Applies from the frame recorded after the next present.
void configure(const Options& options) noexcept;
// Empty while off or ready; otherwise why the lights stay the game's, for the overlay.
std::string problem();

// ---- Recording, on the game's render and worker threads ----
void pixelShaderCreated(const void* code, std::size_t size, IUnknown* shader) noexcept;
// Before an indexed draw. Returns true when it made a light's draw itself, so the game's is skipped.
bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call) noexcept;
// indirect_light calls this at a pass that comes after the game's lights and before anything reads them:
// the sun pass, indoors the sky's specular pass in its place, or the resolve, as some scenes draw no sun
// pass at all. Each list that has one draws the lights recorded before it again.
void noteDrawPoint(ID3D11DeviceContext* context) noexcept;
// FinishCommandList and command list lifetime.
void recorded(std::uint64_t list, std::uint64_t context) noexcept;
void forget(std::uint64_t list) noexcept;

// ---- Immediate context ----
// At the kick: the lights recorded for the frame `camera` draws and not traced yet, world space, the
// largest on screen first (at most GI_MAX_LOCAL_LIGHTS); those given are drawn with the rays' shadow.
std::vector<gi_renderer::LocalLight> takeForTrace(const camera::SceneConstants& camera) noexcept;
// Before the immediate context executes `list`, after the global illumination's wait: at each list with
// a draw point (noteDrawPoint), the lights of the lists run so far are drawn.
void executing(ID3D11DeviceContext* context, std::uint64_t list) noexcept;
void present() noexcept;
}
