#include "fxaa.hpp"

#include "device_check.hpp"
#include "game_code.hpp"

#include <Windows.h>
#include <MinHook.h>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace phantom_fx::fxaa
{
namespace
{
// GrPluginFxaa's Exec (slot 8 of its vtable at 0x2103220), which the recording threads call every
// frame with (plugin, pass context, view). It runs the FXAA filter at [plugin + 0x68] when the
// view allows post effects (bit 7 of [[view + 0x570] + 0x100]) and the dword at 0x2B79984, which
// the post processing setting clears, is set.
constexpr std::uintptr_t ExecRva = 0x2155A0;
// mov rax, [r8 + 0x570]; mov edx, [rax + 0x100]
constexpr unsigned char ExecStart[]{0x49, 0x8B, 0x80, 0x70, 0x05, 0x00, 0x00, 0x8B, 0x90, 0x00, 0x01, 0x00, 0x00};
constexpr std::size_t ViewSettings = 0x570, ViewFlags = 0x100, Filter = 0x68;
constexpr std::uint32_t PostEffects = 1u << 7;

using PluginStep = void (*)(std::uint8_t* plugin, void* context, std::uint8_t* view);
// The filter's slot 4. The game's tail call leaves the view in both rdx and r8, so this passes it
// twice to match.
using FilterExec = void (*)(void* filter, std::uint8_t* view, std::uint8_t* sameView);
PluginStep originalExec{};
std::atomic<bool> forced{};

void exec(std::uint8_t* plugin, void* context, std::uint8_t* view)
{
    if (!forced.load(std::memory_order_relaxed)) return originalExec(plugin, context, view);

    const auto* settings = *reinterpret_cast<std::uint8_t**>(view + ViewSettings);
    if (!(*reinterpret_cast<const std::uint32_t*>(settings + ViewFlags) & PostEffects)) return;

    auto* filter = *reinterpret_cast<void**>(plugin + Filter);
    auto* filterExec = (*reinterpret_cast<FilterExec**>(filter))[4];
    filterExec(filter, view, view);
}
}

std::string hook() noexcept
{
    if (!supportedBuild()) return "Unknown game build; FXAA is left to the game.";

    auto* target = game_code::image() + ExecRva;
    if (std::memcmp(target, ExecStart, sizeof(ExecStart))) return "The FXAA plugin has unexpected code; FXAA is left to the game.";

    if (const auto status = game_code::hookAt(target, &exec, originalExec); status != MH_OK)
        return std::string("Hooking the FXAA plugin failed (") + MH_StatusToString(status) + "); FXAA is left to the game.";

    return "The FXAA plugin is hooked: with DLSS on, the engine draws its FXAA pass whatever the post processing setting.";
}

void setForced(bool on) noexcept
{
    forced = on;
}
}
