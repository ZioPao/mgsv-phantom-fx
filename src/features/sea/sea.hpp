#pragma once

#include "game_shaders.hpp"
#include "phantom_fx_settings.hpp"
#include "ui_redirect.hpp"

#include <d3d11.h>
#include <wrl/client.h>
#include <optional>

// The sea (docs/DEVELOPMENT.md, "Sea"). The game's sea (Primitive_TppFourierOcean) is a 64 x 64 wave
// field the CPU makes each frame, drawn after the lighting with fixed sky colours for its reflection, so
// nothing on it is lit or reflected like the rest of the world, and the ray tracing does not see it.
//
// While the game shows its sea, the add-on draws its own into the G-buffer, at the frame's first draw of
// the main view's G-buffer: waves from an ocean spectrum, on the GPU (sea_shaders.hpp), as one of the
// game's materials, so the game's lighting, fog and screen-space reflections, the global illumination and
// path tracing take it as any surface. The game's sea draws are skipped.
namespace phantom_fx::sea
{
// The immediate context reads the sea's level back at present.
void attach(ID3D11Device* device, ID3D11DeviceContext* immediate) noexcept;
// Releases everything; only when no Direct3D work can run (device teardown, unload).
void detach() noexcept;
// Applies from the frame recorded after the next present.
void configure(const Configuration& configuration) noexcept;
// Each present: whether the next frame draws the sea, its time and its wind.
void present() noexcept;
// For the budget (fine while off).
Health health();

// ---- Recording, on the game's render and worker threads ----
// A context's render targets were bound: the G-buffer's three and its depth, or others.
void targetsBound(ID3D11DeviceContext* context, UINT count, ID3D11RenderTargetView* const* targets, ID3D11DepthStencilView* depth) noexcept;
// Before a draw, whoever draws it: the frame's first draw into the main view's G-buffer has the sea drawn
// before it; the game's sea draws tell that the game shows its sea.
void note(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept;
// The game's sea draws, skipped while the add-on draws the sea; returns true when the game's draw must be
// skipped.
bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept;

// ---- On the immediate context ----
// Where the sea is in this frame's G-buffer, at `width` x `height`: its depth where it drew, which equals the
// G-buffer's depth where nothing nearer covers it. Null when this frame has no sea of the add-on's.
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> surfaceDepth(UINT width, UINT height) noexcept;
// The sea's level, for the ray-traced scene's plane: as the game's sea last gave it, read back from the GPU a
// second or so late. Absent when this frame has no sea of the add-on's, or before the first read.
std::optional<float> level() noexcept;
}
