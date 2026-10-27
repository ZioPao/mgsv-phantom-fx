#pragma once

#include "phantom_fx_settings.hpp"
#include "ui_redirect.hpp"

#include <d3d11.h>
#include <cstddef>
#include <cstdint>
#include <string>

// The ray-traced scene's frame on Direct3D 11 (docs/DEVELOPMENT.md, "Global illumination"): what the
// global illumination (gi_light.hpp), the sun's ray-traced shadows (gi_shadow.hpp) and the point and
// spot lights' (light_shadow_rays.hpp) trace. The scene is the game's own, read from its memory
// (engine_scene.hpp), and traced on a Direct3D 12 device beside the game's (gi_renderer.hpp); the
// camera and the terrain's inputs come from its draws (gi_inputs.hpp).
//
// The frame: once the game's G-buffer lists have run, the immediate context packs the G-buffer into
// shared textures and signals a shared fence (the kick, before the list with
// the game's probe upscale). Direct3D 12 builds the scene and traces it while the game draws its
// shadow maps. The list with the deferred resolve waits for it; there the indirect light pass puts the light
// in place of the game's probe light in the diffuse light the resolve reads. The game's light target
// keeps its probe light, for the game's exposure.
//
// The same scene gives the sun's ray-traced shadows (docs/DEVELOPMENT.md, "Ray-traced shadows"),
// with or without the light: Direct3D 12 traces them into a shared texture, which the sun pass reads
// (sun_light_pass.hpp). The sun pass runs in the resolve's list, so it waits for them too. So do the
// point and spot lights' shadows (local_lights.hpp), a bit a light, drawn before the sun pass's list.
namespace phantom_fx::ray_tracing
{
void attach(ID3D11Device* device, ID3D11DeviceContext* immediate) noexcept;
// Releases everything; only when no Direct3D work can run.
void detach() noexcept;
// Applies from the frame recorded after the next present.
void configure(const Configuration& configuration) noexcept;
// What the ray-traced effects share: whether the scene's frame runs (fine while off).
Health health();
// What the scene holds, for the overlay's diagnostics.
std::string status();

// ---- Recording, on the game's render and worker threads ----
void pixelShaderCreated(const void* code, std::size_t size, IUnknown* shader) noexcept;
void vertexShaderCreated(const void* code, std::size_t size, IUnknown* shader) noexcept;
void vertexShaderBound(ID3D11DeviceContext* context, IUnknown* shader) noexcept;
void bufferMapped(ID3D11Resource* buffer, void* data) noexcept;
void bufferUnmapped(ID3D11Resource* buffer) noexcept;
// Before a draw; never takes it. Notes the sky's probe constants, the camera and the terrain's inputs.
void draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call) noexcept;

// The indirect light pass (indirect_light_pass.hpp) calls these from the game's passes it handles.
// The sky light pass: the G-buffer normals it binds.
void noteNormals(ID3D11DeviceContext* context, ID3D11ShaderResourceView* normals) noexcept;
// The probe upscale: the kick happens before its list runs. `depth` is the G-buffer's depth.
void noteUpscale(ID3D11DeviceContext* context, ID3D11ShaderResourceView* depth) noexcept;
// A sun pass: its light and scene constants as bound. Inside the game's shadow map range
// (`shadowMapped`), its constants also give that range.
void noteSun(ID3D11DeviceContext* context, bool shadowMapped) noexcept;
// Indoors, the sky's specular pass in the sun pass's place (SunLight_ES), which gives no sun: the point
// and spot lights are drawn before its list, which waits for their shadows.
void noteSkySpecular(ID3D11DeviceContext* context) noexcept;
// The resolve: its list waits for the light. `albedo` is the G-buffer's albedo (sRGB view).
void noteResolve(ID3D11DeviceContext* context, ID3D11ShaderResourceView* albedo) noexcept;
// The light at the render size for the resolve to add: R16G16B16A16_FLOAT, in the game's diffuse
// light units. Null while off or before the first frame.
ID3D11ShaderResourceView* light(UINT width, UINT height) noexcept;
// The sun's ray-traced shadow at the render size for the sun pass: R16_FLOAT, 1 where the sun
// reaches. Null while off, or until Direct3D 12 has written one at this size.
ID3D11ShaderResourceView* sunShadow(UINT width, UINT height) noexcept;
// The point and spot lights' shadows this frame's kick traced, at the render size: R16_FLOAT, the share of
// the lights local_lights::takeForTrace gave that reaches each pixel. Null when none were traced.
ID3D11ShaderResourceView* localLightShadow(UINT width, UINT height) noexcept;
// Diagnostics: the shadow debug view at the render size, R8G8B8A8_UNORM to blend over the image by
// its alpha (DebugShadows in gi_shadow.hlsl), and the probe at the screen's centre as text.
// Null and empty while off.
ID3D11ShaderResourceView* shadowDebug(UINT width, UINT height) noexcept;
// Diagnostics: the light's denoiser history at the render size, R8G8B8A8_UNORM to blend over the image by
// its alpha (CompositeLight in gi_light.hlsl). Null while off.
ID3D11ShaderResourceView* lightDebug(UINT width, UINT height) noexcept;
// Diagnostics: the probes at the screen's centre as text, with the debug view on: the sun's, while there is
// a sun, and the lamps', with the ray-traced lights.
std::string shadowProbe();
bool active() noexcept;
// The global illumination's light is on and lighting frames: the game's probe light and SSAO make way
// for it. Until then they stay.
bool lightRuns() noexcept;

// FinishCommandList and command list lifetime.
void recorded(std::uint64_t list, std::uint64_t context) noexcept;
void forget(std::uint64_t list) noexcept;

// ---- Immediate context ----
// Before the immediate context executes `list`: the kick, or the wait for the light.
void executing(ID3D11DeviceContext* context, std::uint64_t list) noexcept;
void present() noexcept;
}
