#pragma once

#include "effects.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

// Each feature describes its settings once (describe() in its file). Loading, saving, checking and the
// menu each walk those descriptions as a SettingVisitor.
namespace phantom_fx
{
// The menu's sections, in the rail's order. Only the dev build has Developer (dev_tools.hpp).
enum class Section : std::uint8_t { Budget, Upscaling, Shadows, Lighting, Atmosphere, Camera, Display, World, Diagnostics, Developer };

struct FeatureInfo
{
    const char* menuId; // Its item's and its budget line's
    std::string_view iniKey; // Its switch's
    Section section;
    const char* heading{}; // Above its item, for the items that follow
    const char* name;
    const char* tech{};
    const char* tag{};
    const char* tip{};
    const char* summary{};
    const char* keywords{}; // More words the menu's search finds it by
    std::optional<Effect> effect;
    std::optional<Effect> replacedBy;
    std::optional<Effect> needs; // The effect it builds on: it runs only while that one runs
    bool needsSupportedBuild{};
};

struct ToggleInfo
{
    std::string_view iniKey;
    const char* label;
    const char* tip{};
    bool visible{true};
    bool enabled{true};
};

struct SliderInfo
{
    std::string_view iniKey;
    const char* label;
    float min{}, max{1};
    float standard{}; // What the reset button gives back
    const char* format{"%.2f"};
    float readoutScale{1}; // 100 shows a share as a percentage
    bool logarithmic{};
    bool onRelease{}; // For values slow to apply
    const char* tip{};
    const char* hint{};
    // Set, 0 leaves the value to the game or the display; the menu then shows this hint.
    const char* automaticHint{};
    bool visible{true};
    bool enabled{true};
    std::span<const Effect> shownIn; // Other features' items it shows in, when its own feature has none
};

struct SettingChoice
{
    int value;
    const char* label;
    std::string_view iniKey{}; // Without one, PhantomFX.ini holds the value
};

enum class ChoiceControl : std::uint8_t { Buttons, Dropdown };

struct ChoiceInfo
{
    std::string_view iniKey;
    const char* label;
    std::span<const SettingChoice> choices;
    ChoiceControl control{ChoiceControl::Buttons};
    const char* tip{};
    bool visible{true};
    bool enabled{true};
};

class SettingVisitor
{
public:
    // A feature comes first in its description, with its switch when it has one.
    virtual void feature(bool&, const FeatureInfo&) {}
    virtual void feature(const FeatureInfo&) {}
    virtual void toggle(bool&, const ToggleInfo&) {}
    virtual void slider(float&, const SliderInfo&) {}
    virtual void choice(int&, const ChoiceInfo&) {}

    // An enum's choices hold its values.
    template<class E> requires std::is_enum_v<E>
    void choice(E& value, const ChoiceInfo& info)
    {
        int number = static_cast<int>(value);
        choice(number, info);
        value = static_cast<E>(number);
    }

protected:
    ~SettingVisitor() = default;
};
}
