#include "photo_mode.hpp"

#include "camera.hpp"
#include "device_check.hpp"
#include "focus.hpp"
#include "log.hpp"

#include <Windows.h>
#include <MinHook.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
#include <string>

namespace phantom_fx::photo_mode
{
namespace
{
// The main camera selector's update, on the game thread every frame (from 0x438470): of the cameras
// at [selector + 0x90] (count at + 0x88), the first with its active byte set becomes the render view's
// GrCamera ([view + 0x570]): world and view matrices from its rotation (a quaternion x, y, z, w) and
// position, lens and clip planes from the rest. It also places the audio listener. Culling, lighting
// and the scene constants all follow GrCamera.
constexpr std::uintptr_t SelectorUpdateRva = 0x437B60;
// mov rax, rsp; push rbp; push r12; push r13; push r14; push r15; lea rbp, [rax - 0x5F]
constexpr unsigned char SelectorUpdateStart[]{0x48, 0x8B, 0xC4, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x68, 0xA1};
constexpr std::size_t CameraCount = 0x88, Cameras = 0x90, CameraActive = 0xE0, CameraTransform = 0x120;
// The camera's lens, which the depth of field plugin (0x1DD1F0) reads through GrCamera: the focus
// distance in metres and the aperture as an f-number, from f/100 up no blur at all. The plugin
// raises the aperture to a minimum for the focal length (f/3 at 21 mm) unless the camera's byte
// +0xEA is set.
constexpr std::size_t CameraFocusDistance = 0x38, CameraFocalLength = 0x3C, CameraAperture = 0x44, CameraAnyAperture = 0xEA;

// The window's raw input, from its WM_INPUT handler: reads the RAWINPUT and hands mouse and keyboard
// data to the game's devices.
constexpr std::uintptr_t RawInputRva = 0x1A45540;
// mov [rsp + 8], rbx; mov [rsp + 0x20], rsi; push rdi; sub rsp, 0x250
constexpr unsigned char RawInputStart[]{0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x81, 0xEC, 0x50, 0x02, 0x00, 0x00};
constexpr USHORT ButtonReleases = RI_MOUSE_BUTTON_1_UP | RI_MOUSE_BUTTON_2_UP | RI_MOUSE_BUTTON_3_UP | RI_MOUSE_BUTTON_4_UP | RI_MOUSE_BUTTON_5_UP;

// The pause manager, a global: up to 16 requests, each a dword of pause bits with bit 31 set while
// active. Each frame start ORs the active ones into [manager + 8]; any bit stops game time (0x816A0)
// and systems test their own bits. Create (vtable slot 3) takes (manager, bits, name hash, name) and
// returns the request inactive; the game sets and clears bit 31 itself.
constexpr std::uintptr_t PauseManagerRva = 0x2B65D40, PauseVtableRva = 0x20C56C0, CreatePauseRva = 0xF1C80;
// push rsi; push rdi; push r14; sub rsp, 0x30
constexpr unsigned char CreatePauseStart[]{0x40, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x30};
// Most of the game's own requests ask for every bit but bit 0; under them the camera selector and
// rendering go on.
constexpr std::uint32_t PhotoPauses = 0x7FFFFFFE, Active = 0x80000000;
constexpr std::uint32_t PauseName = 0x4F544850; // A hash none of the game's requests use

// Metres per second, radians per mouse count.
constexpr float Speed = 5, FastSpeed = 25, MouseSensitivity = 0.001f;
// Just short of straight up or down, where the yaw would be lost.
constexpr float MaxPitch = 1.55f;
// A hitch does not throw the camera.
constexpr double MaxStep = 0.1;
// With the blur off, f/64 keeps everything sharp. The depth of field plugin skips f/100 and up, and
// the add-on's depth of field runs in its passes.
constexpr float SharpAperture = 64.0f;
// Millimetres, when the camera's focal length is not one.
constexpr float FallbackFocalLength = 35.0f;
// Focus distances below this one, in metres, are taken as this one.
constexpr float MinFocusDistance = 0.1f;
// Seconds the autofocus takes to settle on a new distance, about.
constexpr float FocusTime = 0.2f;

using SelectorUpdate = void (*)(std::uint8_t* selector);
using RawInput = bool (*)(void* devices, HRAWINPUT input, HWND window);
using CreatePause = std::uint32_t* (*)(void* manager, std::uint32_t pauses, std::uint32_t nameHash, const char* name);
SelectorUpdate originalSelectorUpdate{};
RawInput originalRawInput{};
CreatePause createPause{};
void* pauseManager{};

// Fox Engine is right-handed with y up, and a camera looks along its local +z. Yaw 0 looks along
// +z, and the pitch is positive upwards.
struct FreeCamera
{
    float position[3]{};
    float yaw{}, pitch{};
    float focusDistance{};
};

// What the selector's update reads from the active camera that photo mode sets, and the focal length
// in millimetres, which it only reads.
struct CameraState
{
    float transform[8]{}; // Rotation, position
    float focusDistance{}, aperture{};
    std::uint8_t anyAperture{};
    float focalLength{};
};

// A lens shot on the game thread: each camera update takes the next point of the aperture.
struct Shot
{
    std::uint32_t id{};
    bool active{};
    std::uint32_t next{};
    float centre[3]{};
    float focusDistance{};
    float radius{}; // The aperture's, metres
    int blades{};
    std::uint32_t frames{};
};

std::atomic<bool> installed{}, requested{}, flying{}, hideUi{}, blur{true}, shotToggle{};
std::atomic<float> fNumber{1.4f};
std::atomic<int> blades{};
std::atomic<std::uint32_t> shotFrames{256}, finishedShot{};
std::atomic<std::uint8_t> heldKeys{};
std::atomic<int> mouseX{}, mouseY{};
std::atomic<std::uint32_t*> pauseRequest{};

// Game thread only.
FreeCamera freeCamera;
LARGE_INTEGER lastStep{};
Shot shot;

// Written on the game thread, read by the frames being recorded.
std::mutex lensMutex;
Lens published;

enum Key : std::uint8_t { Forward = 1, Back = 2, Left = 4, Right = 8, Up = 16, Down = 32, Fast = 64 };
constexpr std::uint8_t MoveKeys = Forward | Back | Left | Right | Up | Down;

std::uint8_t* activeCamera(std::uint8_t* selector)
{
    const auto count = *reinterpret_cast<const std::uint32_t*>(selector + CameraCount);
    auto* const* cameras = *reinterpret_cast<std::uint8_t* const* const*>(selector + Cameras);
    for (std::uint32_t i = 0; cameras && i < count; ++i)
        if (cameras[i] && cameras[i][CameraActive]) return cameras[i];

    return nullptr;
}

CameraState stateOf(const std::uint8_t* camera)
{
    CameraState state;
    std::memcpy(state.transform, camera + CameraTransform, sizeof(state.transform));
    std::memcpy(&state.focusDistance, camera + CameraFocusDistance, sizeof(state.focusDistance));
    std::memcpy(&state.aperture, camera + CameraAperture, sizeof(state.aperture));
    state.anyAperture = camera[CameraAnyAperture];
    std::memcpy(&state.focalLength, camera + CameraFocalLength, sizeof(state.focalLength));

    return state;
}

void apply(std::uint8_t* camera, const CameraState& state)
{
    std::memcpy(camera + CameraTransform, state.transform, sizeof(state.transform));
    std::memcpy(camera + CameraFocusDistance, &state.focusDistance, sizeof(state.focusDistance));
    std::memcpy(camera + CameraAperture, &state.aperture, sizeof(state.aperture));
    camera[CameraAnyAperture] = state.anyAperture;
}

FreeCamera fromGame(const CameraState& game)
{
    const float* transform = game.transform;
    const float x = transform[0], y = transform[1], z = transform[2], w = transform[3];
    // The rotated +z axis; roll is dropped.
    const float forward[3]{2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)};

