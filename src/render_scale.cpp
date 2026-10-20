#include "render_scale.hpp"

#include "device_check.hpp"

#include <Windows.h>
#include <MinHook.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace phantom_fx::render_scale
{
namespace
{
// The renderer's per-frame step (slot 6 of its vtable at 0x21086E0) runs on the render thread with
// the renderer object. With the Rebuild flag set it compares the wanted render size with the current
// one and, when they differ, rebuilds every size-dependent target (0x24B2A0). FollowDisplay makes the
// step take the wanted size from the display first.
constexpr std::uintptr_t RendererStepRva = 0x24C220;
// push rbx; sub rsp, 0x20; mov rbx, rcx; mov ecx, [rcx + 0x198]
constexpr unsigned char RendererStepStart[]{0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x8B, 0x89, 0x98, 0x01, 0x00, 0x00};
constexpr std::size_t DisplaySize = 0x180, WantedSize = 0x188, CurrentSize = 0x190, Flags = 0x198;
constexpr std::uint32_t Rebuild = 2, FollowDisplay = 4;

using RendererStep = void (*)(std::uint8_t* renderer);
RendererStep originalRendererStep{};
bool installed{};

std::mutex requestMutex;
Size requestedRender, requestedDisplay;
std::atomic<std::uint64_t> currentSize{}; // width | height << 32
// Render thread only: what the engine chose itself, and what the step last wrote.
std::uint32_t engineWanted[2]{}, written[2]{};

std::uint64_t pack(Size size)
{
    return size.width | static_cast<std::uint64_t>(size.height) << 32;
}

Size unpack(std::uint64_t value)
{
    return {static_cast<unsigned>(value), static_cast<unsigned>(value >> 32)};
}

void rendererStep(std::uint8_t* renderer)
{
    const auto* display = reinterpret_cast<const std::uint32_t*>(renderer + DisplaySize);
    auto* wanted = reinterpret_cast<std::uint32_t*>(renderer + WantedSize);
    auto* flags = reinterpret_cast<std::uint32_t*>(renderer + Flags);

    // A wanted size this step did not write is the engine's own choice (start-up, a display change).
    if (wanted[0] != written[0] || wanted[1] != written[1])
    {
        engineWanted[0] = wanted[0];
        engineWanted[1] = wanted[1];
    }

    // A request planned for another display size waits for the add-on's next plan. With FollowDisplay
    // set the step takes the display's size itself; the next frame corrects it.
    Size render, planned;
    {
        std::lock_guard lock(requestMutex);
        render = requestedRender;
        planned = requestedDisplay;
    }

    const bool scaled = render.width && planned.width == display[0] && planned.height == display[1];
    const std::uint32_t size[2]{scaled ? render.width : engineWanted[0], scaled ? render.height : engineWanted[1]};
    if (!(*flags & FollowDisplay) && size[0] && size[1] && (wanted[0] != size[0] || wanted[1] != size[1]))
    {
        wanted[0] = size[0];
        wanted[1] = size[1];
        *flags |= Rebuild;
    }

    written[0] = wanted[0];
    written[1] = wanted[1];
    originalRendererStep(renderer);

    const auto* now = reinterpret_cast<const std::uint32_t*>(renderer + CurrentSize);
    currentSize = pack({now[0], now[1]});
}
}

std::string hook() noexcept
{
    if (!supportedBuild()) return "Unknown game build; the engine renders at the display size.";

    auto* target = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr)) + RendererStepRva;
    if (std::memcmp(target, RendererStepStart, sizeof(RendererStepStart))) return "The renderer has unexpected code; upscaling is unavailable.";

    auto status = MH_CreateHook(target, reinterpret_cast<void*>(&rendererStep), reinterpret_cast<void**>(&originalRendererStep));
    if (status == MH_OK) status = MH_EnableHook(target);
    if (status != MH_OK)
    {
        MH_RemoveHook(target);
        return std::string("Hooking the renderer failed (") + MH_StatusToString(status) + "); upscaling is unavailable.";
    }

    installed = true;

    return "The renderer is hooked: the engine can render below the display size.";
}

bool hooked() noexcept
{
    return installed;
}

void request(Size render, Size display) noexcept
{
    std::lock_guard lock(requestMutex);
    requestedRender = render;
    requestedDisplay = display;
}

Size current() noexcept
{
    return unpack(currentSize.load());
}
}
