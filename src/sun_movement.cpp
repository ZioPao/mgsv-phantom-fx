#include "sun_movement.hpp"

#include "device_check.hpp"

#include <Windows.h>
#include <cstdint>
#include <cstring>

namespace phantom_fx::sun_movement
{
namespace
{
// The sun and moon light's update (0x79D330) holds the light still until the sky's sun is
// (90 - limit angle) / steps degrees away from it, then blends to it over a time; both come from
// the weather's parameters, and the blend time is 0 in the ones seen. The light's stepped flag
// (+0xCC bit 3) decides, tested here; without it the update takes the sky's direction every frame.
constexpr std::uintptr_t StepTestRva = 0x79D51D;
// mov eax, [rdi + 0xCC]; mov sil, 1; shr eax, 3; test sil, al; je +0x228
constexpr unsigned char StepTestCode[]{0x8B, 0x87, 0xCC, 0x00, 0x00, 0x00, 0x40, 0xB6, 0x01, 0xC1, 0xE8, 0x03, 0x40, 0x84, 0xC6, 0x0F,
    0x84, 0x28, 0x02, 0x00, 0x00};
constexpr std::size_t JumpOffset = 15;
// nop; jmp +0x228: the same target, always taken.
constexpr unsigned char NeverStepped[]{0x90, 0xE9};
}

std::string makeSmooth() noexcept
{
    if (!supportedBuild()) return "Unknown game build; the sun moves in the game's steps.";

    auto* const test = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr)) + StepTestRva;
    if (std::memcmp(test, StepTestCode, sizeof(StepTestCode))) return "The sun's update has unexpected code; the sun moves in the game's steps.";

    auto* const jump = test + JumpOffset;
    DWORD protection{};
    if (!VirtualProtect(jump, sizeof(NeverStepped), PAGE_EXECUTE_READWRITE, &protection))
        return "Could not unprotect the sun's update; the sun moves in the game's steps.";

    std::memcpy(jump, NeverStepped, sizeof(NeverStepped));
    VirtualProtect(jump, sizeof(NeverStepped), protection, &protection);
    FlushInstructionCache(GetCurrentProcess(), jump, sizeof(NeverStepped));

    return "The sun and moon move smoothly, not in the game's steps.";
}
}
