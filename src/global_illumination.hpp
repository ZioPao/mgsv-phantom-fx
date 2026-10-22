#pragma once

#include "ui_redirect.hpp"

#include <d3d11.h>
#include <cstddef>
#include <cstdint>
#include <string>

// Ray-traced global illumination after AMD's GI-1.2 in place of the game's probe lighting
// (docs/DEVELOPMENT.md, "Global illumination"). The game's meshes are captured as it draws them
// (gi_capture.hpp) and traced on a Direct3D 12 device beside the game's (gi_renderer.hpp).
//
// The frame: once the game's G-buffer lists have run, the immediate context packs the G-buffer and
// the new captures into shared textures and signals a shared fence (the kick, before the list with
// the game's probe upscale). Direct3D 12 builds the scene and traces it while the game draws its
// shadow maps. The list with the deferred resolve waits for it; there indirect_light puts the light
// in place of the game's probe light in the diffuse light the resolve reads. The game's light target
// keeps its probe light, for the game's exposure.
//
// The same scene gives the sun's ray-traced shadows (docs/DEVELOPMENT.md, "Ray-traced shadows"),
// with or without the light: Direct3D 12 traces them into a shared texture, which the sun pass reads
// (shadows.hpp). The sun pass runs in the resolve's list, so it waits for them too.
namespace phantom_fx::global_illumination
{
struct Options
{
    bool enabled{};   // The light, in place of the game's probe light
    bool shadows{};   // The sun's ray-traced shadows
    float sunSize{0.53f}; // The sun's angular diameter for the shadows, in degrees
    bool showScene{}; // Diagnostics: the output shows the traced scene
    bool debugShadows{}; // Diagnostics: the shadow debug view and probe (shadowDebug, shadowProbe), with the shadows
    // A cost measurement leaves these out: the light passes (the output keeps its last light), and
    // the capture of meshes the scene lacks or that move (it keeps what it holds).
    bool light = true, capture = true;
};

void attach(ID3D11Device* device, ID3D11DeviceContext* immediate) noexcept;
// Releases everything; only when no Direct3D work can run.
void detach() noexcept;
// Applies from the frame recorded after the next present.
void configure(const Options& options) noexcept;
// Empty while off or running; otherwise why it is not, for the overlay.
std::string problem();
// What the scene holds, for the overlay's diagnostics.
std::string status();

// ---- Recording, on the game's render and worker threads ----
void pixelShaderCreated(const void* code, std::size_t size, IUnknown* shader) noexcept;
void vertexShaderCreated(const void* code, std::size_t size, IUnknown* shader) noexcept;
void vertexShaderBound(ID3D11DeviceContext* context, IUnknown* shader) noexcept;
void bufferMapped(ID3D11Resource* buffer, void* data) noexcept;
void bufferUnmapped(ID3D11Resource* buffer) noexcept;
// Before a draw; never takes it. Notes the sky's probe constants, and captures meshes.
void draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call) noexcept;

// indirect_light calls these from the game's passes it handles.
// The sky light pass: the G-buffer normals it binds.
void noteNormals(ID3D11DeviceContext* context, ID3D11ShaderResourceView* normals) noexcept;
// The probe upscale: the kick happens before its list runs. `depth` is the G-buffer's depth.
void noteUpscale(ID3D11DeviceContext* context, ID3D11ShaderResourceView* depth) noexcept;
// A sun pass: its light and scene constants as bound. Inside the game's shadow map range
// (`shadowMapped`), its constants also give that range.
void noteSun(ID3D11DeviceContext* context, bool shadowMapped) noexcept;
// The resolve: its list waits for the light. `albedo` is the G-buffer's albedo (sRGB view).
void noteResolve(ID3D11DeviceContext* context, ID3D11ShaderResourceView* albedo) noexcept;
// The light at the render size for the resolve to add: R16G16B16A16_FLOAT, in the game's diffuse
// light units. Null while off or before the first frame.
ID3D11ShaderResourceView* light(UINT width, UINT height) noexcept;
// The sun's ray-traced shadow at the render size for the sun pass: R16_FLOAT, 1 where the sun
// reaches. Null while off, or until Direct3D 12 has written one at this size.
ID3D11ShaderResourceView* sunShadow(UINT width, UINT height) noexcept;
// Diagnostics: the shadow debug view at the render size, R8G8B8A8_UNORM to blend over the image by
// its alpha (DebugShadows in shaders/gi_shadow.hlsl), and the probe at the screen's centre as text.
// Null and empty while off.
ID3D11ShaderResourceView* shadowDebug(UINT width, UINT height) noexcept;
std::string shadowProbe();
bool active() noexcept;
// On and lighting frames: the game's probe light and SSAO make way for it. Until then they stay.
bool running() noexcept;

// FinishCommandList and command list lifetime.
void recorded(std::uint64_t list, std::uint64_t context) noexcept;
void forget(std::uint64_t list) noexcept;

// ---- Immediate context ----
// Before the immediate context executes `list`: the kick, or the wait for the light.
void executing(ID3D11DeviceContext* context, std::uint64_t list) noexcept;
void present() noexcept;
}
