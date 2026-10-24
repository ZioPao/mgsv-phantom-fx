#include "draw_distance.hpp"

#include "device_check.hpp"
#include "game_memory.hpp"
#include "log.hpp"

#include <Windows.h>
#include <MinHook.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>

namespace phantom_fx::draw_distance
{
namespace
{
// Creates a location's stream controller from a description that the StageBlockControllerData setup
// (0x165BD0, its only caller) fills from the location's stage file, on the stack.
constexpr std::uintptr_t CreateControllerRva = 0x15F010;
// push rbx; sub rsp, 0x30; mov qword ptr [rsp + 0x20], -2
constexpr unsigned char CreateControllerCode[]{0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF};

// A window in the description: its block size and block count, 32-bit, x then z. The coarser copies'
// block sizes count full-detail blocks.
struct Window
{
    std::size_t size, count;
    int openWorldSize;
};
constexpr Window FullDetail{0x64, 0xB0, 128}, Coarse{0x16C, 0x174, 2}, Coarsest{0x1E4, 0x1EC, 4};
constexpr int GameCount = 5;

// The level of detail's reference resolution, set by GrTools.SetLodFactorResolution (0x1BB2D0). At 0,
// each view uses its own width, so upscaling lowers the detail with the render size.
constexpr std::uintptr_t LodResolutionRva = 0x2B73434, LodResolutionSetterRva = 0x1BB2D0;
// mov dword ptr [rip + 0x29B815E], ecx; ret
constexpr unsigned char LodResolutionSetterCode[]{0x89, 0x0D, 0x5E, 0x81, 0x9B, 0x02, 0xC3};
constexpr int FarLodResolution = 8192;

// The graphics settings' RejectionLengthBias for models and for vegetation (clones), static objects:
// the value a graphics quality selected at +0x60, the one the draw jobs read each frame at +0x64, and
// a pending change at +0x68 that the game later applies (+0x60 to +0x64) or reverts (+0x64 to +0x60).
// The game's Extra High has 128 and 250.
struct Length
{
    std::uintptr_t rva, vtable;
    float longer;
};
constexpr std::array Lengths{Length{0x2B74330, 0x20F4DF8, 1024}, Length{0x2B77FB0, 0x20FFA20, 2048}};
constexpr std::size_t AppliedOffset = 0x64, PendingOffset = 0x68;

// Selects a quality's value by name for both lengths: sets +0x60 and the pending change unless the
// value equals the applied one.
constexpr std::uintptr_t SelectRva = 0x205AF0;
// mov r11, rsp; push rbp; push rsi; push rdi; sub rsp, 0x50; mov qword ptr [r11 - 0x48], -2
constexpr unsigned char SelectCode[]{0x4C, 0x8B, 0xDC, 0x55, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x50, 0x49, 0xC7, 0x43, 0xB8, 0xFE, 0xFF, 0xFF, 0xFF};

using CreateController = void* (*)(std::uint8_t* description);
using Select = int (*)(std::uint8_t* setting, void* request);
CreateController originalCreateController{};
Select originalSelect{};
bool detailAvailable{};

std::atomic<int> wantedFullDetail{}, wantedCoarse{}, wantedCoarsest{};
std::atomic<bool> wantedFarDetail{};
// The windows the last location loaded with; 0 before one loaded.
std::atomic<int> loadedFullDetail{}, loadedCoarse{}, loadedCoarsest{};

// While the detail is raised, what the game would have without the add-on, given back when it stops.
// A length's game value is read once no change of the game's is pending: the game's own apply or
// revert runs on its own values, untouched.
std::mutex detailMutex;
bool detailRaised{};
int gameLodResolution{};
std::array<float, Lengths.size()> gameLength{};
std::array<bool, Lengths.size()> lengthUnread{};

std::uint8_t* image()
{
    return reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
}

int& field(std::uint8_t* description, std::size_t offset)
{
    return *reinterpret_cast<int*>(description + offset);
}

bool openWorld(std::uint8_t* description, const Window& window)
{
    return field(description, window.size) == window.openWorldSize && field(description, window.size + 4) == window.openWorldSize
        && field(description, window.count) == GameCount && field(description, window.count + 4) == GameCount;
}

void setCount(std::uint8_t* description, const Window& window, int count)
{
    field(description, window.count) = count;
    field(description, window.count + 4) = count;
}

void* createController(std::uint8_t* description)
{
    const int fullDetail = wantedFullDetail, coarse = wantedCoarse, coarsest = wantedCoarsest;
    if (!fullDetail && !coarse && !coarsest)
    {
        loadedFullDetail = loadedCoarse = loadedCoarsest = GameCount;
        log("A location loads with the game's streaming windows.");
        return originalCreateController(description);
    }

    if (openWorld(description, FullDetail) && openWorld(description, Coarse) && openWorld(description, Coarsest))
    {
        loadedFullDetail = fullDetail ? fullDetail : GameCount;
        loadedCoarse = coarse ? coarse : GameCount;
        loadedCoarsest = coarsest ? coarsest : GameCount;
        setCount(description, FullDetail, loadedFullDetail);
        setCount(description, Coarse, loadedCoarse);
        setCount(description, Coarsest, loadedCoarsest);
        log(std::format("A location loads with full detail in {0} x {0} blocks and its coarser copies in {1} x {1} and {2} x {2}.",
            loadedFullDetail.load(), loadedCoarse.load(), loadedCoarsest.load()));
    }
    else
    {
        loadedFullDetail = loadedCoarse = loadedCoarsest = 0;
        log("A location's streaming is laid out unlike the open world's; its windows stay the game's.");
    }

    return originalCreateController(description);
}

std::uint8_t* settingOf(const Length& length)
{
    return image() + length.rva;
}

float& applied(const Length& length)
{
    return *reinterpret_cast<float*>(settingOf(length) + AppliedOffset);
}

bool pending(const Length& length)
{
    return settingOf(length)[PendingOffset] != 0;
}

// While the detail is raised, the game's selection compares with the game's own value and, when it
// leaves a change pending, keeps that value until the change is applied or reverted.
int select(std::uint8_t* setting, void* request)
{
    std::lock_guard lock(detailMutex);
    for (std::size_t i = 0; i < Lengths.size(); ++i)
    {
        const auto& length = Lengths[i];
        if (!detailRaised || setting != settingOf(length)) continue;

        if (!lengthUnread[i]) applied(length) = gameLength[i];
        const int result = originalSelect(setting, request);
        if (!pending(length) && !lengthUnread[i]) applied(length) = length.longer;
        return result;
    }

    return originalSelect(setting, request);
}

template<class Function> MH_STATUS hookAt(std::uint8_t* target, void* detour, Function& original)
{
    auto status = MH_CreateHook(target, detour, reinterpret_cast<void**>(&original));
    if (status == MH_OK) status = MH_EnableHook(target);
    if (status != MH_OK)
    {
        MH_RemoveHook(target);
        original = nullptr;
    }

    return status;
}
}

std::string hook() noexcept
{
    if (!supportedBuild()) return "Unknown game build; the draw distance stays the game's.";

    auto* base = image();
    std::string result;
    if (std::memcmp(base + CreateControllerRva, CreateControllerCode, sizeof(CreateControllerCode)))
        result = "The stream controller's creation has unexpected code; the loading distance stays the game's.";
    else if (const auto status = hookAt(base + CreateControllerRva, reinterpret_cast<void*>(&createController), originalCreateController); status != MH_OK)
        result = std::string("Hooking the stream controller's creation failed (") + MH_StatusToString(status) + "); the loading distance stays the game's.";
    else
        result = "The stream controller's creation is hooked: the loading distance can change.";

    bool settingsFound = !std::memcmp(base + LodResolutionSetterRva, LodResolutionSetterCode, sizeof(LodResolutionSetterCode))
        && !std::memcmp(base + SelectRva, SelectCode, sizeof(SelectCode));
    for (const auto& length : Lengths)
        settingsFound = settingsFound
            && game_memory::isA(reinterpret_cast<std::uintptr_t>(settingOf(length)), reinterpret_cast<std::uintptr_t>(base + length.vtable));
    if (!settingsFound) return result + " The level of detail's settings are not as expected; it stays the game's.";

    if (const auto status = hookAt(base + SelectRva, reinterpret_cast<void*>(&select), originalSelect); status != MH_OK)
        return result + " Hooking the graphics quality selection failed (" + MH_StatusToString(status) + "); the level of detail stays the game's.";

    detailAvailable = true;
    return result + " The level of detail can change.";
}

void configure(const Options& options) noexcept
{
    wantedFullDetail = options.fullDetail;
    wantedCoarse = options.coarse;
    wantedCoarsest = options.coarsest;
    wantedFarDetail = options.farDetail;
}

void onFrameStart() noexcept
{
    if (!detailAvailable) return;

    std::lock_guard lock(detailMutex);
    auto& resolution = *reinterpret_cast<int*>(image() + LodResolutionRva);
    if (!wantedFarDetail && !detailRaised) return;

    if (!detailRaised) lengthUnread.fill(true);

    // A resolution other than ours is the game's own, set since.
    if (!detailRaised || resolution != FarLodResolution) gameLodResolution = resolution;

    for (std::size_t i = 0; i < Lengths.size(); ++i)
    {
        const auto& length = Lengths[i];
        if (pending(length))
        {
            lengthUnread[i] = true;
            continue;
        }

        if (lengthUnread[i]) gameLength[i] = applied(length);
        lengthUnread[i] = false;
        applied(length) = wantedFarDetail ? length.longer : gameLength[i];
    }

    detailRaised = wantedFarDetail;
    resolution = detailRaised ? FarLodResolution : gameLodResolution;
}

std::string problem()
{
    if (!supportedBuild()) return {};

    const bool widening = wantedFullDetail || wantedCoarse || wantedCoarsest;
    if (widening && !originalCreateController) return "The loading distance cannot change (see PhantomFX.log).";
    if (wantedFarDetail && !detailAvailable) return "The level of detail cannot change (see PhantomFX.log).";

    return {};
}

bool waitsForLocation() noexcept
{
    const auto loaded = [](int wanted) { return wanted ? wanted : GameCount; };

    return loadedFullDetail
        && (loadedFullDetail != loaded(wantedFullDetail) || loadedCoarse != loaded(wantedCoarse) || loadedCoarsest != loaded(wantedCoarsest));
}
}