    return {{transform[4], transform[5], transform[6]}, std::atan2(forward[0], forward[2]),
        std::clamp(std::asin(std::clamp(forward[1], -1.0f, 1.0f)), -MaxPitch, MaxPitch), game.focusDistance};
}

// The rotation is the yaw about +y after the pitch about +x; looking up is a negative turn about +x.
CameraState photoState(const FreeCamera& camera, const CameraState& game)
{
    const float sy = std::sin(camera.yaw / 2), cy = std::cos(camera.yaw / 2);
    const float sp = std::sin(-camera.pitch / 2), cp = std::cos(-camera.pitch / 2);

    auto state = game;
    const float rotation[4]{cy * sp, sy * cp, -sy * sp, cy * cp};
    std::memcpy(state.transform, rotation, sizeof(rotation));
    std::memcpy(state.transform + 4, camera.position, sizeof(camera.position));
    state.focusDistance = camera.focusDistance;
    state.aperture = blur ? fNumber.load() : SharpAperture;
    state.anyAperture = 1;

    return state;
}

void fly(FreeCamera& camera, std::uint8_t keys, int turnX, int turnY, float seconds)
{
    // Moving the mouse right turns right, which is towards -x when looking along +z.
    camera.yaw -= static_cast<float>(turnX) * MouseSensitivity;
    camera.pitch = std::clamp(camera.pitch - static_cast<float>(turnY) * MouseSensitivity, -MaxPitch, MaxPitch);

    const auto axis = [keys](Key positive, Key negative) { return static_cast<float>((keys & positive) != 0) - static_cast<float>((keys & negative) != 0); };
    const float ahead = axis(Forward, Back), aside = axis(Right, Left), rise = axis(Up, Down);
    const float distance = (keys & Fast ? FastSpeed : Speed) * seconds;
    const float forward[3]{std::cos(camera.pitch) * std::sin(camera.yaw), std::sin(camera.pitch), std::cos(camera.pitch) * std::cos(camera.yaw)};
    const float right[3]{-std::cos(camera.yaw), 0, std::sin(camera.yaw)};
    for (int i = 0; i < 3; ++i) camera.position[i] += distance * (ahead * forward[i] + aside * right[i]);

    camera.position[1] += distance * rise;
}

void pause(bool on)
{
    auto* request = pauseRequest.load();
    if (on && !request)
    {
        // Made once and kept, so the game's 16 requests are not used up.
        request = createPause(pauseManager, PhotoPauses, PauseName, "PhotoMode");
        pauseRequest = request;
        if (!request) log("The game has no free pause request; photo mode does not freeze it.");
    }

    if (!request) return;

    std::atomic_ref<std::uint32_t> flags(*request);
    if (on) flags.fetch_or(Active);
    else flags.fetch_and(~Active);
}

double secondsSince(LARGE_INTEGER& last)
{
    LARGE_INTEGER now{}, frequency{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    const double seconds = static_cast<double>(now.QuadPart - last.QuadPart) / static_cast<double>(frequency.QuadPart);
    last = now;

    return std::min(seconds, MaxStep);
}

float focalLengthOf(const CameraState& game)
{
    return game.focalLength > 1 && game.focalLength < 5000 ? game.focalLength : FallbackFocalLength;
}

void beginShot(const CameraState& game)
{
    static std::uint32_t shots{};
    const float focal = focalLengthOf(game);
    shot = {.id = ++shots, .active = true, .focusDistance = std::max(freeCamera.focusDistance, MinFocusDistance),
        .radius = blur ? focal / 1000 / fNumber / 2 : 0, .blades = blades, .frames = shotFrames};
    std::memcpy(shot.centre, freeCamera.position, sizeof(shot.centre));
    log(std::format("Lens shot {}: {:.0f} mm at f/{:.1f}, focus {:.2f} m, {} frames.", shot.id, focal, fNumber.load(), shot.focusDistance,
        shot.frames));
}

// The shot's next point of the aperture, in the camera's own plane.
void moveToLensPoint(CameraState& state)
{
    const auto point = camera::aperturePoint(shot.next++, shot.blades);
    const float sy = std::sin(freeCamera.yaw), cy = std::cos(freeCamera.yaw);
    const float sp = std::sin(freeCamera.pitch), cp = std::cos(freeCamera.pitch);
    const float right[3]{-cy, 0, sy}, up[3]{-sy * sp, cp, -cy * sp};
    for (int i = 0; i < 3; ++i) state.transform[4 + i] = shot.centre[i] + shot.radius * (point[0] * right[i] + point[1] * up[i]);
}

void publish(const CameraState& game)
{
    std::lock_guard lock(lensMutex);
    published = {.active = flying, .aperture = blur ? focalLengthOf(game) / 1000 / fNumber : 0,
        .focusDistance = shot.active ? shot.focusDistance : std::max(freeCamera.focusDistance, MinFocusDistance), .shot = shot.id,
        .shooting = flying && shot.active, .shotFrames = shot.frames, .centre = {shot.centre[0], shot.centre[1], shot.centre[2]}};
}

void unpublish()
{
    std::lock_guard lock(lensMutex);
    published.active = false;
    published.shooting = false;
}

void selectorUpdate(std::uint8_t* selector)
{
    auto* camera = activeCamera(selector);
    const bool wanted = requested;
    // Without a camera (a loading screen) the request lapses.
    if (wanted && !flying && !camera) requested = false;
    else if (wanted && !flying)
    {
        freeCamera = fromGame(stateOf(camera));
        secondsSince(lastStep);
        mouseX = 0;
        mouseY = 0;
        pause(true);
        focus::setActive(true);
        flying = true;
    }
    else if (!wanted && flying)
    {
        pause(false);
        focus::setActive(false);
        flying = false;
        shot.active = false;
        unpublish();
    }

    if (!flying || !camera)
    {
        originalSelectorUpdate(selector);
        return;
    }

    const float seconds = static_cast<float>(secondsSince(lastStep));
    const int turnX = mouseX.exchange(0), turnY = mouseY.exchange(0);
    const std::uint8_t keys = heldKeys;
    const auto game = stateOf(camera);

    // Moving the camera ends a lens shot, as F5 does; while one lasts, the camera and focus stay.
    if (shot.active && (turnX || turnY || (keys & MoveKeys))) shot.active = false;
    if (shotToggle.exchange(false))
    {
        if (shot.active) shot.active = false;
        else beginShot(game);
    }

    if (!shot.active)
    {
        fly(freeCamera, keys, turnX, turnY, seconds);
        if (const float target = focus::distance(); target > 0)
            freeCamera.focusDistance += (target - freeCamera.focusDistance) * (1 - std::exp(-seconds / FocusTime));
    }

    // Only this update sees the free camera; the game's camera keeps its own.
    auto state = photoState(freeCamera, game);
    if (shot.active && finishedShot != shot.id) moveToLensPoint(state);
    publish(game);
    apply(camera, state);
    originalSelectorUpdate(selector);
    apply(camera, game);
}

// In photo mode the mouse turns the free camera and the game gets no presses. Releases still reach
// it, so nothing held when photo mode started stays held after it.
bool rawInput(void* devices, HRAWINPUT input, HWND window)
{
    if (!flying) return originalRawInput(devices, input, window);

    RAWINPUT data{};
    UINT size = sizeof(data);
    if (GetRawInputData(input, RID_INPUT, &data, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1))
        return originalRawInput(devices, input, window);

    if (data.header.dwType == RIM_TYPEMOUSE)
    {
        const auto& mouse = data.data.mouse;
        if (!(mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
        {
            mouseX += mouse.lLastX;
            mouseY += mouse.lLastY;
        }

        return mouse.usButtonFlags & ButtonReleases ? originalRawInput(devices, input, window) : true;
    }

    if (data.header.dwType == RIM_TYPEKEYBOARD) return data.data.keyboard.Flags & RI_KEY_BREAK ? originalRawInput(devices, input, window) : true;

    return originalRawInput(devices, input, window);
}

template<class Function> MH_STATUS hookAt(std::uint8_t* target, void* detour, Function& original)
{
    auto status = MH_CreateHook(target, detour, reinterpret_cast<void**>(&original));
    if (status == MH_OK) status = MH_EnableHook(target);
    if (status != MH_OK) MH_RemoveHook(target);

    return status;
}
}

std::string hook() noexcept
{
    if (!supportedBuild()) return "Unknown game build; photo mode is unavailable.";

    auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    auto* manager = base + PauseManagerRva;
    if (std::memcmp(base + SelectorUpdateRva, SelectorUpdateStart, sizeof(SelectorUpdateStart))
        || std::memcmp(base + RawInputRva, RawInputStart, sizeof(RawInputStart))
        || std::memcmp(base + CreatePauseRva, CreatePauseStart, sizeof(CreatePauseStart))
        || *reinterpret_cast<std::uint8_t**>(manager) != base + PauseVtableRva)
        return "The camera, input or pause code is unexpected; photo mode is unavailable.";

    pauseManager = manager;
    createPause = reinterpret_cast<CreatePause>(base + CreatePauseRva);
    auto status = hookAt(base + SelectorUpdateRva, reinterpret_cast<void*>(&selectorUpdate), originalSelectorUpdate);
    if (status == MH_OK)
    {
        status = hookAt(base + RawInputRva, reinterpret_cast<void*>(&rawInput), originalRawInput);
        if (status != MH_OK) MH_RemoveHook(base + SelectorUpdateRva);
    }

    if (status != MH_OK) return std::string("Hooking the camera failed (") + MH_StatusToString(status) + "); photo mode is unavailable.";

    installed = true;

    return "The camera selector and raw input are hooked: F2 toggles photo mode.";
}

void toggle() noexcept
{
    if (installed) requested = !requested;
}

void toggleUi() noexcept
{
    hideUi = !hideUi;
}

void toggleBlur() noexcept
{
    blur = !blur;
}

void configure(const LensSettings& settings) noexcept
{
    fNumber = settings.fNumber;
    blades = settings.blades;
    shotFrames = settings.shotFrames;
}

Lens lens() noexcept
{
    std::lock_guard lock(lensMutex);
    return published;
}

void toggleShot() noexcept
{
    if (flying) shotToggle = true;
}

void shotFinished(std::uint32_t id) noexcept
{
    finishedShot = id;
}

void setControls(const Controls& controls) noexcept
{
    heldKeys = static_cast<std::uint8_t>((controls.forward ? Forward : 0) | (controls.back ? Back : 0) | (controls.left ? Left : 0)
        | (controls.right ? Right : 0) | (controls.up ? Up : 0) | (controls.down ? Down : 0) | (controls.fast ? Fast : 0));
}

bool uiHidden() noexcept
{
    return hideUi || flying;
}

void leave() noexcept
{
    requested = false;
    flying = false;
    pause(false);
    focus::setActive(false);
    unpublish();
}
}
