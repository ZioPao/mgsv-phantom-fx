#pragma once

#include <array>
#include <cstdint>

// The camera side of DLSS: recognising the main view in the engine's scene constants, adding the
// jitter to it, and the matrices the motion vectors and camera cuts come from. Pure maths, so the
// tests can check it without the game.
namespace phantom_fx::camera
{
// cVSScene / cPSScene (slot b2, 480 bytes): c[n] is register cn as docs/DEVELOPMENT.md lists them.
// Matrices are rows applied as dot(row, v).
struct SceneConstants
{
    float c[30][4];
};

// Row-major 4x4 in the engine's convention.
using Matrix = std::array<double, 16>;

enum class SceneView : std::uint8_t
{
    Other,
    // c4-c7 hold the main camera's perspective projection for the render size in c22, unjittered.
    MainProjection,
    // Also c0-c3 = projection x view with the view in c8-c11: the camera itself.
    MainCamera,
};

SceneView classify(const SceneConstants& scene);

// The main camera as one upload had it before the jitter.
struct View
{
    Matrix projectionView{}, projection{}, view{};
    std::array<float, 3> eye{}, forward{};
    float focal{}; // P11: changes with the field of view
    float width{}, height{};
    float nearPlane{}, farPlane{};
};

View viewOf(const SceneConstants& scene);

// Offsets a MainProjection upload by `ndc`: the projection (c4, c5), the projection-view in c0-c3
// when it has one, and m_cameraCenterOffset (c28), which the engine's screen-space passes subtract
// before reconstructing positions from depth.
void applyJitter(SceneConstants& scene, std::array<float, 2> ndc);

// DLSS takes the jitter in render pixels, x right and y down; the projection takes it in NDC, y up.
std::array<float, 2> jitterNdc(std::array<float, 2> pixels, float width, float height);
// Halton(2, 3) offsets in [-0.5, 0.5) pixels; `frame` counts up by one per rendered frame.
std::array<float, 2> jitterPixels(std::uint64_t frame, int phases);
// DLSS asks for 8 x (output / render)^2 phases; DLAA renders at output size.
int jitterPhases(float renderScale);

// Maps this frame's unjittered NDC and depth to the previous frame's clip position.
Matrix previousFromCurrent(const View& previous, const View& current);

// A cut (cutscenes, menus into the game) moves or turns the camera further than gameplay does in
// one frame, or changes the field of view at once; DLSS then drops its history.
bool cameraCut(const View& previous, const View& current);

// The camera as NVIDIA Streamline takes it for DLSS Frame Generation: matrices for row vectors
// (v x M) and a view space with x right, y up and z forward. The engine's view mirrors x (P00 < 0);
// the mirror moves from the projection into the view, so the projection is the usual one and P x V
// is unchanged.
struct StreamlineCamera
{
    std::array<float, 16> viewToClip{}, clipToView{}, clipToPreviousClip{}, previousClipToClip{};
    std::array<float, 3> position{}, right{}, up{}, forward{};
    float nearPlane{}, farPlane{};
    float verticalFov{}; // Radians
    float aspectRatio{};
};

StreamlineCamera streamlineCamera(const View& previous, const View& current);
}
