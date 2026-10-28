#pragma once

#include "camera.hpp"
#include "game_constants.hpp"

#include <array>
#include <cstdint>
#include <optional>

// The frames the upscaler runs on. The game records a whole frame, then executes it
// (docs/DEVELOPMENT.md, "Command lists"). A frame starts with the first main-view upload after the
// immediate context executed anything, and every main-view upload of the frame gets the same jitter.
namespace phantom_fx::upscaler
{
struct Frame
{
    std::uint64_t index{};
    // The game loop's frame (game_loop.hpp) as the frame's recording started, for Reflex.
    std::uint32_t gameFrame{};
    bool isJittered{};
    std::array<float, 2> jitter{}; // Render pixels
    std::optional<camera::View> view;
};

// Read only when an upload starts a frame.
struct FrameStart
{
    std::uint32_t gameFrame{};
    bool canJitter{};
    int jitterPhases{8};
};

// The frame being recorded: its start, its main view, its jitter. Not locked: called under recordMutex.
class Frames
{
public:
    // kind: camera::classify(scene), found before the lock so other views never wait on it; they neither
    // start a frame nor get the jitter.
    void sceneWritten(game_constants::SceneConstants& scene, camera::SceneView kind, const FrameStart& start);
    // The frame a pass recorded now belongs to. Between the immediate context's last call and the next
    // frame's first upload, that is the next frame.
    std::uint64_t recordingFrame() const;

    // The immediate context executed something: the next main-view upload starts a frame.
    void executed();
    const Frame& current() const;
    // At the frame's grading, once the upscaler ran or fell back: whether the next frame is jittered.
    void upscaled(bool shouldJitterNext);
    // A frame the upscaler did not run on (menus, loading screens, a fallback) leaves the next one
    // unjittered, so a 3D view without FXAA never shakes.
    void presented(bool wasUpscaled);

private:
    void startFrame(const FrameStart& start);

    Frame frame;
    bool hasExecutedSinceStart{true};
    bool isNextJittered{};
};
}
