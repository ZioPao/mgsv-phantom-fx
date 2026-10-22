// The Phantom FX settings, drawn in ReShade's overlay.
#include "atmosphere.hpp"
#include "character.hpp"
#include "depth_of_field.hpp"
#include "frame_generation.hpp"
#include "frame_log.hpp"
#include "fsr_frame_generation.hpp"
#include "global_illumination.hpp"
#include "hdr.hpp"
#include "indirect_light.hpp"
#include "menu_style.hpp"
#include "performance.hpp"
#include "presenter.hpp"
#include "shadows.hpp"
#include "upscaler.hpp"
#include "weather.hpp"

#include <imgui.h>
#include <reshade.hpp> // Routes ImGui calls through ReShade's overlay.
#include <algorithm>
#include <cctype>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>

namespace phantom_fx::upscaler
{
namespace
{
using namespace menu;

constexpr Settings Defaults{};

// One coloured line on what the upscaler is doing right now.
void drawStatus(const Status& state, const Settings& wanted, const Diagnostics& options)
{
    const auto& name = state.upscalerName;
    const auto nativeMode = label(Mode::Dlaa, wanted.upscaler);
    // FSR has no presets.
    const auto preset = wanted.upscaler == Upscaler::Dlss ? std::format(", preset {}", label(wanted.preset)) : std::string();
    if (!state.supportedBuild) statusLine(Red, "This game version is not supported, so upscaling stays off.");
    else if (wanted.mode == Mode::Off) statusLine(Grey, "Upscaling is off. The game's FXAA is in use.");
    else if (state.failed) statusLine(Red, std::format("{} stopped: {} Choose a mode or upscaler again to retry.", name, state.failure));
    else if (upscales(wanted.mode) && !state.upscalingAvailable)
        statusLine(Red, std::format("Upscaling is unavailable (see PhantomFX.log); {} still works.", nativeMode));
    else if (options.gameFxaa) statusLine(Yellow, "Showing the game's FXAA for comparison (Diagnostics).");
    else if (!state.fxaaRecognised) statusLine(Yellow, "Waiting for the game to draw its anti-aliasing.");
    else if (!state.running && state.lastFallback.empty()) statusLine(Yellow, std::format("{} is waiting for the game's 3D view.", name));
    else if (!state.running) statusLine(Yellow, std::format("{} is waiting: {}.", name, state.lastFallback));
    else if (state.renderWidth == state.outputWidth && state.renderHeight == state.outputHeight)
        statusLine(Green, std::format("{} {} active at {}x{}{}.", name, nativeMode, state.outputWidth, state.outputHeight, preset));
    else
        statusLine(Green, std::format("{} {} active: {}x{} -> {}x{}{}.", name, label(wanted.mode, wanted.upscaler), state.renderWidth,
            state.renderHeight, state.outputWidth, state.outputHeight, preset));
}

// A slider whose value applies when it is released: each new render scale rebuilds the engine's
// targets, and each new texture bias makes new samplers. Returns true on release.
bool releasedSlider(const char* name, float& value, float low, float high, const char* format)
{
    static const char* editing{};
    static float edited{};
    float shown = editing == name ? edited : value;
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

// After a slider: a button that puts its value back to the default, shown while it differs.
// Returns true when pressed.
bool resetButton(const char* slider, float& value, float standard)
{
    if (value == standard) return false;

    ImGui::SameLine();
    ImGui::PushID(slider);
    const bool pressed = ImGui::SmallButton("Reset");
    ImGui::PopID();
    if (pressed) value = standard;

    return pressed;
}

// Returns true when the setting changed.
bool drawFrameGeneration(Settings& wanted)
{
    bool changed{};
    // Until Streamline reports what the GPU supports, every DLSS mode is offered.
    const auto supported = frame_generation::capabilities();
    if (ImGui::BeginCombo("Frame generation", std::string(label(wanted.frameGeneration)).c_str()))
    {
        for (int i = 0; i < static_cast<int>(FrameGeneration::Count); ++i)
        {
            const auto mode = static_cast<FrameGeneration>(i);
            bool unsupported{};
            if (mode == FrameGeneration::Dynamic) unsupported = supported.maxMultiplier && !supported.dynamic;
            else if (multiplier(mode)) unsupported = supported.maxMultiplier && multiplier(mode) > supported.maxMultiplier;

            if (ImGui::Selectable(std::string(label(mode)).c_str(), mode == wanted.frameGeneration, unsupported ? ImGuiSelectableFlags_Disabled : 0))
            {
                wanted.frameGeneration = mode;
                changed = true;
            }
        }

        ImGui::EndCombo();
    }
    tooltip("Frames generated between the ones the game renders, with either upscaler.\n"
            "DLSS Frame Generation: 2x needs an RTX 40, 3x to 6x and Dynamic an RTX 50. Dynamic reaches the display's\n"
            "refresh rate. It turns on NVIDIA Reflex.\n"
            "FSR frame generation: 2x on any GPU, machine-learned on Radeon RX 9000.\n"
            "Turning it on, or switching between DLSS and FSR, takes a restart, as the game then presents through\n"
            "Direct3D 12; other changes apply at once.");

    // The route the game started with is the one that runs until a restart.
    const bool fsrRoute = presenter::route() == presenter::Route::Fsr;
    const bool sameRoute = (wanted.frameGeneration == FrameGeneration::Fsr) == fsrRoute;
    if (presenter::active() && wanted.frameGeneration != FrameGeneration::Off && !sameRoute)
        statusLine(Yellow, "Restart the game to switch frame generation.");
    else if (presenter::active()) statusLine(Grey, fsrRoute ? fsr_frame_generation::status() : frame_generation::status());
    else if (wanted.frameGeneration != FrameGeneration::Off)
    {
        const auto reason = presenter::status();
        statusLine(Yellow, presenter::installed() && sameRoute && !reason.empty() ? reason : "Restart the game to start frame generation.");
    }

    return changed;
}

// Returns true when a setting changed.
bool drawUpscaling(const Status& state, Settings& wanted)
{
    drawStatus(state, wanted, currentDiagnostics());
    ImGui::Spacing();

    bool changed{};
    if (ImGui::BeginCombo("Upscaler", std::string(label(wanted.upscaler)).c_str()))
    {
        for (const auto upscaler : {Upscaler::Dlss, Upscaler::Fsr})
            if (ImGui::Selectable(std::string(label(upscaler)).c_str(), upscaler == wanted.upscaler))
            {
                wanted.upscaler = upscaler;
                changed = true;
            }

        ImGui::EndCombo();
    }
    tooltip("NVIDIA DLSS needs an NVIDIA RTX GPU.\n"
            "AMD FSR runs on any GPU: FSR 4 on Radeon RX 7000 and 9000, FSR 3.1 on the rest.");

    if (ImGui::BeginCombo("Mode", std::string(label(wanted.mode, wanted.upscaler)).c_str()))
    {
        for (const auto mode : {Mode::Off, Mode::Dlaa, Mode::Quality, Mode::Balanced, Mode::Performance, Mode::UltraPerformance, Mode::Custom})
            if (ImGui::Selectable(std::string(label(mode, wanted.upscaler)).c_str(), mode == wanted.mode))
            {
                wanted.mode = mode;
                changed = true;
            }

        ImGui::EndCombo();
    }
    tooltip("DLAA and Native AA replace the game's FXAA at full resolution. Quality to Ultra Performance render the 3D scene\n"
            "below the output resolution and upscale it; the HUD and menus stay at full resolution. Off leaves the game unchanged.");

    if (wanted.mode == Mode::Custom)
    {
        beginIndent();
        float percent = wanted.customScale * 100;
        if (releasedSlider("Render scale", percent, Settings::MinScale * 100, Settings::MaxScale * 100, "%.0f%%"))
        {
            wanted.customScale = percent / 100;
            changed = true;
        }
        tooltip("Render width as a share of the output width. Applies when released.");
        changed |= resetButton("Render scale", wanted.customScale, Defaults.customScale);
        endIndent();
    }

    ImGui::BeginDisabled(wanted.mode == Mode::Off);
    if (wanted.upscaler == Upscaler::Dlss)
    {
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
    }

    float sharpening = wanted.sharpness * 100;
    if (ImGui::SliderFloat("Sharpening", &sharpening, 0, Settings::MaxSharpness * 100, "%.0f%%", ImGuiSliderFlags_AlwaysClamp))
    {
        wanted.sharpness = sharpening / 100;
        changed = true;
    }
    tooltip("AMD FidelityFX RCAS on the upscaled image. 0% is off.");
    changed |= resetButton("Sharpening", wanted.sharpness, Defaults.sharpness);

    changed |= releasedSlider("Texture detail bias", wanted.mipBiasOffset, Settings::MinMipBias, Settings::MaxMipBias, "%+.2f");
    tooltip("Added to the automatic texture LOD bias for the render size. Lower is sharper but can shimmer. Applies when released.");
    changed |= resetButton("Texture detail bias", wanted.mipBiasOffset, Defaults.mipBiasOffset);
    ImGui::EndDisabled();

    ImGui::Spacing();
    changed |= drawFrameGeneration(wanted);

    return changed;
}

// Returns true when a setting changed.
bool drawSoftShadows(Settings& wanted)
{
    bool changed = ImGui::Checkbox("Soft shadows (PCSS)", &wanted.softShadows);
    tooltip("Contact-hardening soft shadows from the sun's shadow map: sharp where an object meets the ground, softer the\n"
            "further the shadow falls from it, as the sun's disc gives. Off keeps the game's evenly sharp shadow edges.");
    // The ray-traced shadows show the sun size while on.
    if (!wanted.softShadows || wanted.rayTracedShadows) return changed;

    beginIndent();
    changed |= ImGui::SliderFloat("Sun size", &wanted.sunSize, Settings::MinSunSize, Settings::MaxSunSize, "%.2f deg", ImGuiSliderFlags_AlwaysClamp);
    tooltip("The sun's angular diameter, which sets how fast shadows soften with distance. 0.53 is the real sun.");
    changed |= resetButton("Sun size", wanted.sunSize, Defaults.sunSize);
    if (shadows::failed()) statusLine(Red, "Soft shadows are unavailable (see PhantomFX.log).");
    endIndent();

    return changed;
}

// Returns true when a setting changed.
bool drawRayTracedShadows(Settings& wanted)
{
    bool changed = ImGui::Checkbox("Ray-traced shadows", &wanted.rayTracedShadows);
    tooltip("The sun's shadows from rays traced through the scene the global illumination captures: as soft as the sun's\n"
            "disc makes them, exact where objects meet the ground, and as far as you can see, where the game's shadows end\n"
            "at about 90 m. Leaves and grass keep the game's shadow map near you and let half the sun through further away.\n"
            "It replaces soft shadows and sun contact shadows while on. Needs a GPU that traces rays (DXR 1.1). Off keeps\n"
            "the game's shadow map.");
    if (!wanted.rayTracedShadows) return changed;

    beginIndent();
    changed |= ImGui::SliderFloat("Sun size", &wanted.sunSize, Settings::MinSunSize, Settings::MaxSunSize, "%.2f deg", ImGuiSliderFlags_AlwaysClamp);
    tooltip("The sun's angular diameter, which sets how fast shadows soften with distance. 0.53 is the real sun.");
    changed |= resetButton("Sun size", wanted.sunSize, Defaults.sunSize);
    if (const auto problem = global_illumination::problem(); !problem.empty()) statusLine(Yellow, problem);
    if (shadows::failed()) statusLine(Red, "Ray-traced shadows are unavailable (see PhantomFX.log).");
    endIndent();

    return changed;
}

// Returns true when a setting changed.
bool drawAmbientOcclusion(Settings& wanted)
{
    bool changed = ImGui::Checkbox("Ambient occlusion (GTVBAO)", &wanted.ambientOcclusion);
    tooltip("Ground-truth visibility bitmask ambient occlusion in place of the game's SSAO, whatever its SSAO setting. It darkens\n"
            "only the indirect light, so sunlit surfaces keep their brightness. Off keeps the game's SSAO, which darkens all light.");
    if (!wanted.ambientOcclusion) return changed;

    beginIndent();
    changed |= ImGui::SliderFloat("Occlusion radius", &wanted.occlusionRadius, Settings::MinOcclusionRadius, Settings::MaxOcclusionRadius,
        "%.2f m", ImGuiSliderFlags_AlwaysClamp);
    tooltip("How far around a surface GTVBAO looks for what occludes it.");
    changed |= resetButton("Occlusion radius", wanted.occlusionRadius, Defaults.occlusionRadius);
    changed |= ImGui::SliderFloat("Occlusion strength", &wanted.occlusionStrength, 0, Settings::MaxOcclusionStrength, "%.2f",
        ImGuiSliderFlags_AlwaysClamp);
    tooltip("1 is physically based; higher darkens occluded areas more, 0 turns the occlusion off.");
    changed |= resetButton("Occlusion strength", wanted.occlusionStrength, Defaults.occlusionStrength);
    if (const auto problem = indirect_light::occlusionProblem(); !problem.empty()) statusLine(Yellow, problem);
    endIndent();

    return changed;
}

// Returns true when a setting changed.
bool drawIndirectLight(Settings& wanted)
{
    bool changed = ImGui::Checkbox("Indirect lighting (SSILVB)", &wanted.indirectLight);
    tooltip("Screen-space indirect lighting with visibility bitmask: light bounces off nearby surfaces onto others, taking\n"
            "their colour, such as sunlit ground brightening the underside of a jeep. It only knows what is on screen.\n"
            "Off leaves the game's indirect light unchanged.");
    if (!wanted.indirectLight) return changed;

    beginIndent();
    changed |= ImGui::SliderFloat("Indirect light radius", &wanted.indirectLightRadius, Settings::MinIndirectLightRadius,
        Settings::MaxIndirectLightRadius, "%.2f m", ImGuiSliderFlags_AlwaysClamp);
    tooltip("How far around a surface SSILVB looks for surfaces that light it.");
    changed |= resetButton("Indirect light radius", wanted.indirectLightRadius, Defaults.indirectLightRadius);
    changed |= ImGui::SliderFloat("Indirect light strength", &wanted.indirectLightStrength, 0, Settings::MaxIndirectLightStrength, "%.2f",
        ImGuiSliderFlags_AlwaysClamp);
    tooltip("1 is physically based; higher brightens the bounced light, 0 turns it off.");
    changed |= resetButton("Indirect light strength", wanted.indirectLightStrength, Defaults.indirectLightStrength);
    if (const auto problem = indirect_light::lightProblem(); !problem.empty()) statusLine(Yellow, problem);
    endIndent();

    return changed;
}

// Returns true when a setting changed.
bool drawGlobalIllumination(Settings& wanted)
{
    bool changed = ImGui::Checkbox("Global illumination (GI-1.2)", &wanted.globalIllumination);
    tooltip("Ray-traced indirect light after AMD's GI-1.2: sunlight and skylight bounce between surfaces, also those off\n"
            "screen, and the sky only lights what it can reach. It replaces the game's baked probe light and SSAO, and\n"
            "GTVBAO and SSILVB while on. Needs a GPU that traces rays (DXR 1.1). Off keeps the game's indirect light.");
    if (!wanted.globalIllumination) return changed;

    beginIndent();
    changed |= ImGui::SliderFloat("Global illumination strength", &wanted.globalIlluminationStrength, 0, Settings::MaxGlobalIlluminationStrength,
        "%.2f", ImGuiSliderFlags_AlwaysClamp);
    tooltip("1 is physically based; higher brightens the indirect light, 0 leaves only the sun and the lamps.");
    changed |= resetButton("Global illumination strength", wanted.globalIlluminationStrength, Defaults.globalIlluminationStrength);
    if (const auto problem = global_illumination::problem(); !problem.empty()) statusLine(Yellow, problem);
    endIndent();

    return changed;
}

// Returns true when a setting changed.
bool drawVolumetricFog(Settings& wanted)
{
    bool changed = ImGui::Checkbox("Volumetric fog", &wanted.volumetricFog);
    tooltip("Fog with the sun shadowed in it: light shafts through trees, buildings and dust, and shade where the sun cannot\n"
            "reach. Shadows reach about 90 m, as the game's do. Off leaves the game's fog unchanged.");
    if (!wanted.volumetricFog) return changed;

    beginIndent();
    if (ImGui::BeginCombo("Look", std::string(label(wanted.atmosphereLook)).c_str()))
    {
        for (const auto look : {AtmosphereLook::Mgsv, AtmosphereLook::Physical})
            if (ImGui::Selectable(std::string(label(look)).c_str(), look == wanted.atmosphereLook))
            {
                wanted.atmosphereLook = look;
                changed = true;
            }

        ImGui::EndCombo();
    }
    tooltip("MGSV keeps the game's own sky and haze colours, with the sun shadowed in the fog.\n"
            "Physical draws the sky and the air as they are in nature (Hillaire's atmosphere, as in Unreal Engine), as\n"
            "bright as the game's sky, with height fog that gathers low, as thick as the game's weather makes its fog.");
    if (wanted.atmosphereLook == AtmosphereLook::Physical)
    {
        float haze = wanted.atmosphereHaze * 100;
        if (ImGui::SliderFloat("Haze", &haze, 0, 100, "%.0f%%", ImGuiSliderFlags_AlwaysClamp))
        {
            wanted.atmosphereHaze = haze / 100;
            changed = true;
        }
        tooltip("Dust in the air that fades the distance. A matter of taste: no setting is more physically based or closer to\n"
                "the game's look. 0% (the default) is clear air, about 120 km of view on a clear day; 100% is about 8 km, with a\n"
                "paler sky. Fog, sandstorms and rain are always at full thickness.");
        changed |= resetButton("Haze", wanted.atmosphereHaze, Defaults.atmosphereHaze);
    }
    if (const auto problem = atmosphere::problem(); !problem.empty()) statusLine(Yellow, problem);
    endIndent();

    return changed;
}

// Returns true when a setting changed.
bool drawDepthOfField(Settings& wanted)
{
    bool changed = ImGui::Checkbox("Depth of field", &wanted.depthOfField);
    tooltip("A gather in place of the game's depth of field, in cutscenes, gameplay and photo mode. Slight blur stays smooth\n"
            "at full resolution, what lies in front of the focus spreads over what lies behind it, and lights are blurred\n"
            "before the game's tonemap, so they stay bright. It needs Depth of Field on in the game's graphics settings.\n"
            "Off leaves the game's own.");
    if (wanted.depthOfField)
        if (const auto problem = depth_of_field::problem(); !problem.empty()) statusLine(Yellow, problem);

    const auto aperture = [](int blades) { return blades ? std::format("{} blades", blades) : std::string("Round"); };
    if (ImGui::BeginCombo("Aperture", aperture(wanted.apertureBlades).c_str()))
    {
        for (const int blades : {0, 5, 6, 7, 8, 9})
            if (ImGui::Selectable(aperture(blades).c_str(), blades == wanted.apertureBlades))
            {
                wanted.apertureBlades = blades;
                changed = true;
            }

        ImGui::EndCombo();
    }
    tooltip("The shape of out-of-focus lights: a round aperture, or one with 5 to 9 blades. For the gather and lens shots.");

    return changed;
}

// Returns true when a setting changed.
bool drawHdr(Settings& wanted)
{
    bool changed = ImGui::Checkbox("HDR output (HDR10)", &wanted.hdr);
    tooltip("The game in HDR10 on an HDR display: the sun, lamps, fire and reflections shine brighter than the game's white.\n"
            "It needs HDR on for the display in Windows. Turning it on or off takes a restart; the brightness applies at once.");

    const auto shown = hdr::output();
    if (shown == hdr::Output::Unavailable) statusLine(Red, "HDR output is unavailable (see PhantomFX.log).");
    else if (wanted.hdr != hdr::active()) statusLine(Yellow, "Restart the game to apply.");
    else if (shown == hdr::Output::Sdr) statusLine(Yellow, "HDR is off for this display in Windows, so the game shows SDR.");
    else if (shown == hdr::Output::Hdr10) statusLine(Green, "HDR10 active.");

    if (!wanted.hdr) return changed;

    beginIndent();
    // Until moved, the slider shows the display's own peak, which a reset goes back to.
    float peak = std::clamp(hdr::peakNitsFor(wanted.peakNits), Settings::MinPeakNits, Settings::MaxPeakNits);
    if (ImGui::SliderFloat("Peak brightness", &peak, Settings::MinPeakNits, Settings::MaxPeakNits,
            wanted.peakNits ? "%.0f nits" : "%.0f nits (display's)", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp))
    {
        wanted.peakNits = peak;
        changed = true;
    }
    tooltip("The brightest your display shows; by default, what it reports. The brightest the game gets lands on it.");
    changed |= resetButton("Peak brightness", wanted.peakNits, Defaults.peakNits);
    changed |= ImGui::SliderFloat("Paper white", &wanted.paperWhiteNits, Settings::MinPaperWhiteNits, Settings::MaxPaperWhiteNits, "%.0f nits",
        ImGuiSliderFlags_AlwaysClamp);
    tooltip("How bright the game's white is, the HUD and menus included. 203 nits is the HDR reference; raise it in a bright room.");
    changed |= resetButton("Paper white", wanted.paperWhiteNits, Defaults.paperWhiteNits);
    endIndent();

    return changed;
}

// Returns true when a setting changed.
bool drawVisuals(Settings& wanted)
{
    bool enhancements = enhancementsOn();
    if (ImGui::Checkbox("Visual enhancements (F1)", &enhancements)) setEnhancements(enhancements);
    tooltip("Ray-traced shadows, soft shadows, contact shadows, global illumination, GTVBAO, SSILVB and the volumetric fog\n"
            "together, as set below. Off shows the game's own lighting for comparison; F1 toggles it at any time. Not saved: each start begins\n"
            "with them on.");
    if (!enhancements) statusLine(Yellow, "Visual enhancements are off. F1 turns them back on.");

    bool changed{};
    ImGui::BeginDisabled(!enhancements);
    beginIndent();
    changed |= drawRayTracedShadows(wanted);
    // The ray-traced shadows take the place of both.
    ImGui::BeginDisabled(wanted.rayTracedShadows);
    changed |= drawSoftShadows(wanted);
    changed |= ImGui::Checkbox("Sun contact shadows", &wanted.sunScreenSpaceShadows);
    tooltip("Bend Studio's screen-space shadows for the sun: short shadows traced through the depth buffer. They add the\n"
            "contact shadows the sun's shadow map misses (feet, rocks, folds of clothing). Cheap: one trace a frame.");
    ImGui::EndDisabled();
    changed |= ImGui::Checkbox("Lamp contact shadows", &wanted.lightScreenSpaceShadows);
    tooltip("The same for every lamp, spot light and flashlight, which gives lights that have no shadow of their own one.\n"
            "Costly: one trace per light, so the frame rate drops where many lights are on screen, as in a base at night.");
    if ((wanted.sunScreenSpaceShadows || wanted.lightScreenSpaceShadows) && shadows::failed())
        statusLine(Red, "Contact shadows are unavailable (see PhantomFX.log).");

    changed |= drawGlobalIllumination(wanted);
    // The global illumination takes the place of both.
    ImGui::BeginDisabled(wanted.globalIllumination);
    changed |= drawAmbientOcclusion(wanted);
    changed |= drawIndirectLight(wanted);
    ImGui::EndDisabled();
    changed |= drawVolumetricFog(wanted);
    endIndent();
    ImGui::EndDisabled();

    ImGui::Spacing();
    changed |= drawDepthOfField(wanted);
    ImGui::Spacing();
    changed |= drawHdr(wanted);

    return changed;
}

// Returns true when a setting changed.
bool drawPhotoMode(Settings& wanted)
{
    statusLine(Grey, "F2 opens photo mode. Fly with WASD and the mouse, Space and Ctrl for up and down, Shift for faster. "
                     "F3 hides the HUD at any time.");

    bool changed = ImGui::SliderFloat("f-stop", &wanted.fNumber, Settings::MinFNumber, Settings::MaxFNumber, "f/%.1f",
        ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
    tooltip("Photo mode's aperture: a lower f-stop blurs more of what is out of focus. F4 turns the blur off.");
    changed |= resetButton("f-stop", wanted.fNumber, Defaults.fNumber);

    if (ImGui::BeginCombo("Lens shot frames", std::to_string(wanted.lensShotFrames).c_str()))
    {
        for (const int frames : {64, 128, 256, 512, 1024})
            if (ImGui::Selectable(std::to_string(frames).c_str(), frames == wanted.lensShotFrames))
            {
                wanted.lensShotFrames = frames;
                changed = true;
            }

        ImGui::EndCombo();
    }
    tooltip("F5 in photo mode takes a lens shot: the frozen scene rendered from points across the aperture, one per frame,\n"
            "and averaged, as a real lens forms its image, so it sees past the edges of what is out of focus. More frames\n"
            "make wide blurs smoother and take longer. Moving the camera or F5 again leaves it. Sharpest with DLAA.");

    const auto shot = depth_of_field::shotProgress();
    if (shot.total && shot.frames < shot.total) statusLine(Yellow, std::format("Lens shot: {} of {} frames.", shot.frames, shot.total));
    else if (shot.total) statusLine(Green, "Lens shot ready. Move the camera or press F5 to leave it.");

    return changed;
}

// "06:30" for a time in hours.
std::string clockText(float hours)
{
    const auto minutes = static_cast<int>(hours * 60);

    return std::format("{:02}:{:02}", minutes / 60, minutes % 60);
}

// The game's weather and time of day, to see the sky in every condition.
void drawWeather()
{
    if (!weather::supported())
    {
        statusLine(Red, "Unavailable for this game version.");
        return;
    }

    const auto now = weather::state();
    if (!now.available)
    {
        statusLine(Grey, "Available in a mission or free roam.");
        return;
    }

    // While dragged, the slider shows its own value; the game's clock follows a frame later.
    static std::optional<float> dragged;
    float hours = dragged.value_or(now.clock / 3600);
    if (ImGui::SliderFloat("Time of day", &hours, 0, 24, clockText(hours).c_str(), ImGuiSliderFlags_AlwaysClamp))
    {
        dragged = hours;
        weather::setClock(hours * 3600);
    }
    if (!ImGui::IsItemActive()) dragged.reset();
    tooltip("Sets the game's clock while dragged. The day goes on from there unless the clock is stopped.");

    bool stopped = now.clockStopped;
    if (ImGui::Checkbox("Stop the clock", &stopped)) weather::stopClock(stopped);

    static float fogDensity = 0.1f;
    const auto shown = now.forced ? std::string(weather::label(*now.forced)) : std::string("The game's");
    if (ImGui::BeginCombo("Weather", shown.c_str()))
    {
        if (ImGui::Selectable("The game's", !now.forced)) weather::release();
        for (std::size_t i = 0; i < now.typeAvailable.size(); ++i)
        {
            const auto type = static_cast<weather::Type>(i);
            if (ImGui::Selectable(std::string(weather::label(type)).c_str(), now.forced == type, now.typeAvailable[i] ? 0 : ImGuiSelectableFlags_Disabled))
                weather::force(type, fogDensity);
        }

        ImGui::EndCombo();
    }
    tooltip("Switches the weather at once and holds it over the game's own, as its mission scripts do. Greyed out: this\n"
            "location has no settings for that weather. A checkpoint saved meanwhile keeps it; The game's gives it back.");

    if (now.forced == weather::Type::Foggy)
    {
        beginIndent();
        if (ImGui::SliderFloat("Fog density", &fogDensity, 0, 1, "%.2f", ImGuiSliderFlags_AlwaysClamp)) weather::force(weather::Type::Foggy, fogDensity);
        endIndent();
    }

    if (!now.requested) return;

    if (!now.forced) statusLine(Grey, std::format("The game's weather: {}.", weather::label(*now.requested)));
    else if (now.requested != now.forced) statusLine(Yellow, std::format("The game overrides it with {} for now.", weather::label(*now.requested)));
}

// Whether `text` holds every word of `search`, in any case.
bool matches(std::string_view text, std::string_view search)
{
    const auto sameLetter = [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); };
    for (const auto word : std::views::split(search, ' '))
        if (!word.empty() && std::ranges::search(text, word, sameLetter).empty()) return false;

    return true;
}

// A combo whose list starts with a search box and shows the items whose name matches it. Returns the
// item picked.
template<class Item> std::optional<std::size_t> searchCombo(const char* label, const std::string& preview, std::span<const Item> items, std::optional<std::size_t> selected)
{
    if (!ImGui::BeginCombo(label, preview.c_str(), ImGuiComboFlags_HeightLargest)) return std::nullopt;

    // One list is open at a time, and each opens with an empty search.
    static char search[64];
    const bool opened = ImGui::IsWindowAppearing();
    if (opened)
    {
        search[0] = 0;
        ImGui::SetKeyboardFocusHere();
    }

    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##search", "Search", search, sizeof(search));
    std::optional<std::size_t> picked;
    // The list scrolls under the search box.
    ImGui::BeginChild("##items", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 16));
    for (std::size_t i = 0; i < items.size(); ++i)
    {
        const std::string name(items[i].name);
        if (!matches(name, search)) continue;

        const bool chosen = i == selected;
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Selectable(name.c_str(), chosen)) picked = i;
        ImGui::PopID();
        if (chosen && opened) ImGui::SetScrollHereY();
    }

