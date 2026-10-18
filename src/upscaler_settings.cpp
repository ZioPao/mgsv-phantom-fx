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

namespace mgsv_dlss::upscaler
{
namespace
{
constexpr std::string_view ModeKeys[]{"off", "dlaa", "quality", "balanced", "performance", "ultra_performance", "custom"};
static_assert(std::size(ModeKeys) == static_cast<std::size_t>(Mode::Count));

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

std::string_view label(Mode mode)
{
    switch (mode)
    {
    case Mode::Off: return "Off (game FXAA)";
    case Mode::Dlaa: return "DLAA";
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

bool validPreset(int value)
{
    return value == 0 || (value >= static_cast<int>(Preset::J) && value <= static_cast<int>(Preset::M));
}

bool upscales(Mode mode)
{
    return mode >= Mode::Quality && mode <= Mode::Custom;
}

float nominalScale(Mode mode, float customScale)
{
    switch (mode)
    {
    case Mode::Quality: return 2.0f / 3.0f;
    case Mode::Balanced: return 0.58f;
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
        if (key == "mode")
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
    }

    if (input.bad()) throw std::runtime_error("Cannot read the DLSS settings.");

    return settings;
}

void saveSettings(const std::filesystem::path& path, const Settings& settings)
{
    const auto within = [](float value, float low, float high) { return std::isfinite(value) && value >= low && value <= high; };
    if (settings.mode >= Mode::Count || !validPreset(static_cast<int>(settings.preset)) || !within(settings.sharpness, 0, Settings::MaxSharpness)
        || !within(settings.customScale, Settings::MinScale, Settings::MaxScale)
        || !within(settings.mipBiasOffset, Settings::MinMipBias, Settings::MaxMipBias))
        throw std::runtime_error("Cannot save invalid DLSS settings.");

    auto temporary = path;
    temporary += L".tmp";
    try
    {
        std::ofstream output(temporary, std::ios::trunc);
        output.imbue(std::locale::classic());
        output << "mode=" << ModeKeys[static_cast<std::size_t>(settings.mode)] << "\npreset=" << static_cast<int>(settings.preset)
               << "\nsharpness=" << settings.sharpness << "\ncustom_scale=" << settings.customScale << "\nmip_bias_offset=" << settings.mipBiasOffset
               << "\nunlock_fps=" << settings.unlockFps << '\n';
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
