// DLSS settings, drawn in ReShade's overlay.
#include "frame_log.hpp"
#include "menu_style.hpp"
#include "upscaler.hpp"

#include <imgui.h>
#include <reshade.hpp> // Routes ImGui calls through ReShade's overlay.
#include <format>
#include <string>

namespace mgsv_dlss::upscaler
{
namespace
{
using namespace menu;

// One coloured line on what DLSS is doing right now.
void drawStatus(const Status& state, const Settings& wanted, const Diagnostics& options)
{
    if (!state.supportedBuild) statusLine(Red, "This game version is not supported, so DLSS stays off.");
    else if (wanted.mode == Mode::Off) statusLine(Grey, "DLSS is off. The game's FXAA is in use.");
    else if (state.failed) statusLine(Red, std::format("DLSS stopped: {} Choose a mode again to retry.", state.failure));
    else if (upscales(wanted.mode) && !state.upscalingAvailable) statusLine(Red, "Upscaling is unavailable (see MGSVDLSS.log); DLAA still works.");
    else if (options.gameFxaa) statusLine(Yellow, "Showing the game's FXAA for comparison (Diagnostics).");
    else if (!state.fxaaRecognised) statusLine(Yellow, "Waiting for the game to draw its anti-aliasing.");
    else if (!state.running && state.lastFallback.empty()) statusLine(Yellow, "DLSS is waiting for the game's 3D view.");
    else if (!state.running) statusLine(Yellow, std::format("DLSS is waiting: {}.", state.lastFallback));
    else if (state.renderWidth == state.outputWidth && state.renderHeight == state.outputHeight)
        statusLine(Green, std::format("DLAA active at {}x{}, preset {}.", state.outputWidth, state.outputHeight, label(wanted.preset)));
    else
        statusLine(Green, std::format("DLSS {} active: {}x{} -> {}x{}, preset {}.", label(wanted.mode), state.renderWidth, state.renderHeight,
            state.outputWidth, state.outputHeight, label(wanted.preset)));
}

// A slider whose value applies when it is released: each new render scale rebuilds the engine's
// targets, and each new texture bias makes new samplers. Returns true on release.
bool releasedSlider(const char* name, float& value, float low, float high, const char* format, float width)
{
    static const char* editing{};
    static float edited{};
    float shown = editing == name ? edited : value;
    ImGui::SetNextItemWidth(width);
    if (ImGui::SliderFloat(name, &shown, low, high, format, ImGuiSliderFlags_AlwaysClamp))
    {
        editing = name;
        edited = shown;
    }

    if (!ImGui::IsItemDeactivatedAfterEdit()) return false;

    editing = nullptr;
    value = shown;

    return true;
}

// Returns true when a setting changed.
bool drawSettings(Settings& wanted, float width)
{
    bool changed{};
    ImGui::SetNextItemWidth(width);
    if (ImGui::BeginCombo("Mode", std::string(label(wanted.mode)).c_str()))
    {
        for (const auto mode : {Mode::Off, Mode::Dlaa, Mode::Quality, Mode::Balanced, Mode::Performance, Mode::UltraPerformance, Mode::Custom})
            if (ImGui::Selectable(std::string(label(mode)).c_str(), mode == wanted.mode))
            {
                wanted.mode = mode;
                changed = true;
            }

        ImGui::EndCombo();
    }
    tooltip("DLAA replaces the game's FXAA at full resolution. Quality to Ultra Performance render the 3D scene below the output\n"
            "resolution and upscale it; the HUD and menus stay at full resolution. Off leaves the game unchanged.");

    if (wanted.mode == Mode::Custom)
    {
        float percent = wanted.customScale * 100;
        if (releasedSlider("Render scale", percent, Settings::MinScale * 100, Settings::MaxScale * 100, "%.0f%%", width))
        {
            wanted.customScale = percent / 100;
            changed = true;
        }
        tooltip("Render width as a share of the output width. Applies when released.");
    }

    ImGui::BeginDisabled(wanted.mode == Mode::Off);
    ImGui::SetNextItemWidth(width);
    if (ImGui::BeginCombo("DLSS preset", std::string(label(wanted.preset)).c_str()))
    {
        for (const auto preset : {Preset::Default, Preset::K, Preset::J, Preset::M, Preset::L})
            if (ImGui::Selectable(std::string(label(preset)).c_str(), preset == wanted.preset))
            {
                wanted.preset = preset;
                changed = true;
            }

        ImGui::EndCombo();
    }
    tooltip("Default lets DLSS choose (K for DLAA).");

    float sharpening = wanted.sharpness * 100;
    ImGui::SetNextItemWidth(width);
    if (ImGui::SliderFloat("Sharpening", &sharpening, 0, Settings::MaxSharpness * 100, "%.0f%%", ImGuiSliderFlags_AlwaysClamp))
    {
        wanted.sharpness = sharpening / 100;
        changed = true;
    }
    tooltip("AMD FidelityFX RCAS on the DLSS image. 0% is off.");

    changed |= releasedSlider("Texture detail bias", wanted.mipBiasOffset, Settings::MinMipBias, Settings::MaxMipBias, "%+.2f", width);
    tooltip("Added to the automatic texture LOD bias for the render size. Lower is sharper but can shimmer. Applies when released.");
    ImGui::EndDisabled();

    changed |= ImGui::Checkbox("Unlock frame rate", &wanted.unlockFps);
    tooltip("Lifts the game's 60 fps cap, like framerate_control \"Variable\" in its settings file. Off keeps the game's setting.");

    return changed;
}

void drawDiagnostics(const Status& state, float width)
{
    ImGui::TextDisabled("For troubleshooting. Leave these at their defaults.");
    auto options = currentDiagnostics();
    bool changed{};
    const char* views[]{"Final image", "Motion vectors", "Object motion (red)"};
    ImGui::SetNextItemWidth(width);
    changed |= ImGui::Combo("Debug view", &options.debugView, views, static_cast<int>(std::size(views)));
    changed |= ImGui::Checkbox("Jitter off", &options.noJitter);
    tooltip("DLSS gets the same pixel positions every frame. Edges stay aliased.");
    changed |= ImGui::Checkbox("Game FXAA (comparison)", &options.gameFxaa);
    if (changed) applyDiagnostics(options);

    ImGui::Spacing();
    ImGui::Text("Frames: %llu with DLSS, %llu without; %llu history resets", static_cast<unsigned long long>(state.evaluations),
        static_cast<unsigned long long>(state.fallbacks), static_cast<unsigned long long>(state.resets));
    ImGui::Text("Render %ux%u, output %ux%u, texture LOD bias %+.2f", state.renderWidth, state.renderHeight, state.outputWidth, state.outputHeight,
        state.mipBias);
    if (!state.lastFallback.empty()) ImGui::TextWrapped("Last fallback: %s", state.lastFallback.c_str());
    ImGui::Text("Jitter %.3f, %.3f px", state.jitter[0], state.jitter[1]);
    if (frame_log::installed()) ImGui::TextUnformatted("F10 records the next frame into the MGSVDLSS-frame-log folder.");
}
}

void drawMenu()
{
    constexpr float width = 260;
    auto wanted = currentSettings();
    const auto options = currentDiagnostics();
    const auto state = status();
    drawStatus(state, wanted, options);
    ImGui::Separator();
    if (drawSettings(wanted, width)) applySettings(wanted);

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Diagnostics")) drawDiagnostics(state, width);
}
}