    ImGui::EndChild();
    if (picked) ImGui::CloseCurrentPopup();
    ImGui::EndCombo();

    return picked;
}

// Play as another of the game's characters.
void drawCharacter()
{
    if (!character::supported())
    {
        statusLine(Red, "Unavailable for this game version.");
        return;
    }

    const auto now = character::state();
    if (!now.available)
    {
        statusLine(Grey, "Available in a mission or free roam.");
        return;
    }

    const auto characters = character::characters();
    const auto current = std::ranges::find_if(characters, [&](const character::Character& c) { return c.type == now.type && c.parts == now.parts; });
    const auto chosen = current != characters.end() ? std::optional(static_cast<std::size_t>(current - characters.begin())) : std::nullopt;
    const auto shown = chosen ? std::string(current->name) : std::string("Another outfit");
    if (const auto picked = searchCombo("Character", shown, characters, chosen)) character::choose(characters[*picked]);
    tooltip("Switches the player's character and outfit at once, as the game's scripts do for cutscenes. A checkpoint keeps it;\n"
            "the next mission starts with the character chosen for its sortie.");

    const auto faces = character::faces(now.type);
    if (faces.empty()) return;

    const auto face = std::ranges::find(faces, now.face, &character::Face::id);
    const auto faceChosen = face != faces.end() ? std::optional(static_cast<std::size_t>(face - faces.begin())) : std::nullopt;
    const auto faceShown = faceChosen ? face->name : std::format("Face {}", now.face);
    if (const auto picked = searchCombo("Face", faceShown, faces, faceChosen)) character::chooseFace(faces[*picked].id);
    tooltip("Any face of the game's soldiers. Search by number, skin (light, olive, fair, brown, dark), headgear or tattoo.");
}

// Returns true when a setting changed.
bool drawUtility(Settings& wanted)
{
    bool changed = ImGui::Checkbox("Unlock frame rate", &wanted.unlockFps);
    tooltip("Lifts the game's 60 fps cap, like framerate_control \"Variable\" in its settings file. Off keeps the game's setting.");

    ImGui::SeparatorText("Photo mode");
    changed |= drawPhotoMode(wanted);

    ImGui::SeparatorText("Weather and time");
    drawWeather();

    ImGui::SeparatorText("Character");
    drawCharacter();

    return changed;
}

void drawDiagnostics(const Status& state)
{
    ImGui::TextDisabled("For troubleshooting. Leave these at their defaults.");
    auto options = currentDiagnostics();
    bool changed{};
    const char* views[]{"Final image", "Motion vectors", "Object motion (red)"};
    changed |= ImGui::Combo("Debug view", &options.debugView, views, static_cast<int>(std::size(views)));
    changed |= ImGui::Checkbox("Jitter off", &options.noJitter);
    tooltip("The upscaler gets the same pixel positions every frame. Edges stay aliased.");
    changed |= ImGui::Checkbox("Game FXAA (comparison)", &options.gameFxaa);
    changed |= ImGui::Checkbox("Sun contact shadows only", &options.onlyScreenSpaceShadows);
    tooltip("With sun contact shadows on, the sun is shadowed by them alone, without the shadow map.");
    changed |= ImGui::Checkbox("Ray-traced shadows only", &options.onlyRayTracedShadows);
    tooltip("With ray-traced shadows on, the sun is shadowed by the rays alone, without the shadow map that gives the\n"
            "shadows of leaves and grass near you.");
    changed |= ImGui::Checkbox("GTVBAO only", &options.showAmbientOcclusion);
    tooltip("With GTVBAO on, the scene shows its occlusion alone: white where nothing occludes.");
    changed |= ImGui::Checkbox("SSILVB only", &options.showIndirectLight);
    tooltip("With SSILVB on, the scene shows the light it adds alone: black where nothing bounces.");
    changed |= ImGui::Checkbox("Global illumination only", &options.showGlobalIllumination);
    tooltip("With global illumination on, the scene shows its indirect light alone.");
    changed |= ImGui::Checkbox("Global illumination scene", &options.showTracedScene);
    tooltip("With global illumination on, the scene shows the geometry it traces, coloured by its normals: red where it\n"
            "disagrees with the game's depth, magenta where it lacks what the game drew, yellow where it has something in the sky.");
    if (const auto scene = global_illumination::status(); options.showTracedScene && !scene.empty()) ImGui::TextWrapped("%s", scene.c_str());
    if (changed) applyDiagnostics(options);

    ImGui::Spacing();
    ImGui::Text("Frames: %llu upscaled, %llu without; %llu history resets", static_cast<unsigned long long>(state.evaluations),
        static_cast<unsigned long long>(state.fallbacks), static_cast<unsigned long long>(state.resets));
    ImGui::Text("Render %ux%u, output %ux%u, texture LOD bias %+.2f", state.renderWidth, state.renderHeight, state.outputWidth, state.outputHeight,
        state.mipBias);
    if (!state.lastFallback.empty()) ImGui::TextWrapped("Last fallback: %s", state.lastFallback.c_str());
    ImGui::Text("Jitter %.3f, %.3f px", state.jitter[0], state.jitter[1]);
    if (frame_log::installed()) ImGui::TextUnformatted("F10 records the next frame into the PhantomFX-frame-log folder.");

    ImGui::SeparatorText("Costs");
    performance::drawDiagnostics();
}
}

void drawMenu()
{
    auto wanted = currentSettings();
    const auto state = status();
    bool changed{};
    // Wide enough for every value, and growing with ReShade's font size.
    ImGui::PushItemWidth(ImGui::GetFontSize() * 20);
    if (ImGui::CollapsingHeader("Upscaling", ImGuiTreeNodeFlags_DefaultOpen)) changed |= drawUpscaling(state, wanted);
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Visuals", ImGuiTreeNodeFlags_DefaultOpen)) changed |= drawVisuals(wanted);
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Utility", ImGuiTreeNodeFlags_DefaultOpen)) changed |= drawUtility(wanted);
    if (changed) applySettings(wanted);

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Diagnostics")) drawDiagnostics(state);
    ImGui::PopItemWidth();
}
}
