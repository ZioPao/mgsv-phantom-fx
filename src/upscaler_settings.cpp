#include "upscaler_settings.hpp"

#include <Windows.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <locale>
#include <stdexcept>
#include <string>
#include <system_error>

namespace phantom_fx::upscaler
{
namespace
{
constexpr std::string_view UpscalerKeys[]{"dlss", "fsr"};
static_assert(std::size(UpscalerKeys) == static_cast<std::size_t>(Upscaler::Count));
constexpr std::string_view ModeKeys[]{"off", "dlaa", "quality", "balanced", "performance", "ultra_performance", "custom"};
static_assert(std::size(ModeKeys) == static_cast<std::size_t>(Mode::Count));
constexpr std::string_view FrameGenerationKeys[]{"off", "2x", "3x", "4x", "5x", "6x", "dynamic", "fsr"};
constexpr std::string_view AtmosphereLookKeys[]{"mgsv", "physical"};
static_assert(std::size(AtmosphereLookKeys) == static_cast<std::size_t>(AtmosphereLook::Count));
static_assert(std::size(FrameGenerationKeys) == static_cast<std::size_t>(FrameGeneration::Count));

std::string_view trim(std::string_view text)
{
    const auto begin = text.find_first_not_of(" \t\r");
    return begin == text.npos ? std::string_view{} : text.substr(begin, text.find_last_not_of(" \t\r") - begin + 1);
}

template<class T> bool number(std::string_view text, T& result)
{
    T value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return false;

    result = value;

    return true;
}

// Each leaves the setting as it is when the text is not valid.
void readFlag(std::string_view value, bool& result)
{
    if (value == "0" || value == "1") result = value == "1";
}

void readFloat(std::string_view value, float& result, float low, float high)
{
    float parsed{};
    if (number(value, parsed) && std::isfinite(parsed)) result = std::clamp(parsed, low, high);
}

template<class T, std::size_t N> void readKey(std::string_view value, const std::string_view (&keys)[N], T& result)
{
    const auto found = std::find(std::begin(keys), std::end(keys), value);
    if (found != std::end(keys)) result = static_cast<T>(found - std::begin(keys));
}
}

std::string_view label(Upscaler upscaler)
{
    switch (upscaler)
    {
    case Upscaler::Dlss: return "DLSS";
    case Upscaler::Fsr: return "FSR";
    default: return "Unknown";
    }
}

std::string_view label(Mode mode, Upscaler upscaler)
{
    switch (mode)
    {
    case Mode::Off: return "Off (game FXAA)";
    case Mode::Dlaa: return upscaler == Upscaler::Fsr ? "Native AA" : "DLAA";
    case Mode::Quality: return "Quality";
    case Mode::Balanced: return "Balanced";
    case Mode::Performance: return "Performance";
    case Mode::UltraPerformance: return "Ultra Performance";
    case Mode::Custom: return "Custom";
    default: return "Unknown";
    }
}

std::string_view label(Preset preset)
{
    switch (preset)
    {
    case Preset::Default: return "Default";
    case Preset::J: return "J (transformer, less ghosting)";
    case Preset::K: return "K (transformer)";
    case Preset::L: return "L (Ultra Performance model)";
    case Preset::M: return "M (Performance model)";
    default: return "Unknown";
    }
}

std::string_view label(AtmosphereLook look)
{
    switch (look)
    {
    case AtmosphereLook::Mgsv: return "MGSV";
    case AtmosphereLook::Physical: return "Physical";
    default: return "Unknown";
    }
}

std::string_view label(FrameGeneration mode)
{
    switch (mode)
    {
    case FrameGeneration::Off: return "Off";
    case FrameGeneration::X2: return "DLSS 2x";
    case FrameGeneration::X3: return "DLSS 3x";
    case FrameGeneration::X4: return "DLSS 4x";
    case FrameGeneration::X5: return "DLSS 5x";
    case FrameGeneration::X6: return "DLSS 6x";
    case FrameGeneration::Dynamic: return "DLSS Dynamic (display refresh rate)";
    case FrameGeneration::Fsr: return "FSR 2x";
    default: return "Unknown";
    }
}

unsigned multiplier(FrameGeneration mode)
{
    return mode >= FrameGeneration::X2 && mode <= FrameGeneration::X6 ? static_cast<unsigned>(mode) + 1 : 0;
}

bool validPreset(int value)
{
    return value == 0 || (value >= static_cast<int>(Preset::J) && value <= static_cast<int>(Preset::M));
}

bool validRayQuality(int value)
{
    return value == 1 || value == 2 || value == 4 || value == 8;
}

bool validGlobalIlluminationRays(int value)
{
    return value == 1 || value == 2 || value == 4;
}

bool upscales(Mode mode)
{
    return mode >= Mode::Quality && mode <= Mode::Custom;
}

float nominalScale(Upscaler upscaler, Mode mode, float customScale)
{
    switch (mode)
    {
    case Mode::Quality: return 2.0f / 3.0f;
    // AMD's Balanced divides by 1.7.
    case Mode::Balanced: return upscaler == Upscaler::Fsr ? 1.0f / 1.7f : 0.58f;
    case Mode::Performance: return 0.5f;
    case Mode::UltraPerformance: return 1.0f / 3.0f;
    case Mode::Custom:
        return std::isfinite(customScale) ? std::clamp(customScale, Settings::MinScale, Settings::MaxScale) : Settings::DefaultCustomScale;
    default: return 1.0f;
    }
}

float mipBias(float scale, float offset)
{
    if (!std::isfinite(scale) || scale <= 0) scale = 1;
    if (!std::isfinite(offset)) offset = 0;

    return std::log2(std::min(scale, 1.0f)) + std::clamp(offset, Settings::MinMipBias, Settings::MaxMipBias);
}

void applyPreset(Settings& settings, EffectPreset preset)
{
    const bool screenSpace = preset == EffectPreset::ScreenSpace;
    settings.softShadows = screenSpace;
    settings.sunScreenSpaceShadows = screenSpace;
    settings.lightScreenSpaceShadows = screenSpace;
    settings.ambientOcclusion = screenSpace;
    settings.indirectLight = screenSpace;

    settings.rayTracedShadows = !screenSpace;
    settings.rayTracedLights = !screenSpace;
    settings.globalIllumination = !screenSpace;

    settings.unlockFps = true;
    settings.volumetricFog = true;
    settings.atmosphereLook = AtmosphereLook::Physical;
}

std::optional<EffectPreset> presetOf(const Settings& settings)
{
    for (const auto preset : {EffectPreset::ScreenSpace, EffectPreset::RayTraced})
    {
        auto switched = settings;
        applyPreset(switched, preset);
        if (switched == settings) return preset;
    }

    return std::nullopt;
}

Settings firstLaunchSettings()
{
    Settings settings{.mode = Mode::Quality};
    applyPreset(settings, EffectPreset::ScreenSpace);

    return settings;
}

Settings loadSettings(const std::filesystem::path& path)
{
    Settings settings;
    std::ifstream input(path);
    if (!input)
    {
        if (!std::filesystem::exists(path)) return firstLaunchSettings();

        throw std::runtime_error("Cannot read the DLSS settings.");
    }

    std::string line;
    while (std::getline(input, line))
    {
        const auto separator = line.find('=');
        if (separator == line.npos) continue;

        const auto key = trim(std::string_view(line).substr(0, separator));
        const auto value = trim(std::string_view(line).substr(separator + 1));
        if (key == "upscaler") readKey(value, UpscalerKeys, settings.upscaler);
        else if (key == "mode") readKey(value, ModeKeys, settings.mode);
        else if (key == "preset")
        {
            int preset{};
            if (number(value, preset) && validPreset(preset)) settings.preset = static_cast<Preset>(preset);
        }
        else if (key == "sharpness") readFloat(value, settings.sharpness, 0.0f, Settings::MaxSharpness);
        else if (key == "custom_scale") readFloat(value, settings.customScale, Settings::MinScale, Settings::MaxScale);
        else if (key == "mip_bias_offset") readFloat(value, settings.mipBiasOffset, Settings::MinMipBias, Settings::MaxMipBias);
        else if (key == "unlock_fps") readFlag(value, settings.unlockFps);
        else if (key == "soft_shadows") readFlag(value, settings.softShadows);
        else if (key == "sun_size") readFloat(value, settings.sunSize, Settings::MinSunSize, Settings::MaxSunSize);
        else if (key == "sun_screen_space_shadows") readFlag(value, settings.sunScreenSpaceShadows);
        else if (key == "light_screen_space_shadows") readFlag(value, settings.lightScreenSpaceShadows);
        else if (key == "soft_local_shadows") readFlag(value, settings.softLocalShadows);
        else if (key == "contact_shadow_contrast")
            readFloat(value, settings.contactShadowContrast, Settings::MinContactShadowContrast, Settings::MaxContactShadowContrast);
        else if (key == "contact_shadow_soft") readFlag(value, settings.contactShadowSoft);
        else if (key == "contact_shadow_gain")
            readFloat(value, settings.contactShadowGain, Settings::MinContactShadowGain, Settings::MaxContactShadowGain);
        else if (key == "contact_shadow_reach")
            readFloat(value, settings.contactShadowReach, Settings::MinContactShadowReach, Settings::MaxContactShadowReach);
        else if (key == "ray_traced_shadows") readFlag(value, settings.rayTracedShadows);
        else if (key == "vanilla_shadows_with_ray_tracing") readFlag(value, settings.vanillaShadowsWithRayTracing);
        else if (key == "ray_traced_lights") readFlag(value, settings.rayTracedLights);
        else if (key == "ray_quality")
        {
            int rays{};
            if (number(value, rays) && validRayQuality(rays)) settings.rayQuality = rays;
        }
        else if (key == "flashlight_brightness") readFloat(value, settings.flashlightBrightness, Settings::MinFlashlightBrightness, Settings::MaxFlashlightBrightness);
        else if (key == "frame_generation") readKey(value, FrameGenerationKeys, settings.frameGeneration);
        else if (key == "ambient_occlusion") readFlag(value, settings.ambientOcclusion);
        else if (key == "occlusion_radius") readFloat(value, settings.occlusionRadius, Settings::MinOcclusionRadius, Settings::MaxOcclusionRadius);
        else if (key == "occlusion_strength") readFloat(value, settings.occlusionStrength, 0.0f, Settings::MaxOcclusionStrength);
        else if (key == "volumetric_fog") readFlag(value, settings.volumetricFog);
        else if (key == "atmosphere_look") readKey(value, AtmosphereLookKeys, settings.atmosphereLook);
        else if (key == "atmosphere_haze") readFloat(value, settings.atmosphereHaze, 0.0f, 1.0f);
        else if (key == "indirect_light") readFlag(value, settings.indirectLight);
        else if (key == "indirect_light_radius") readFloat(value, settings.indirectLightRadius, Settings::MinIndirectLightRadius, Settings::MaxIndirectLightRadius);
        else if (key == "indirect_light_strength") readFloat(value, settings.indirectLightStrength, 0.0f, Settings::MaxIndirectLightStrength);
        else if (key == "global_illumination") readFlag(value, settings.globalIllumination);
        else if (key == "global_illumination_strength") readFloat(value, settings.globalIlluminationStrength, 0.0f, Settings::MaxGlobalIlluminationStrength);
        else if (key == "global_illumination_rays")
        {
            int rays{};
            if (number(value, rays) && validGlobalIlluminationRays(rays)) settings.globalIlluminationRays = rays;
        }
        else if (key == "f_number") readFloat(value, settings.fNumber, Settings::MinFNumber, Settings::MaxFNumber);
        else if (key == "hdr") readFlag(value, settings.hdr);
        else if (key == "hdr_peak_nits")
        {
            float nits{};
            if (number(value, nits) && std::isfinite(nits))
                settings.peakNits = nits == 0 ? 0 : std::clamp(nits, Settings::MinPeakNits, Settings::MaxPeakNits);
        }
        else if (key == "hdr_paper_white_nits") readFloat(value, settings.paperWhiteNits, Settings::MinPaperWhiteNits, Settings::MaxPaperWhiteNits);
    }

    if (input.bad()) throw std::runtime_error("Cannot read the DLSS settings.");

    return settings;
}

void saveSettings(const std::filesystem::path& path, const Settings& settings)
{
    const auto within = [](float value, float low, float high) { return std::isfinite(value) && value >= low && value <= high; };
    if (settings.upscaler >= Upscaler::Count || settings.mode >= Mode::Count || !validPreset(static_cast<int>(settings.preset)) || !within(settings.sharpness, 0, Settings::MaxSharpness)
        || !within(settings.customScale, Settings::MinScale, Settings::MaxScale)
        || !within(settings.mipBiasOffset, Settings::MinMipBias, Settings::MaxMipBias) || settings.frameGeneration >= FrameGeneration::Count
        || !within(settings.sunSize, Settings::MinSunSize, Settings::MaxSunSize)
        || !within(settings.occlusionRadius, Settings::MinOcclusionRadius, Settings::MaxOcclusionRadius)
        || !within(settings.occlusionStrength, 0, Settings::MaxOcclusionStrength)
        || !within(settings.indirectLightRadius, Settings::MinIndirectLightRadius, Settings::MaxIndirectLightRadius)
        || !within(settings.indirectLightStrength, 0, Settings::MaxIndirectLightStrength) || !within(settings.atmosphereHaze, 0, 1)
        || !within(settings.globalIlluminationStrength, 0, Settings::MaxGlobalIlluminationStrength)
        || !validRayQuality(settings.rayQuality)
        || !validGlobalIlluminationRays(settings.globalIlluminationRays)
        || !within(settings.flashlightBrightness, Settings::MinFlashlightBrightness, Settings::MaxFlashlightBrightness)
        || !within(settings.fNumber, Settings::MinFNumber, Settings::MaxFNumber)
        || (settings.peakNits != 0 && !within(settings.peakNits, Settings::MinPeakNits, Settings::MaxPeakNits))
        || !within(settings.paperWhiteNits, Settings::MinPaperWhiteNits, Settings::MaxPaperWhiteNits))
        throw std::runtime_error("Cannot save invalid DLSS settings.");

    auto temporary = path;
    temporary += L".tmp";
    try
    {
        std::ofstream output(temporary, std::ios::trunc);
        output.imbue(std::locale::classic());
        output << "upscaler=" << UpscalerKeys[static_cast<std::size_t>(settings.upscaler)]
               << "\nmode=" << ModeKeys[static_cast<std::size_t>(settings.mode)] << "\npreset=" << static_cast<int>(settings.preset)
               << "\nsharpness=" << settings.sharpness << "\ncustom_scale=" << settings.customScale << "\nmip_bias_offset=" << settings.mipBiasOffset
               << "\nunlock_fps=" << settings.unlockFps << "\nframe_generation=" << FrameGenerationKeys[static_cast<std::size_t>(settings.frameGeneration)]
               << "\nsoft_shadows=" << settings.softShadows << "\nsun_size=" << settings.sunSize
               << "\nsun_screen_space_shadows=" << settings.sunScreenSpaceShadows
               << "\nlight_screen_space_shadows=" << settings.lightScreenSpaceShadows
                << "\ncontact_shadow_contrast=" << settings.contactShadowContrast
                << "\ncontact_shadow_reach=" << settings.contactShadowReach
                << "\ncontact_shadow_soft=" << settings.contactShadowSoft
                << "\ncontact_shadow_gain=" << settings.contactShadowGain
                << "\nsoft_local_shadows=" << settings.softLocalShadows << "\nray_traced_shadows=" << settings.rayTracedShadows
               << "\nvanilla_shadows_with_ray_tracing=" << settings.vanillaShadowsWithRayTracing
               << "\nray_traced_lights=" << settings.rayTracedLights << "\nray_quality=" << settings.rayQuality
               << "\nflashlight_brightness=" << settings.flashlightBrightness
               << "\nambient_occlusion=" << settings.ambientOcclusion
               << "\nocclusion_radius=" << settings.occlusionRadius << "\nocclusion_strength=" << settings.occlusionStrength
               << "\nindirect_light=" << settings.indirectLight << "\nindirect_light_radius=" << settings.indirectLightRadius
               << "\nindirect_light_strength=" << settings.indirectLightStrength << "\nglobal_illumination=" << settings.globalIllumination
               << "\nglobal_illumination_strength=" << settings.globalIlluminationStrength
               << "\nglobal_illumination_rays=" << settings.globalIlluminationRays << "\nvolumetric_fog=" << settings.volumetricFog
               << "\natmosphere_look=" << AtmosphereLookKeys[static_cast<std::size_t>(settings.atmosphereLook)]
               << "\natmosphere_haze=" << settings.atmosphereHaze
               << "\nf_number=" << settings.fNumber
               << "\nhdr=" << settings.hdr << "\nhdr_peak_nits=" << settings.peakNits
               << "\nhdr_paper_white_nits=" << settings.paperWhiteNits << '\n';
        output.close();
        if (!output) throw std::runtime_error("Cannot write the DLSS settings.");

        // Keep the last complete file if writing or replacing it fails.
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Cannot replace the DLSS settings");
    }
    catch (...)
    {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}
}
