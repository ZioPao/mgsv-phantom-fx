#pragma once

#include "phantom_fx_settings.hpp"

#include <d3d11.h>
#include <filesystem>

// Phantom FX's settings in force (PhantomFX.ini), its diagnostics, the enhancements switch (F1) and the
// effects a cost measurement leaves out. Every change goes to every module as a Configuration: each
// takes what it needs (configure in each).
namespace phantom_fx::core
{
// `directory` holds PhantomFX.ini. On an unsupported game build the effects that read the game's memory
// stay off (FeatureInfo::needsSupportedBuild) and so does upscaling.
void initialize(const std::filesystem::path& directory, bool supportedBuild) noexcept;
// The game's device: a first launch on a GPU other than NVIDIA's upscales with FSR.
void attach(ID3D11Device* device) noexcept;
// Saves a change PhantomFX.ini does not have yet.
void detach() noexcept;
// Each present: a change saves once the settings settle.
void present() noexcept;

// ---- Settings UI (menu.cpp) ----
Settings currentSettings();
// Applies at once; PhantomFX.ini is written when the settings settle, or at detach().
void applySettings(const Settings& settings);
Diagnostics currentDiagnostics();
void applyDiagnostics(const Diagnostics& diagnostics);
// The lighting effects (Effect, effects.hpp) together, for comparing with the game's own lighting
// (F1). Not saved: each start begins with them on, as their own settings say.
bool enhancementsOn();
void setEnhancements(bool on);
void toggleEnhancements();
// The plan `settings` would run by on this game build with the enhancements switch as it is, without a
// cost measurement's left-out effects: for the budget and the measurement's steps.
EffectPlan effectPlan(const Settings& settings);

// ---- Cost measurement (performance.hpp) ----
void leaveOut(const LeftOut& effects);
}
