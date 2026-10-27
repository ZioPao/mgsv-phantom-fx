#include "features/motion_blur/motion_blur.hpp"

#include "device_check.hpp"
#include "game_code.hpp"
#include "game_memory.hpp"

#include <Windows.h>
#include <MinHook.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace phantom_fx::motion_blur
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
// Motion blur on and its amount, as PreExec and Exec read them (docs/DEVELOPMENT.md, "Engine addresses").
constexpr std::uintptr_t MotionBlurOnRva = 0x2B75AC4, AmountRva = 0x2B75ACC;

using PluginStep = void (*)(void* plugin, std::uint8_t* context, void* view);
PluginStep originalPreExec{}, originalExec{};
std::atomic<bool> velocityOnly{}, hooked{}, enabled{true};
std::atomic<float> strength{1};
// The game's own amount, relearned when it writes it, so the strength multiplies the game's radius.
std::atomic<float> gameAmount{1};
std::atomic<float> writtenAmount{std::numeric_limits<float>::quiet_NaN()};

// The player's motion blur setting, as PreExec reads it.
bool gameWantsBlur() noexcept
{
    const auto on = game_memory::read<std::uint32_t>(reinterpret_cast<std::uintptr_t>(game_code::image()) + MotionBlurOnRva);

    return on && *on != 0;
}

// The add-on's strength in the game's amount, written after PreExec has read it, so this frame's
// blur draws use it. The toggle is the add-on's own, in exec().
void applyStrength() noexcept
{
    if (!hooked.load(std::memory_order_relaxed) || !enabled.load(std::memory_order_relaxed)) return;
    const auto base = reinterpret_cast<std::uintptr_t>(game_code::image());
    const auto amount = game_memory::read<float>(base + AmountRva);
    if (!amount) return;

    const auto written = writtenAmount.load(std::memory_order_relaxed);
    if (!(std::fabs(*amount - written) <= 1e-4f)) gameAmount = *amount;
    const float wanted = std::clamp(gameAmount.load(std::memory_order_relaxed) * strength.load(std::memory_order_relaxed), 0.0f, 1.0f);
    if (!(std::fabs(wanted - *amount) <= 1e-4f)) game_memory::write(base + AmountRva, wanted);
    writtenAmount = wanted;
}

void preExec(void* plugin, std::uint8_t* context, void* view)
{
    originalPreExec(plugin, context, view);

    auto* flags = *reinterpret_cast<std::uint64_t**>(context + FrameFlags);
    if (velocityOnly.load(std::memory_order_relaxed) && flags) *flags |= VelocityPasses;

    applyStrength();
}

void exec(void* plugin, std::uint8_t* context, void* view)
{
    // The bit may be the add-on's, forced on for the upscaler's motion vectors; the blur still follows
    // the add-on's toggle and the player's setting.
    if (enabled.load(std::memory_order_relaxed) && (!velocityOnly.load(std::memory_order_relaxed) || gameWantsBlur()))
        originalExec(plugin, context, view);
}
}

std::string hook() noexcept
{
    if (!isSupportedGameBuild()) return "Unknown game build; motion blur is left to the game.";

    auto* base = game_code::image();
    auto* preExecTarget = base + PreExecRva;
    auto* execTarget = base + ExecRva;
    if (std::memcmp(preExecTarget, PreExecStart, sizeof(PreExecStart)) || std::memcmp(execTarget, ExecStart, sizeof(ExecStart)))
        return "The motion blur plugin has unexpected code; motion blur is left to the game.";

    auto status = game_code::hookAt(preExecTarget, &preExec, originalPreExec);
    if (status == MH_OK)
    {
        status = game_code::hookAt(execTarget, &exec, originalExec);
        if (status != MH_OK) MH_RemoveHook(preExecTarget);
    }

    if (status != MH_OK)
        return std::string("Hooking the motion blur plugin failed (") + MH_StatusToString(status) + "); motion blur is left to the game.";

    return "The motion blur plugin is hooked: with DLSS on, the engine draws its velocity passes every frame, and its blur as the game's setting says.";
}

void setVelocityOnly(bool on) noexcept
{
    velocityOnly = on;
}

void configure(const Configuration& configuration) noexcept
{
    enabled = configuration.settings.motionBlur.on;
    strength = std::clamp(configuration.settings.motionBlur.strength, 0.0f, 2.0f);
}
}
