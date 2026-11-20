#include "devaddon/game_loop.hpp"

#include "devkit/commands.hpp"
#include "devkit/cutscenes.hpp"
#include "devkit/game.hpp"
#include "devkit/game_lua.hpp"
#include "devkit/travel.hpp"
#include "game_code.hpp"

#include <Windows.h>
#include <MinHook.h>
#include <cstdint>
#include <cstring>

namespace devaddon
{
namespace
{
// The main loop's frame start, on the loop object (rcx). PhantomFX's game_loop.hpp has the loop's end too.
constexpr std::uintptr_t FrameStartRva = 0x7F840;
// push rbx; push rsi; push rdi; sub rsp, 0x60; mov qword ptr [rsp + 0x20], -2
constexpr unsigned char FrameStartCode[]{0x40, 0x53, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x60, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF};

using FrameStart = void (*)(std::uint8_t* loop, void* argument);
FrameStart original{};

void frameStart(std::uint8_t* loop, void* argument)
{
    devkit::travel::onFrameStart();
    devkit::cutscenes::onFrameStart();
    devkit::game_lua::onFrameStart();
    devkit::commands::onFrameStart();
    original(loop, argument);
}
}

std::string hook() noexcept
{
    if (!devkit::game::isSupportedBuild()) return "Unknown game build; the game loop is not hooked.";

    auto* base = phantom_fx::game_code::image();
    if (std::memcmp(base + FrameStartRva, FrameStartCode, sizeof(FrameStartCode)))
        return "The game loop has unexpected code; the tools that run at the frame start are unavailable.";

    const auto status = phantom_fx::game_code::hookAt(base + FrameStartRva, &frameStart, original);
    if (status != MH_OK) return std::string("Hooking the game loop failed (") + MH_StatusToString(status) + ").";

    return "The game loop is hooked.";
}

void unhook() noexcept
{
    if (!original) return;

    MH_RemoveHook(phantom_fx::game_code::image() + FrameStartRva);
    original = nullptr;
}
}
