#include "shadow_resolution.hpp"

#include "device_check.hpp"
#include "log.hpp"

#include <Windows.h>
#include <MinHook.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace phantom_fx::shadow_resolution
{
namespace
{
// The shadow settings (vtable 0x20FA9A0) load a table per shadow quality from the game's Lua and
// keep the chosen quality's values. SetValues takes one quality's values as pending; the settings'
// event 4 then applies them.
constexpr std::uintptr_t SetValuesRva = 0x1F0DA0, HandleEventRva = 0x1EA1B0;
// mov [rsp + 8], rbx; push rdi; sub rsp, 0x20; mov eax, [rdx]; mov rdi, rdx; mov rbx, rcx
constexpr unsigned char SetValuesStart[]{0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0x02, 0x48, 0x8B, 0xFA, 0x48, 0x8B, 0xD9};
// push r15; sub rsp, 0x30; cmp byte ptr [rcx + 0xA0], 0; mov r15, rcx
constexpr unsigned char HandleEventStart[]{0x41, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x80, 0xB9, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x4C, 0x8B, 0xF9};

// One shadow quality's row of the table, in the order of its Lua fields.
struct Values
{
    std::int32_t directional; // DirectionalLightShadowResolution: the sun's atlas width and height
    std::int32_t spot;        // SpotLightLightShadowResolution
    std::int32_t point;       // PointLightLightShadowResolution
    float rangeScale;         // CascadeShadowRangeScale
    float cascadeBlend;       // EnableCascadeShadowBlend
};

using SetValues = std::uint32_t (*)(void* settings, const Values* values);
using HandleEvent = bool (*)(void* settings, int event, void* a, void* b);
SetValues originalSetValues{};
HandleEvent originalHandleEvent{};

std::mutex settingsMutex;
bool doubled{};

std::string describe(const Values& values)
{
    return std::to_string(values.directional) + " " + std::to_string(values.spot) + " " + std::to_string(values.point) + " "
        + std::to_string(values.rangeScale) + " " + std::to_string(values.cascadeBlend);
}

std::uint32_t setValues(void* settings, const Values* values)
{
    std::lock_guard lock(settingsMutex);
    Values chosen = *values;
    if (doubled) chosen.directional = std::min(chosen.directional * 2, 16384);

    const auto result = originalSetValues(settings, &chosen);
    log("Shadow settings " + hex(reinterpret_cast<std::uintptr_t>(settings)) + ": SetValues " + describe(*values) + " as " + describe(chosen)
        + " returned " + std::to_string(result) + ".");

    return result;
}

bool handleEvent(void* settings, int event, void* a, void* b)
{
    const auto result = originalHandleEvent(settings, event, a, b);
    const auto* applied = reinterpret_cast<const Values*>(static_cast<std::uint8_t*>(settings) + 0x74);
    log("Shadow settings " + hex(reinterpret_cast<std::uintptr_t>(settings)) + ": event " + std::to_string(event) + " returned "
        + std::to_string(result) + ", applied " + describe(*applied) + ".");

    return result;
}
}

std::string hook() noexcept
{
    if (!supportedBuild()) return "Unknown game build; the shadow resolution is left to the game.";

    auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    auto* setValuesTarget = base + SetValuesRva;
    auto* handleEventTarget = base + HandleEventRva;
    if (std::memcmp(setValuesTarget, SetValuesStart, sizeof(SetValuesStart)) || std::memcmp(handleEventTarget, HandleEventStart, sizeof(HandleEventStart)))
        return "The shadow settings have unexpected code; the shadow resolution is left to the game.";

    auto status = MH_CreateHook(setValuesTarget, reinterpret_cast<void*>(&setValues), reinterpret_cast<void**>(&originalSetValues));
    if (status == MH_OK) status = MH_CreateHook(handleEventTarget, reinterpret_cast<void*>(&handleEvent), reinterpret_cast<void**>(&originalHandleEvent));
    if (status == MH_OK) status = MH_EnableHook(setValuesTarget);
    if (status == MH_OK) status = MH_EnableHook(handleEventTarget);
    if (status != MH_OK)
    {
        MH_RemoveHook(setValuesTarget);
        MH_RemoveHook(handleEventTarget);
        originalSetValues = nullptr;
        return std::string("Hooking the shadow settings failed (") + MH_StatusToString(status) + "); the shadow resolution is left to the game.";
    }

    return "The shadow settings are hooked: the shadow resolution can be doubled.";
}

void setDoubled(bool on) noexcept
{
    std::lock_guard lock(settingsMutex);
    if (on != doubled) log(std::string("Shadow resolution: doubled ") + (on ? "on" : "off") + ".");
    doubled = on;
}
}
