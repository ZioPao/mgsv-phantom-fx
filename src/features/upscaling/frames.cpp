#include "features/upscaling/frames.hpp"

namespace phantom_fx::upscaler
{
void Frames::sceneWritten(game_constants::SceneConstants& scene, camera::SceneView kind, const FrameStart& start)
{
    if (kind == camera::SceneView::Other) return;

    if (hasExecutedSinceStart)
    {
        hasExecutedSinceStart = false;
        startFrame(start);
    }

    // The frame's largest camera view: a mirror's reflection is drawn first, and smaller (the intro's,
    // 1080x608 of a 2560x1440 frame, 2026-10-26).
    const float area = scene.width * scene.height;
    if (kind == camera::SceneView::MainCamera && (!frame.view || area > frame.view->width * frame.view->height)) frame.view = camera::viewOf(scene);
    if (frame.isJittered) camera::applyJitter(scene, camera::jitterNdc(frame.jitter, scene.width, scene.height));
}

std::uint64_t Frames::recordingFrame() const
{
    return hasExecutedSinceStart ? frame.index + 1 : frame.index;
}

void Frames::executed()
{
    hasExecutedSinceStart = true;
}

const Frame& Frames::current() const
{
    return frame;
}

void Frames::upscaled(bool shouldJitterNext)
{
    isNextJittered = shouldJitterNext;
}

void Frames::presented(bool wasUpscaled)
{
    if (!wasUpscaled) isNextJittered = false;
}

void Frames::startFrame(const FrameStart& start)
{
    frame.index++;
    frame.gameFrame = start.gameFrame;
    frame.isJittered = isNextJittered && start.canJitter;
    frame.jitter = frame.isJittered ? camera::jitterPixels(frame.index, start.jitterPhases) : std::array<float, 2>{};
    frame.view.reset();
}
}
