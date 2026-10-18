#include "frame_rate.hpp"

#include "device_check.hpp"

#include <Windows.h>
#include <MinHook.h>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace mgsv_dlss::frame_rate
{
namespace
{
// SetGameFrameWaitType picks how the main loop waits for its next frame, by a 48-bit name hash, and
// keeps the last one at CurrentWaitTypeRva. Applying the graphics settings asks for CappedWait with
// framerate_control Auto or Limited30 (it sleeps until the next 60 or 30 fps tick) and VariableWait
// with Variable (it only measures the frame time). Lua scripts can call it too.
constexpr std::uintptr_t SetWaitTypeRva = 0x81090, CurrentWaitTypeRva = 0x29E3D68;
constexpr std::uint64_t CappedWait = 0xA5D6397A8E12, VariableWait = 0xA29EF1CEFC2;
// push rbx; sub rsp, 0x20; mov rax, 0xA5D6397A8E12
constexpr unsigned char SetWaitTypeStart[]{0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0xB8, 0x12, 0x8E, 0x7A, 0x39, 0xD6, 0xA5, 0x00, 0x00};

using SetWaitType = int (*)(std::uint64_t type);
SetWaitType originalSetWaitType{};
std::mutex waitMutex;
std::uint64_t requested{}; // The wait the game asked for last, given back when the unlock is turned off
bool unlocked{};

std::uint64_t chosen(std::uint64_t type)
{
    return unlocked && type == CappedWait ? VariableWait : type;
}

int setWaitType(std::uint64_t type)
{
    std::lock_guard lock(waitMutex);
    requested = type;
    return originalSetWaitType(chosen(type));
}
}

std::string hook() noexcept
{
    if (!supportedBuild()) return "Unknown game build; the frame rate is left to the game.";

    auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    auto* target = base + SetWaitTypeRva;
    if (std::memcmp(target, SetWaitTypeStart, sizeof(SetWaitTypeStart)))
        return "The frame wait setter has unexpected code; the frame rate is left to the game.";

    auto status = MH_CreateHook(target, reinterpret_cast<void*>(&setWaitType), reinterpret_cast<void**>(&originalSetWaitType));
    if (status == MH_OK) status = MH_EnableHook(target);
    if (status != MH_OK)
    {
        MH_RemoveHook(target);
        originalSetWaitType = nullptr;
        return std::string("Hooking the frame wait setter failed (") + MH_StatusToString(status) + "); the frame rate is left to the game.";
    }

    // The game may have chosen its wait before ReShade loaded the add-on.
    std::lock_guard lock(waitMutex);
    requested = *reinterpret_cast<const std::uint64_t*>(base + CurrentWaitTypeRva);

    return "The frame wait setter is hooked: the frame rate can be unlocked.";
}

void setUnlocked(bool on) noexcept
{
    if (!originalSetWaitType) return;

    std::lock_guard lock(waitMutex);
    if (on == unlocked) return;

    unlocked = on;
    // Switches at once. Before the game has asked for a wait, the hook applies the choice then.
    if (requested == CappedWait) originalSetWaitType(chosen(requested));
}
}
