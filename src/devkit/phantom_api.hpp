#pragma once

#include <cstddef>
#include <cstdint>

struct ID3D11DeviceContext;
struct ID3D11Resource;

// What PhantomFX's dev build lets the DevKit add-on reach: the parts of the automation that run inside its
// passes (the held camera, the records, the costs, the probes, the scene and the settings). The automation itself,
// its command language, runner, cases and F9, is DevKit's (devkit/automation.*) and calls these.
//
// Plain C types only: each add-on has its own CRT, so nothing that owns heap memory crosses. Text comes back through a
// caller's buffer: the function returns the text's length, and writes as much as fits, with a NUL; a result of `cap` or
// more means the buffer was short and the call can be made again with a larger one. kNone says there is no answer.
// PhantomFX exports PhantomFXGetDevApi() in a dev build alone; DevKit reads it once PhantomFX has loaded.
extern "C"
{
constexpr std::size_t PhantomFXNone = static_cast<std::size_t>(-1);
constexpr std::uint32_t PhantomFXDevApiVersion = 1;

struct PhantomFXPose
{
    float position[3];
    float yaw, pitch;
    float focalLength; // 0: the game camera's own lens
};

struct PhantomFXWeather
{
    int available;
    float clock;       // Seconds since midnight
    int requested;     // The weather type the sky shows or moves to, -1 for none
    float fogDensity;
};

struct PhantomFXRigidDraw
{
    float position[3];
    std::uint32_t firstIndex, indexCount;
};

struct PhantomFXDevApi
{
    std::uint32_t size, version;

    // The game's loop frame, and what the ray-traced scene holds: its text, and how much it has yet to make.
    std::uint32_t (*frame)();
    std::size_t (*sceneStatus)(char* out, std::size_t cap);
    std::size_t (*sceneWork)();

    // PhantomFX.ini's settings. The value of `key` (PhantomFXNone for no such setting).
    std::size_t (*settingValue)(const char* key, char* out, std::size_t cap);
    // 0 no such setting, 1 changed, 2 the value it already has, 3 turned down.
    int (*settingSet)(const char* key, const char* value);
    int (*enhancements)();
    void (*setEnhancements)(int isOn);
    // show_scene, two_sided_shadows, no_jitter, debug_view or river_light: whether the name is one.
    int (*diagnostic)(const char* name, int value);

    // The probes at a render pixel, and back to the screen's centre with the view the probe turned on put back.
    void (*probeAt)(std::uint32_t x, std::uint32_t y);
    std::size_t (*probeText)(char* out, std::size_t cap);
    void (*probeClear)();

    // The debug cases' camera: photo mode at a pose.
    int (*cameraLast)(PhantomFXPose* out);
    void (*cameraHold)(const PhantomFXPose* pose);
    void (*cameraMove)(const PhantomFXPose* pose, int updates);
    void (*cameraRelease)();
    int (*cameraState)(); // 0 the game's, 1 frozen, 2 held with the game running
    void (*cameraLive)(int isLive);

    // The sky.
    void (*weatherState)(PhantomFXWeather* out);
    int (*weatherTypes)();
    void (*setClock)(float seconds);
    void (*stopClock)(int isStopped);
    void (*forceWeather)(int type, float fogDensity);
    void (*releaseWeather)();

    // What the scene leaves out: `all`, `off` or hex model hashes; where a model is placed; the rigid draws near a place.
    void (*reveal)(const char* what);
    void (*watchOwners)(std::uint64_t hash);
    std::size_t (*watchedOwners)(char* out, std::size_t cap);
    void (*recordRigidDraws)(int isOn);
    void (*takeRigidDraws)(void (*each)(const PhantomFXRigidDraw* draw, void* user), void* user);

    // The records: every pixel's, and the costs. Paths are UTF-8, and the file's extension is added.
    void (*inspectRequest)(const char* file);
    std::uint64_t (*inspectFinished)();
    void (*perfCapture)(const char* file, std::uint32_t frames);
    void (*perfCancel)();
    std::uint64_t (*perfFinished)();
    std::size_t (*perfLast)(char* out, std::size_t cap);
    int (*perfRecording)();
    void (*setPerfRecording)(int isOn);

    // Render thread, from a present job: the back buffer, and the ray tracing's textures with the game's shadow.
    int (*saveBackBuffer)(ID3D11DeviceContext* context, ID3D11Resource* backBuffer, const char* file);
    void (*saveTextures)(ID3D11DeviceContext* context, const char* folder);
    void (*keepGameShadow)(int isKept);
    // The remastering tool's line, on the render thread: what it did.
    std::size_t (*remasterLine)(const char* line, char* out, std::size_t cap);
};

// PhantomFX's export: null before it is ready.
using PhantomFXGetDevApiFn = const PhantomFXDevApi* (*)();
}
