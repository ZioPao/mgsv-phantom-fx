#include "motion_blur.hpp"

#include "device_check.hpp"

#include <Windows.h>
#include <MinHook.h>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace mgsv_dlss::motion_blur
{
namespace
{
// GrPluginMotionBlur's PreExec and Exec (slots 6 and 8 of its vtable at 0x20F8830), which the
// recording threads call every frame with (plugin, pass context, view). With the game's motion
// blur on, PreExec asks for the velocity passes by setting bit 0 of the frame flags at
// [context + 0xD0], which MakeVelocityBuffer tests, and Exec draws the blur.
constexpr std::uintptr_t PreExecRva = 0x1E2D80, ExecRva = 0x1E2E20;
// cmp byte ptr [rcx + 0x68], 0; mov rax, [r8 + 0x570]
constexpr unsigned char PreExecStart[]{0x80, 0x79, 0x68, 0x00, 0x49, 0x8B, 0x80, 0x70, 0x05, 0x00, 0x00};
// mov [rsp + 8], rbx; mov [rsp + 0x10], rsi; push rdi; sub rsp, 0x20
constexpr unsigned char ExecStart[]{0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20};
constexpr std::size_t FrameFlags = 0xD0;
constexpr std::uint64_t VelocityPasses = 1;

using PluginStep = void (*)(void* plugin, std::uint8_t* context, void* view);
PluginStep originalPreExec{}, originalExec{};
std::atomic<bool> velocityOnly{};

void preExec(void* plugin, std::uint8_t* context, void* view)
{
    originalPreExec(plugin, context, view);

    auto* flags = *reinterpret_cast<std::uint64_t**>(context + FrameFlags);
    if (velocityOnly.load(std::memory_order_relaxed) && flags) *flags |= VelocityPasses;
}

void exec(void* plugin, std::uint8_t* context, void* view)
{
    if (!velocityOnly.load(std::memory_order_relaxed)) originalExec(plugin, context, view);
}
}

std::string hook() noexcept
{
    if (!supportedBuild()) return "Unknown game build; motion blur is left to the game.";

    auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    auto* preExecTarget = base + PreExecRva;
    auto* execTarget = base + ExecRva;
    if (std::memcmp(preExecTarget, PreExecStart, sizeof(PreExecStart)) || std::memcmp(execTarget, ExecStart, sizeof(ExecStart)))
        return "The motion blur plugin has unexpected code; motion blur is left to the game.";

    auto status = MH_CreateHook(preExecTarget, reinterpret_cast<void*>(&preExec), reinterpret_cast<void**>(&originalPreExec));
    if (status == MH_OK) status = MH_CreateHook(execTarget, reinterpret_cast<void*>(&exec), reinterpret_cast<void**>(&originalExec));
    if (status == MH_OK) status = MH_EnableHook(preExecTarget);
    if (status == MH_OK) status = MH_EnableHook(execTarget);
    if (status != MH_OK)
    {
        MH_RemoveHook(preExecTarget);
        MH_RemoveHook(execTarget);
        return std::string("Hooking the motion blur plugin failed (") + MH_StatusToString(status) + "); motion blur is left to the game.";
    }

    return "The motion blur plugin is hooked: with DLSS on, the engine draws velocity and no blur.";
}

void setVelocityOnly(bool on) noexcept
{
    velocityOnly = on;
}
}
