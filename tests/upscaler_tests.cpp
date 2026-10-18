#include "camera.hpp"
#include "upscaler_settings.hpp"

#include <Windows.h>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>

namespace
{
namespace fs = std::filesystem;
using namespace mgsv_dlss;
using namespace mgsv_dlss::upscaler;

void expect(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

bool close(double actual, double expected, double tolerance = 1e-4)
{
    return std::abs(actual - expected) <= tolerance;
}

void write(const fs::path& path, std::string_view text)
{
    std::ofstream output(path, std::ios::binary);
    output << text;
    output.close();
    expect(static_cast<bool>(output), "Write the test fixture.");
}

void settingsPersist(const fs::path& root)
{
    const auto path = root / L"MGSVDLSS.ini";
    expect(loadSettings(path) == Settings{}, "A first launch uses DLAA with the default preset and sharpening.");
    expect(!fs::exists(path), "Loading defaults must not create a settings file.");

    const Settings chosen{.mode = Mode::Off, .preset = Preset::J, .sharpness = 0.5f, .unlockFps = true};
    saveSettings(path, chosen);
    expect(loadSettings(path) == chosen, "Restart restores the mode, preset, sharpening and frame rate unlock.");

    write(path, "# DLSS\r\nunknown=future\r\n mode = dlaa \r\npreset=13\r\nbroken\r\n");
    expect(loadSettings(path) == Settings{Mode::Dlaa, Preset::M}, "CRLF, whitespace and unknown keys keep the valid fields.");
    write(path, "mode=ultra\npreset=7\nsharpness=nan\ncustom_scale=inf\n");
    expect(loadSettings(path) == Settings{}, "Unknown modes, reserved presets and invalid numbers fall back to defaults.");
    write(path, "sharpness=-1\n");
    expect(loadSettings(path).sharpness == 0, "Negative sharpening turns it off.");
    write(path, "sharpness=7\n");
    expect(loadSettings(path).sharpness == Settings::MaxSharpness, "Sharpening is clamped.");
    write(path, "custom_scale=0.1\nmip_bias_offset=-5\n");
    expect(loadSettings(path).customScale == Settings::MinScale && loadSettings(path).mipBiasOffset == Settings::MinMipBias,
        "The custom scale and the texture bias offset are clamped.");

    const Settings upscaling{.mode = Mode::Custom, .preset = Preset::K, .sharpness = 0.25f, .customScale = 0.75f, .mipBiasOffset = -0.5f};
    saveSettings(path, upscaling);
    expect(loadSettings(path) == upscaling, "Restart restores an upscaling mode with its custom scale and texture bias offset.");

    for (const auto& invalid : {Settings{.mode = Mode::Count}, Settings{.preset = static_cast<Preset>(7)},
             Settings{.sharpness = std::numeric_limits<float>::quiet_NaN()}, Settings{.sharpness = 2}, Settings{.customScale = 2},
             Settings{.mipBiasOffset = 3}})
    {
        saveSettings(path, chosen);
        bool rejected = false;
        try
        {
            saveSettings(path, invalid);
        }
        catch (const std::exception&)
        {
            rejected = true;
        }

        expect(rejected && loadSettings(path) == chosen, "Invalid settings cannot replace valid saved ones.");
    }
}

// The main view of a recorded frame (docs/DEVELOPMENT.md, "Constant buffers"): P00 = -1.75, P11 = 28/9,
// reversed-Z with near 0.069 and far 4000, at 1920x1080.
constexpr float A = -1.72503e-05f, B = 0.0690012f;
constexpr float ViewRows[4][4]{
    {-0.310798f, 0.0f, 0.950476f, -202.916f},
    {0.0333242f, 0.999385f, 0.0108968f, -319.845f},
    {-0.949891f, 0.0350606f, -0.310607f, 153.871f},
    {0, 0, 0, 1},
};

void fill(camera::SceneConstants& scene, const float (&view)[4][4], const float eye[3])
{
    scene = {};
    const float projection[4][4]{{-1.75f, 0, 0, 0}, {0, 28.0f / 9, 0, 0}, {0, 0, A, B}, {0, 0, 1, 0}};
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
        {
            scene.c[4 + row][column] = projection[row][column];
            scene.c[8 + row][column] = view[row][column];
            for (int k = 0; k < 4; ++k) scene.c[row][column] += projection[row][k] * view[k][column];
        }

    for (int i = 0; i < 3; ++i) scene.c[20][i] = eye[i];
    scene.c[20][3] = 1;
    scene.c[22][0] = 1920;
    scene.c[22][1] = 1080;
}

// Pixel position (x right, y down) of a world point through the scene's projection-view.
std::array<double, 2> project(const camera::SceneConstants& scene, const double (&point)[3])
{
    double clip[4]{};
    for (int row = 0; row < 4; ++row)
        clip[row] = scene.c[row][0] * point[0] + scene.c[row][1] * point[1] + scene.c[row][2] * point[2] + scene.c[row][3];

    return {(clip[0] / clip[3] + 1) * 960, (1 - clip[1] / clip[3]) * 540};
}

void mainViewRecognised()
{
    constexpr float eye[3]{93.75f, 314.25f, 244.15f};
    camera::SceneConstants scene;
    fill(scene, ViewRows, eye);
    expect(camera::classify(scene) == camera::SceneView::MainCamera, "The main view's upload is the camera.");

    auto motionBlur = scene;
    motionBlur.c[8][3] += 5; // MotionBlurCameraVelocity keeps the previous projection-view in c8-c11
    expect(camera::classify(motionBlur) == camera::SceneView::MainProjection, "Passes with other matrices in c8-c11 still get the jitter.");

    auto shadow = scene;
    shadow.c[7][2] = 0;
    shadow.c[7][3] = 1;
    expect(camera::classify(shadow) == camera::SceneView::Other, "Orthographic shadow views are left alone.");

    auto square = scene;
    square.c[4][0] = -square.c[5][1];
    expect(camera::classify(square) == camera::SceneView::Other, "A square perspective view is not the main camera.");

    auto jittered = scene;
    camera::applyJitter(jittered, {0.001f, 0.001f});
    expect(camera::classify(jittered) == camera::SceneView::Other, "An upload is never jittered twice.");
}

void jitterShiftsTheImage()
{
    constexpr float eye[3]{93.75f, 314.25f, 244.15f};
    camera::SceneConstants scene;
    fill(scene, ViewRows, eye);
    const auto view = camera::viewOf(scene);
    const std::array<float, 2> pixels{0.25f, -0.375f};
    auto jittered = scene;
    camera::applyJitter(jittered, camera::jitterNdc(pixels, 1920, 1080));

    // A point 30 m in front of the camera.
    const double point[3]{eye[0] + 30.0 * ViewRows[2][0], eye[1] + 30.0 * ViewRows[2][1], eye[2] + 30.0 * ViewRows[2][2]};
    const auto before = project(scene, point), after = project(jittered, point);
    expect(close(after[0] - before[0], pixels[0], 1e-3) && close(after[1] - before[1], pixels[1], 1e-3),
        "The image moves by the jitter in render pixels, x right and y down, as DLSS takes it.");
    expect(jittered.c[28][0] == camera::jitterNdc(pixels, 1920, 1080)[0] && jittered.c[28][1] == camera::jitterNdc(pixels, 1920, 1080)[1],
        "m_cameraCenterOffset carries the NDC offset the screen-space passes subtract.");
    expect(camera::viewOf(scene).projectionView == view.projectionView, "The camera is captured before the jitter.");
}

void reprojection()
{
    constexpr float eye[3]{93.75f, 314.25f, 244.15f};
    camera::SceneConstants scene;
    fill(scene, ViewRows, eye);
    const auto view = camera::viewOf(scene);
    const auto still = camera::previousFromCurrent(view, view);
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            expect(close(still[row * 4 + column], row == column ? 1 : 0, 1e-9), "A camera that stands still has no motion.");

    // Previous frame: the camera 0.2 m to the side, so a point straight ahead was elsewhere on screen.
    camera::SceneConstants before;
    const float previousEye[3]{eye[0] - 0.2f * ViewRows[0][0], eye[1] - 0.2f * ViewRows[0][1], eye[2] - 0.2f * ViewRows[0][2]};
    float previousRows[4][4];
    std::memcpy(previousRows, ViewRows, sizeof(previousRows));
    for (int row = 0; row < 3; ++row)
        previousRows[row][3] = -(ViewRows[row][0] * previousEye[0] + ViewRows[row][1] * previousEye[1] + ViewRows[row][2] * previousEye[2]);

    fill(before, previousRows, previousEye);
    const auto moved = camera::previousFromCurrent(camera::viewOf(before), view);

    const double point[3]{eye[0] + 10.0 * ViewRows[2][0], eye[1] + 10.0 * ViewRows[2][1], eye[2] + 10.0 * ViewRows[2][2]};
    const auto now = project(scene, point), then = project(before, point);
    double clip[4]{};
    for (int row = 0; row < 4; ++row)
        clip[row] = scene.c[row][0] * point[0] + scene.c[row][1] * point[1] + scene.c[row][2] * point[2] + scene.c[row][3];

    const double position[4]{clip[0] / clip[3], clip[1] / clip[3], clip[2] / clip[3], 1};
    double previous[4]{};
    for (int row = 0; row < 4; ++row)
        for (int k = 0; k < 4; ++k) previous[row] += moved[row * 4 + k] * position[k];

    const double x = (previous[0] / previous[3] + 1) * 960, y = (1 - previous[1] / previous[3]) * 540;
    expect(close(x, then[0], 0.05) && close(y, then[1], 0.05) && !close(now[0], then[0], 1),
        "Depth and the two matrices find where a point was in the previous frame.");
}

void cuts()
{
    constexpr float eye[3]{93.75f, 314.25f, 244.15f};
    camera::SceneConstants scene;
    fill(scene, ViewRows, eye);
    const auto view = camera::viewOf(scene);

    auto walked = view;
    walked.eye[0] += 0.3f;
    expect(!camera::cameraCut(view, walked), "Riding or running is not a cut.");

    auto jumped = view;
    jumped.eye[1] += 3;
    expect(camera::cameraCut(view, jumped), "A camera that jumps metres in one frame is a cut.");

    auto turned = view;
    turned.forward = {-view.forward[0], view.forward[1], -view.forward[2]};
    expect(camera::cameraCut(view, turned), "A reverse shot is a cut.");

    auto zoomed = view;
    zoomed.focal *= 1.5f;
    expect(camera::cameraCut(view, zoomed), "A sudden change of field of view is a cut.");
}

void modeMaths()
{
    expect(!upscales(Mode::Off) && !upscales(Mode::Dlaa) && upscales(Mode::Quality) && upscales(Mode::Custom),
        "Only the modes below native resolution upscale.");
    expect(close(nominalScale(Mode::Quality, 0), 2.0 / 3) && close(nominalScale(Mode::Balanced, 0), 0.58)
            && close(nominalScale(Mode::Performance, 0), 0.5) && close(nominalScale(Mode::UltraPerformance, 0), 1.0 / 3)
            && nominalScale(Mode::Dlaa, 0.5f) == 1,
        "The modes keep NVIDIA's render scales.");
    expect(nominalScale(Mode::Custom, 0.8f) == 0.8f && nominalScale(Mode::Custom, 0.1f) == Settings::MinScale
            && nominalScale(Mode::Custom, std::numeric_limits<float>::quiet_NaN()) == Settings::DefaultCustomScale,
        "Custom uses its scale within the supported range.");

    expect(mipBias(1, 0) == 0 && close(mipBias(0.5f, 0), -1) && close(mipBias(2.0f / 3, 0), std::log2(2.0 / 3)),
        "The texture LOD bias is log2(render / output).");
    expect(close(mipBias(0.5f, 0.25f), -0.75) && close(mipBias(0.5f, -9), -1 + Settings::MinMipBias) && mipBias(0, 0) == 0,
        "The offset is added within its range, and invalid scales give no bias.");
}

void jitterSequence()
{
    expect(camera::jitterPhases(1.0f) == 8, "DLAA uses 8 phases.");
    expect(camera::jitterPhases(0.5f) == 32 && camera::jitterPhases(1.0f / 3) == 72 && camera::jitterPhases(0) == 8,
        "Lower render scales get NVIDIA's recommended count; an invalid scale gets 8.");

    std::set<std::pair<float, float>> offsets;
    double sumX = 0, sumY = 0;
    for (std::uint64_t frame = 0; frame < 8; ++frame)
    {
        const auto jitter = camera::jitterPixels(frame, 8);
        expect(jitter[0] >= -0.5f && jitter[0] < 0.5f && jitter[1] >= -0.5f && jitter[1] < 0.5f, "Jitter stays inside the pixel.");
        offsets.emplace(jitter[0], jitter[1]);
        sumX += jitter[0];
        sumY += jitter[1];
    }

    expect(offsets.size() == 8, "Each of the 8 phases samples a different position.");
    expect(std::abs(sumX / 8) < 0.07 && std::abs(sumY / 8) < 0.07, "The phases are centred on the pixel.");
    expect(camera::jitterPixels(3, 8) == camera::jitterPixels(11, 8), "The sequence repeats after its phases.");
    expect(close(camera::jitterPixels(0, 8)[0], 0) && close(camera::jitterPixels(0, 8)[1], 1.0 / 3 - 0.5), "It is Halton(2, 3) from index 1.");
}
}

int main()
{
    const auto root =
        fs::temp_directory_path() / (L"mgsv-dlss-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    try
    {
        fs::create_directories(root);
        settingsPersist(root);
        mainViewRecognised();
        jitterShiftsTheImage();
        reprojection();
        cuts();
        modeMaths();
        jitterSequence();
        fs::remove_all(root);
        std::cout << "Upscaler tests passed.\n";

        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        fs::remove_all(root);

        return 1;
    }
}
