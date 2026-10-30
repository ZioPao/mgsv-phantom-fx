#pragma once

#include "features/frame_generation/frame_generation.hpp"
#include "game_shaders.hpp"
#include "phantom_fx_settings.hpp"
#include "ui_redirect.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

// NVIDIA DLSS or AMD FSR (fsr_upscaler.hpp) after Fox Engine's depth of field and before its grading,
// in place of its FXAA, at native resolution (DLAA) or upscaling from a lower render size
// (render_scale.hpp, ui_redirect.hpp); docs/DEVELOPMENT.md ("What the add-on changes") describes how.
// The ReShade add-on (addon.cpp) forwards the game's Direct3D calls, with native objects. Command
// lists are identified by their native deferred context or ID3D11CommandList.
namespace phantom_fx::upscaler
{
struct Status
{
    bool supportedBuild{}, failed{}, running{}, fxaaRecognised{}, upscalingAvailable{};
    // DLSS Ray Reconstruction has not failed this session; the last frame was Ray Reconstruction's.
    bool reconstructionAvailable{}, rayReconstruction{};
    std::string upscalerName; // "DLSS", "DLSS Ray Reconstruction", or "FSR" with its version ("FSR 3.1.5") once it ran
    std::string failure, lastFallback;
    unsigned renderWidth{}, renderHeight{}, outputWidth{}, outputHeight{};
    std::uint64_t evaluations{}, fallbacks{}, resets{};
    float jitter[2]{};
    float mipBias{};
};

// ---- Setup ----
// `directory` is the add-on's, which holds the DLSS and FSR DLLs.
void initialize(const std::filesystem::path& directory) noexcept;
// Applies at once. On an unsupported game build upscaling stays off.
void configure(const Configuration& configuration) noexcept;
// DLSS or FSR replaces the game's FXAA: on, on a supported build, not failed or left out.
bool replaces() noexcept;
// The game's device and immediate context; NGX receives `ngxContext`, ReShade's wrapper where there
// is one, so other add-ons can follow the evaluation.
void attach(ID3D11Device* device, ID3D11DeviceContext* immediate, ID3D11DeviceContext* ngxContext) noexcept;
// Releases everything; only when no Direct3D work can run (device teardown, unload).
void detach() noexcept;
// The swap chain's size: the output size the upscaler upscales to.
void setOutputSize(unsigned width, unsigned height) noexcept;

// ---- Recording, on the game's render and worker threads ----
// Notes the FXAA pass as it is recorded. Returns true when the draw was made on the UI target instead
// (the 2D pass and GammaControl while upscaling), or must not be made (FXAA, as DLSS runs before the
// grading), so the game's own draw must be skipped; the render target clear likewise.
bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept;
// After the notes, before the draws that may take the draw: notes the camera velocity pass, whose input
// holds the moving meshes' velocity, the depth of field's composite (for Ray Reconstruction's guide), and
// the game's grading, which DLSS runs before. While upscaling,
// true for its full-size draws (the lens flare layer's composite and the grading), which are then
// bound to the output size (ui_redirect.hpp) and must be made in that state, whoever makes them;
// afterPostDraw then puts the game's bindings back. The motion blur's draws as well, while upscaling
// or, with the scene in float, at every upscaler setting, on the add-on's float surfaces.
bool beforePostDraw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept;
void afterPostDraw(ID3D11DeviceContext* context) noexcept;
bool clearTarget(ID3D11DeviceContext* context, ID3D11RenderTargetView* view, const float color[4]) noexcept;
void depthStencilCleared(ID3D11DeviceContext* context, ID3D11DepthStencilView* view, UINT flags, float depth, UINT8 stencil) noexcept;
// Fills `biased` and returns true when any sampler should carry the texture LOD bias for the render size.
bool biasSamplers(ID3D11DeviceContext* context, unsigned count, ID3D11SamplerState* const* samplers, ID3D11SamplerState** biased) noexcept;
// A buffer mapped for writing, then unmapped with the data still mapped: the scene constants get
// the jitter here.
void bufferMapped(ID3D11Resource* buffer, void* data) noexcept;
void bufferUnmapped(ID3D11Resource* buffer) noexcept;
// What `from` recorded now belongs to `into`: FinishCommandList, or a command list executed in a
// deferred context.
void recorded(std::uint64_t into, std::uint64_t from) noexcept;
void forget(std::uint64_t contextOrList) noexcept;

// ---- Immediate context ----
// Before the immediate context executes `list`.
void executing(ID3D11DeviceContext* immediate, std::uint64_t list) noexcept;
void present() noexcept;

// ---- Frame generation (presenter.hpp) ----
// While the game presents through Direct3D 12: the upscaler's depth and motion are shared with it,
// and GammaControl also draws the frame before the HUD. Set at each present.
void setFrameGeneration(bool on) noexcept;
// What frame generation needs from the frame being presented, before present().
frame_generation::Inputs frameGenerationInputs() noexcept;

// ---- Settings UI (menu.cpp) ----
Status status();
}
