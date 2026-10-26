#pragma once

#include "ui_redirect.hpp"

#include <d3d11.h>
#include <cstddef>

// What the add-on does to the game's shadows (docs/DEVELOPMENT.md, "Soft shadows" and "Screen-space
// shadows"):
//
// - Soft shadows filter the sun's shadow map with contact-hardening soft shadows (PCSS): the
//   penumbra widens with the distance between the occluder and the receiver, at the sun's angular
//   size.
// - Screen-space shadows add Bend Studio's screen-space shadow, traced through the depth buffer, to
//   the sun, to every point and spot light, or to both. The sun's is one trace a frame; the lights'
//   is one trace per light, so it costs far more where many lights are on screen.
// - Lamp soft shadows filter a point or spot light's own shadow map with the same
//   contact-hardening soft shadows as the sun's, in a replacement of the game's light shader.
// - Ray-traced shadows take the sun's shadow from rays through the global illumination's scene
//   (global_illumination::sunShadow; docs/DEVELOPMENT.md, "Ray-traced shadows"), everywhere the sun
//   lights. The shadow map keeps only what the rays leave out, the alpha-tested meshes' shadows.
//
// The engine lights the sun in one full-screen pass whose shaders sample the shadow map
// themselves. While an option is on, that pass draws with replacement shaders that compute the
// same with the option added. A point or spot light draws its volume twice, to mark the stencil and
// then to light; with the lights' screen-space shadows on, the add-on traces before the first and
// scales the second's light by the shadow, which it keeps in the light targets' unused alpha. Off
// draws the game's own shaders.
namespace phantom_fx::shadows
{
struct Options
{
    bool soft{};              // Contact-hardening soft shadows from the sun's shadow map
    float sunSize{0.53f};     // The sun's angular diameter for the soft shadows, in degrees
    bool turnPattern{};       // DLSS accumulates frames, so the soft shadows' samples turn each frame
    bool screenSpaceSun{};    // Bend Studio's screen-space shadows on the sun
    bool screenSpaceLights{}; // Bend Studio's screen-space shadows on every point and spot light
    float contactContrast{1}; // How hard and dark the contact shadows' trace is (Bend's ShadowContrast)
    float contactReach{80};   // How far the trace marches, in pixels of the render size
    bool softLocalShadows{};  // Contact-hardening soft shadows from a point light's own shadow map
    bool rayTraced{};         // The sun's ray-traced shadows: the rays alone shadow the sun
    bool withShadowMap{};     // With rayTraced: the shadow map's as well, near the camera (the darker of the two)
    bool onlyScreenSpace{};   // Diagnostics: the sun is shadowed by the screen-space shadow alone
};

void attach(ID3D11Device* device) noexcept;
// Releases everything; only when no Direct3D work can run (device teardown, unload).
void detach() noexcept;
// Applies from the frame recorded after the next present.
void configure(const Options& options) noexcept;
// The add-on's shaders did not compile, so the game draws its own shadows (see PhantomFX.log).
bool failed() noexcept;

// ---- Recording, on the game's render and worker threads ----
void shaderCreated(const void* code, std::size_t size, IUnknown* shader) noexcept;
// Before a draw. The sun pass and the local lights' draws, while an option is on, are drawn here
// instead; returns true when the game's draw must be skipped.
bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call) noexcept;
// FinishCommandList on `context`: the next sun pass traces again.
void contextFinished(ID3D11DeviceContext* context) noexcept;
// Each present. The game records a whole frame between presents, so all of a frame's light passes
// see the same options.
void present() noexcept;
}
