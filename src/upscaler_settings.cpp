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

bool validApertureBlades(int value)
{
    return value == 0 || (value >= 5 && value <= 9);
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

Settings loadSettings(const std::filesystem::path& path)
{
    Settings settings;
    std::ifstream input(path);
    if (!input)
    {
        if (!std::filesystem::exists(path)) return settings;

        throw std::runtime_error("Cannot read the DLSS settings.");
    }

    std::string line;
    while (std::getline(input, line))
    {
        const auto separator = line.find('=');
        if (separator == line.npos) continue;

        const auto key = trim(std::string_view(line).substr(0, separator));
        const auto value = trim(std::string_view(line).substr(separator + 1));
        if (key == "upscaler")
        {
            const auto found = std::find(std::begin(UpscalerKeys), std::end(UpscalerKeys), value);
            if (found != std::end(UpscalerKeys)) settings.upscaler = static_cast<Upscaler>(found - std::begin(UpscalerKeys));
        }
        else if (key == "mode")
        {
            const auto found = std::find(std::begin(ModeKeys), std::end(ModeKeys), value);
            if (found != std::end(ModeKeys)) settings.mode = static_cast<Mode>(found - std::begin(ModeKeys));
        }
        else if (key == "preset")
        {
            int preset{};
            if (number(value, preset) && validPreset(preset)) settings.preset = static_cast<Preset>(preset);
        }
        else if (key == "sharpness")
        {
            float sharpness{};
            if (number(value, sharpness) && std::isfinite(sharpness)) settings.sharpness = std::clamp(sharpness, 0.0f, Settings::MaxSharpness);
        }
        else if (key == "custom_scale")
        {
            float scale{};
            if (number(value, scale) && std::isfinite(scale)) settings.customScale = std::clamp(scale, Settings::MinScale, Settings::MaxScale);
        }
        else if (key == "mip_bias_offset")
        {
            float offset{};
            if (number(value, offset) && std::isfinite(offset))
                settings.mipBiasOffset = std::clamp(offset, Settings::MinMipBias, Settings::MaxMipBias);
        }
        else if (key == "unlock_fps")
        {
            if (value == "0" || value == "1") settings.unlockFps = value == "1";
        }
        else if (key == "soft_shadows")
        {
            if (value == "0" || value == "1") settings.softShadows = value == "1";
        }
        else if (key == "sun_size")
        {
            float size{};
            if (number(value, size) && std::isfinite(size)) settings.sunSize = std::clamp(size, Settings::MinSunSize, Settings::MaxSunSize);
        }
        else if (key == "double_shadow_resolution")
        {
            if (value == "0" || value == "1") settings.doubleShadowResolution = value == "1";
        }
        else if (key == "sun_screen_space_shadows")
        {
            if (value == "0" || value == "1") settings.sunScreenSpaceShadows = value == "1";
        }
        else if (key == "light_screen_space_shadows")
        {
            if (value == "0" || value == "1") settings.lightScreenSpaceShadows = value == "1";
        }
        else if (key == "frame_generation")
        {
            const auto found = std::find(std::begin(FrameGenerationKeys), std::end(FrameGenerationKeys), value);
            if (found != std::end(FrameGenerationKeys)) settings.frameGeneration = static_cast<FrameGeneration>(found - std::begin(FrameGenerationKeys));
        }
        else if (key == "ambient_occlusion")
        {
            if (value == "0" || value == "1") settings.ambientOcclusion = value == "1";
        }
        else if (key == "occlusion_radius")
        {
            float radius{};
            if (number(value, radius) && std::isfinite(radius))
                settings.occlusionRadius = std::clamp(radius, Settings::MinOcclusionRadius, Settings::MaxOcclusionRadius);
        }
        else if (key == "occlusion_strength")
        {
            float strength{};
            if (number(value, strength) && std::isfinite(strength)) settings.occlusionStrength = std::clamp(strength, 0.0f, Settings::MaxOcclusionStrength);
        }
        else if (key == "volumetric_fog")
        {
            if (value == "0" || value == "1") settings.volumetricFog = value == "1";
        }
        else if (key == "atmosphere_look")
        {
            const auto found = std::find(std::begin(AtmosphereLookKeys), std::end(AtmosphereLookKeys), value);
            if (found != std::end(AtmosphereLookKeys)) settings.atmosphereLook = static_cast<AtmosphereLook>(found - std::begin(AtmosphereLookKeys));
        }
        else if (key == "atmosphere_haze")
        {
            float haze{};
            if (number(value, haze) && std::isfinite(haze)) settings.atmosphereHaze = std::clamp(haze, 0.0f, 1.0f);
        }
        else if (key == "indirect_light")
        {
            if (value == "0" || value == "1") settings.indirectLight = value == "1";
        }
        else if (key == "indirect_light_radius")
        {
            float radius{};
            if (number(value, radius) && std::isfinite(radius))
                settings.indirectLightRadius = std::clamp(radius, Settings::MinIndirectLightRadius, Settings::MaxIndirectLightRadius);
        }
        else if (key == "indirect_light_strength")
        {
            float strength{};
            if (number(value, strength) && std::isfinite(strength))
                settings.indirectLightStrength = std::clamp(strength, 0.0f, Settings::MaxIndirectLightStrength);
        }
        else if (key == "depth_of_field")
        {
            if (value == "0" || value == "1") settings.depthOfField = value == "1";
        }
        else if (key == "aperture_blades")
        {
            int blades{};
            if (number(value, blades) && validApertureBlades(blades)) settings.apertureBlades = blades;
        }
        else if (key == "f_number")
        {
            float fNumber{};
            if (number(value, fNumber) && std::isfinite(fNumber)) settings.fNumber = std::clamp(fNumber, Settings::MinFNumber, Settings::MaxFNumber);
        }
        else if (key == "lens_shot_frames")
        {
            int frames{};
            if (number(value, frames)) settings.lensShotFrames = std::clamp(frames, Settings::MinLensShotFrames, Settings::MaxLensShotFrames);
        }
        else if (key == "hdr")
        {
            if (value == "0" || value == "1") settings.hdr = value == "1";
        }
        else if (key == "hdr_peak_nits")
        {
            float nits{};
            if (number(value, nits) && std::isfinite(nits))
                settings.peakNits = nits == 0 ? 0 : std::clamp(nits, Settings::MinPeakNits, Settings::MaxPeakNits);
        }
        else if (key == "hdr_paper_white_nits")
        {
            float nits{};
            if (number(value, nits) && std::isfinite(nits))
                settings.paperWhiteNits = std::clamp(nits, Settings::MinPaperWhiteNits, Settings::MaxPaperWhiteNits);
        }
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
        || !validApertureBlades(settings.apertureBlades)
        || !within(settings.fNumber, Settings::MinFNumber, Settings::MaxFNumber) || settings.lensShotFrames < Settings::MinLensShotFrames
        || settings.lensShotFrames > Settings::MaxLensShotFrames
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
               << "\ndouble_shadow_resolution=" << settings.doubleShadowResolution
               << "\nsun_screen_space_shadows=" << settings.sunScreenSpaceShadows
               << "\nlight_screen_space_shadows=" << settings.lightScreenSpaceShadows << "\nambient_occlusion=" << settings.ambientOcclusion
               << "\nocclusion_radius=" << settings.occlusionRadius << "\nocclusion_strength=" << settings.occlusionStrength
               << "\nindirect_light=" << settings.indirectLight << "\nindirect_light_radius=" << settings.indirectLightRadius
               << "\nindirect_light_strength=" << settings.indirectLightStrength << "\nvolumetric_fog=" << settings.volumetricFog
               << "\natmosphere_look=" << AtmosphereLookKeys[static_cast<std::size_t>(settings.atmosphereLook)]
               << "\natmosphere_haze=" << settings.atmosphereHaze << "\ndepth_of_field=" << settings.depthOfField
               << "\naperture_blades=" << settings.apertureBlades << "\nf_number=" << settings.fNumber
               << "\nlens_shot_frames=" << settings.lensShotFrames << "\nhdr=" << settings.hdr << "\nhdr_peak_nits=" << settings.peakNits
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
