#include "camera.hpp"
#include "constant_capture.hpp"
#include "features/global_illumination/gi_lighting.hpp"
#include "features/upscaling/frames.hpp"
#include "features/upscaling/render_size_plan.hpp"
#include "frame_latch.hpp"
#include "game_constants.hpp"
#include "phantom_fx_settings.hpp"
#include "range_allocator.hpp"
#include "shader_bytecode.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
namespace fs = std::filesystem;
using namespace phantom_fx;
using namespace phantom_fx::upscaler;

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

// Over a valid saved file, so a rejected save can be seen to leave it as it was.
bool saveRejected(const fs::path& path, const Settings& saved, const Settings& invalid)
{
    saveSettings(path, saved);
    try
    {
        saveSettings(path, invalid);
    }
    catch (const std::exception&)
    {
        return loadSettings(path) == saved;
    }

    return false;
}

void settingsPersist(const fs::path& root)
{
    const auto path = root / L"PhantomFX.ini";
    const auto first = loadSettings(path);
    expect(first == firstLaunchSettings() && first.upscaling.upscaler == Upscaler::Dlss && first.upscaling.mode == Mode::Quality
            && presetOf(first) == EffectPreset::ScreenSpace,
        "A first launch upscales with DLSS Quality and the screen-space preset.");
    expect(!fs::exists(path), "Loading defaults must not create a settings file.");

    const Settings chosen{.upscaling = {.upscaler = Upscaler::Fsr, .mode = Mode::Off, .dlssPreset = Preset::J, .sharpness = 0.5f},
        .frameGeneration = {.mode = FrameGenerationMode::X3}, .sun = {.diameterDegrees = 1.25f}, .rayTracedShadows = {.on = true, .withGameShadows = true}, .softShadows = {.on = true},
        .sunContactShadows = {.on = true}, .rayTracedLights = {.on = true, .shadowRaysPerPixel = 8, .flashlightBrightness = 3.5f},
        .lampContactShadows = {.on = true}, .lampSoftShadows = {.on = true},
        .contactShadowShape = {.contrast = 2, .reachPixels = 40, .thickness = 0.01f, .fadeMetres = 300}, .globalIllumination = {.raysPerLightPixel = 4}, .ambientOcclusion = {.on = true, .radiusMetres = 2, .strength = 1.5f},
        .indirectLight = {.on = true, .radius = 0.9f, .strength = 0.5f}, .volumetricFog = {.on = true, .look = AtmosphereLook::Physical, .haze = 0.5f},
        .frameRate = {.unlocked = true}};
    saveSettings(path, chosen);
    expect(loadSettings(path) == chosen,
        "Restart restores the upscaler, mode, preset, sharpening, frame rate unlock, frame generation, soft, screen-space and ray-traced "
        "shadows, the contact shadows' shape, GTVBAO, SSGI, the global illumination's rays and the volumetric fog.");
    write(path, "frame_generation=dynamic\n");
    expect(loadSettings(path).frameGeneration.mode == FrameGenerationMode::Dynamic, "Dynamic frame generation is read back.");
    write(path, "frame_generation=fsr\n");
    expect(loadSettings(path).frameGeneration.mode == FrameGenerationMode::Fsr, "FSR frame generation is read back.");
    write(path, "frame_generation=8x\n");
    expect(loadSettings(path).frameGeneration.mode == FrameGenerationMode::Off, "An unknown multiplier leaves frame generation off.");

    write(path, "# DLSS\r\nunknown=future\r\n mode = dlaa \r\npreset=13\r\nbroken\r\n");
    expect(loadSettings(path) == Settings{.upscaling = {.mode = Mode::Dlaa, .dlssPreset = Preset::M}}, "CRLF, whitespace and unknown keys keep the valid fields.");
    write(path, "upscaler=xess\nmode=ultra\npreset=7\nsharpness=nan\ncustom_scale=inf\n");
    expect(loadSettings(path) == Settings{}, "Unknown upscalers and modes, reserved presets and invalid numbers fall back to defaults.");
    write(path, "sharpness=-1\n");
    expect(loadSettings(path).upscaling.sharpness == 0, "Negative sharpening turns it off.");
    write(path, "sharpness=7\n");
    expect(loadSettings(path).upscaling.sharpness == 1, "Sharpening is clamped.");
    write(path, "custom_scale=0.1\nmip_bias_offset=-5\n");
    const auto clamped = loadSettings(path).upscaling;
    expect(clamped.customRenderScale == Upscaling::MinRenderScale && clamped.textureBiasOffset == Upscaling::MinTextureBiasOffset,
        "The custom scale and the texture bias offset are clamped.");
    write(path, "ambient_occlusion=gtvbao\nocclusion_radius=10\nocclusion_strength=-1\n");
    const auto occlusion = loadSettings(path).ambientOcclusion;
    expect(!occlusion.on && occlusion.radiusMetres == 4 && occlusion.strength == 0,
        "An invalid toggle leaves GTVBAO off, and its radius and strength are clamped.");
    write(path, "soft_shadows=on\nsun_size=20\n");
    const auto shadows = loadSettings(path);
    expect(!shadows.softShadows.on && shadows.sun.diameterDegrees == 4, "An invalid toggle leaves soft shadows off, and the sun size is clamped.");
    write(path, "indirect_light=yes\nindirect_light_radius=0\nindirect_light_strength=9\n");
    const auto light = loadSettings(path).indirectLight;
    expect(!light.on && light.radius == 0.05f && light.strength == 3,
        "An invalid toggle leaves SSGI off, and its radius and strength are clamped.");

    const Settings upscaling{.upscaling = {.mode = Mode::Custom, .dlssPreset = Preset::K, .sharpness = 0.25f, .customRenderScale = 0.75f, .textureBiasOffset = -0.5f}};
    saveSettings(path, upscaling);
    expect(loadSettings(path) == upscaling, "Restart restores an upscaling mode with its custom scale and texture bias offset.");

    for (const auto& invalid : {Settings{.upscaling = {.upscaler = Upscaler::Count}}, Settings{.upscaling = {.mode = Mode::Count}},
             Settings{.upscaling = {.dlssPreset = static_cast<Preset>(7)}}, Settings{.upscaling = {.sharpness = std::numeric_limits<float>::quiet_NaN()}},
             Settings{.upscaling = {.sharpness = 2}}, Settings{.upscaling = {.customRenderScale = 2}}, Settings{.upscaling = {.textureBiasOffset = 3}},
             Settings{.frameGeneration = {.mode = FrameGenerationMode::Count}}, Settings{.sun = {.diameterDegrees = 0}},
             Settings{.globalIllumination = {.raysPerLightPixel = 3}}, Settings{.ambientOcclusion = {.radiusMetres = 0}},
             Settings{.ambientOcclusion = {.strength = 4}}, Settings{.indirectLight = {.radius = 5}}, Settings{.indirectLight = {.strength = -1}}})
        expect(saveRejected(path, chosen, invalid), "Invalid settings cannot replace valid saved ones.");
}

void effectPresets()
{
    Settings settings{.upscaling = {.mode = Mode::Performance}, .hdr = {.on = true}};
    settings.softShadows.on = true;
    settings.globalIllumination.raysPerLightPixel = 4;
    applyPreset(settings, EffectPreset::RayTraced);
    expect(presetOf(settings) == EffectPreset::RayTraced && !settings.softShadows.on && settings.volumetricFog.on
            && settings.volumetricFog.look == AtmosphereLook::Physical && settings.frameRate.unlocked,
        "The ray-traced preset switches the screen-space effects off, the fog on.");
    expect(settings.upscaling.mode == Mode::Performance && settings.hdr.on && settings.globalIllumination.raysPerLightPixel == 4,
        "A preset keeps the upscaler, HDR and the effects' own settings.");

    settings.indirectLight.on = true;
    expect(!presetOf(settings), "One effect switched otherwise matches no preset.");
}

class IniKeys final : public SettingVisitor
{
public:
    void feature(bool&, const FeatureInfo& info) override { keys.push_back(info.iniKey); }

    void toggle(bool&, const ToggleInfo& info) override { keys.push_back(info.iniKey); }

    void slider(float&, const SliderInfo& info) override { keys.push_back(info.iniKey); }

    void choice(int&, const ChoiceInfo& info) override { keys.push_back(info.iniKey); }

    std::vector<std::string_view> keys;
};

void featuresDescribed()
{
    for (const auto effect : AllEffects)
        expect(featureOf(effect).menuId && featureOf(effect).effect == effect, "Every lighting effect has a feature.");

    Settings settings;
    IniKeys described;
    describe(settings, described);
    std::ranges::sort(described.keys);
    expect(std::ranges::adjacent_find(described.keys) == described.keys.end(), "No two settings share a PhantomFX.ini key.");
}

// Custom settings with every lighting effect on: the presets never turn on a replacer with what it replaces.
void effectPlan()
{
    Settings all;
    all.rayTracedShadows.on = all.softShadows.on = all.sunContactShadows.on = true;
    all.rayTracedLights.on = all.lampContactShadows.on = all.lampSoftShadows.on = true;
    all.globalIllumination.on = all.ambientOcclusion.on = all.indirectLight.on = true;
    all.volumetricFog.on = true;

    const auto plan = planEffects(all, {});
    expect(plan.runs(Effect::RayTracedShadows) && plan.runs(Effect::RayTracedLights) && plan.runs(Effect::GlobalIllumination)
            && plan.runs(Effect::VolumetricFog),
        "The effects nothing replaces run.");

    for (const auto effect : {Effect::SoftShadows, Effect::SunContactShadows, Effect::LampContactShadows, Effect::LampSoftShadows, Effect::AmbientOcclusion,
             Effect::IndirectLight})
        expect(plan[effect] == EffectState::Replaced, "The ray-traced effects replace the soft and contact shadows, GTVBAO and SSGI.");

    Settings withoutTracedLights = all;
    withoutTracedLights.rayTracedLights.on = false;
    const auto withoutTracedLightsPlan = planEffects(withoutTracedLights, {});
    expect(withoutTracedLightsPlan.runs(Effect::LampContactShadows) && withoutTracedLightsPlan.runs(Effect::LampSoftShadows),
        "Only the ray-traced lights replace the lamps' contact and soft shadows.");

    LeftOut tracedShadowsLeftOut;
    tracedShadowsLeftOut.effects[Effect::RayTracedShadows] = true;
    const auto measured = planEffects(all, {.leftOut = tracedShadowsLeftOut});
    expect(measured[Effect::RayTracedShadows] == EffectState::LeftOut && measured[Effect::SoftShadows] == EffectState::Replaced
            && measured[Effect::SunContactShadows] == EffectState::Replaced,
        "Leaving out the ray-traced shadows keeps what they replace off.");

    expect(planEffects(Settings{}, {.enhancements = false})[Effect::SoftShadows] == EffectState::Off, "An effect switched off is off, held or not.");
    LeftOut softShadowsLeftOut;
    softShadowsLeftOut.effects[Effect::SoftShadows] = true;
    expect(planEffects(all, {.enhancements = false, .leftOut = softShadowsLeftOut})[Effect::SoftShadows] == EffectState::Held, "Held comes before replaced and left out.");
    expect(planEffects(all, {.leftOut = softShadowsLeftOut})[Effect::SoftShadows] == EffectState::Replaced, "Replaced comes before left out.");

    Settings softAndGlobal;
    softAndGlobal.softShadows.on = softAndGlobal.globalIllumination.on = true;
    const auto unsupported = planEffects(softAndGlobal, {.supportedBuild = false});
    expect(unsupported.runs(Effect::SoftShadows) && unsupported[Effect::GlobalIllumination] == EffectState::Unsupported && !unsupported.tracesScene(),
        "An unsupported game build stops the ray tracing, not the soft shadows.");
    expect(planEffects(softAndGlobal, {.supportedBuild = false, .enhancements = false})[Effect::GlobalIllumination] == EffectState::Unsupported,
        "Unsupported comes before held.");
}

// A frame keeps the settings it started with.
void frameLatch()
{
    FrameLatch<int> latch;
    latch.set(1);
    latch.present();
    latch.set(2);
    expect(latch.frame() == 1 && latch.latest() == 2, "A change waits for the next present.");
    expect(latch.present() == 2 && latch.frame() == 2, "Present starts the next frame with the latest settings.");
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

void fill(game_constants::SceneConstants& scene, const float (&view)[4][4], const float eye[3])
{
    scene = {};
    const float projection[4][4]{{-1.75f, 0, 0, 0}, {0, 28.0f / 9, 0, 0}, {0, 0, A, B}, {0, 0, 1, 0}};
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
        {
            scene.projection[row][column] = projection[row][column];
            scene.view[row][column] = view[row][column];
            for (int k = 0; k < 4; ++k) scene.projectionView[row][column] += projection[row][k] * view[k][column];
        }

    for (int i = 0; i < 3; ++i) scene.eye[i] = eye[i];
    scene.eye[3] = 1;
    scene.width = 1920;
    scene.height = 1080;
    scene.nearPlane = 0.069f;
    scene.farPlane = 4000;
}

// The main view's scene constants (vs b2) in frame 390038 of the frame log (2560x1440), as it printed
// them: to six significant digits.
constexpr game_constants::SceneConstants DumpedMainView{
    .projectionView = {{-0.0166166f, 0, -2.4142f, -4059.87f}, {-1.12057f, 4.14315f, 0.00771272f, -449.885f},
        {1.20663e-05f, 3.26366e-06f, -8.30504e-08f, 0.0502964f}, {-0.965292f, -0.261089f, 0.00664395f, -23.6653f}},
    .projection = {{-2.41426f, 0, 0, 0}, {0, 4.29202f, 0, 0}, {0, 0, -1.25002e-05f, 0.0500006f}, {0, 0, 1, 0}},
    .view = {{0.00688267f, 0, 0.999976f, 1681.62f}, {-0.261083f, 0.965315f, 0.00179699f, -104.819f},
        {-0.965292f, -0.261089f, 0.00664395f, -23.6653f}, {0, 0, 0, 1}},
    .eye = {-61.7844f, 95.0046f, -1681.23f, 1},
    .width = 2560,
    .height = 1440,
    .nearPlane = 0.05f,
    .farPlane = 4000,
};

// Pixel position (x right, y down) of a world point through the scene's projection-view.
std::array<double, 2> project(const game_constants::SceneConstants& scene, const double (&point)[3])
{
    const auto& m = scene.projectionView;
    double clip[4]{};
    for (int row = 0; row < 4; ++row) clip[row] = m[row][0] * point[0] + m[row][1] * point[1] + m[row][2] * point[2] + m[row][3];

    return {(clip[0] / clip[3] + 1) * 960, (1 - clip[1] / clip[3]) * 540};
}

void mainViewRecognised()
{
    constexpr float eye[3]{93.75f, 314.25f, 244.15f};
    game_constants::SceneConstants scene;
    fill(scene, ViewRows, eye);
    expect(camera::classify(scene) == camera::SceneView::MainCamera, "The main view's upload is the camera.");

    auto motionBlur = scene;
    motionBlur.view[0][3] += 5; // MotionBlurCameraVelocity keeps the previous projection-view in view
    expect(camera::classify(motionBlur) == camera::SceneView::MainProjection, "Passes with other matrices in view still get the jitter.");

    auto shadow = scene;
    shadow.projection[3][2] = 0;
    shadow.projection[3][3] = 1;
    expect(camera::classify(shadow) == camera::SceneView::Other, "Orthographic shadow views are left alone.");

    auto square = scene;
    square.projection[0][0] = -square.projection[1][1];
    expect(camera::classify(square) == camera::SceneView::Other, "A square perspective view is not the main camera.");

    auto jittered = scene;
    camera::applyJitter(jittered, {0.001f, 0.001f});
    expect(camera::classify(jittered) == camera::SceneView::Other, "An upload is never jittered twice.");
}

// The numbers the game wrote, which the frame log rounds to six digits, stay within the classifier's
// tolerance.
void dumpedMainViewIsTheMainCamera()
{
    expect(camera::classify(DumpedMainView) == camera::SceneView::MainCamera && camera::isMainCamera(DumpedMainView),
        "The main view the game wrote is the main camera.");
}

// The ray tracing notes the main camera's scene constants as the upscaler leaves them: jittered.
void jitteredMainViewIsStillTheMainCamera()
{
    constexpr float eye[3]{93.75f, 314.25f, 244.15f};
    game_constants::SceneConstants scene;
    fill(scene, ViewRows, eye);
    auto jittered = scene;
    camera::applyJitter(jittered, camera::jitterNdc({0.25f, -0.375f}, 1920, 1080));
    auto dumped = DumpedMainView;
    camera::applyJitter(dumped, camera::jitterNdc({-0.5f, 0.5f}, 2560, 1440));
    expect(camera::isMainCamera(jittered) && camera::isMainCamera(dumped), "The main camera, jittered, is still the main camera.");

    auto motionBlur = jittered;
    motionBlur.view[0][3] += 5;
    expect(!camera::isMainCamera(motionBlur), "A jittered pass with other matrices is not the main camera.");
}

// A sun pass's constants in frame 21132 of the frame log (ps b2 and b3), at daytime.
void sunPassDirectionPointsToTheSun()
{
    constexpr game_constants::SceneConstants scene{
        .projectionView = {{-1.49411f, 0, -0.911126f, 2413.01f}, {0.517112f, 2.94831f, -0.847983f, 949.309f},
            {8.51127e-06f, -5.50713e-06f, -1.39572e-05f, 0.099199f}, {-0.493399f, 0.319248f, 0.809097f, -1750.57f}},
        .projection = {{-1.75f, 0, 0, 0}, {0, 3.11111f, 0, 0}, {0, 0, -1.72503e-05f, 0.0690012f}, {0, 0, 1, 0}},
        .view = {{0.853774f, 0.166214f, -0.493399f, 262.796f}, {-1.49012e-08f, 0.947671f, 0.319248f, 269.697f},
            {0.520643f, -0.272566f, 0.809097f, 2217.44f}, {0, 0, 0, 1}},
        .eye = {262.796f, 269.697f, 2217.44f, 1},
    };
    constexpr game_constants::SunLightConstants light{.direction = {0.592003f, 0.597131f, 0.541264f}};

    const auto eye = game_constants::toWorld(scene, {0, 0, 0}, 1);
    expect(close(eye[0], scene.eye[0], 1e-3) && close(eye[1], scene.eye[1], 1e-3) && close(eye[2], scene.eye[2], 1e-3),
        "A light pass's view space has the camera at its origin.");

    // Back to view space through the pass's own camera: P x V's rows 0, 1 and 3 are P00 V0, P11 V1 and V2.
    const auto sun = game_constants::toWorld(scene, light.direction, 0);
    const auto& m = scene.projectionView;
    const double scales[3]{scene.projection[0][0], scene.projection[1][1], 1};
    const int rows[3]{0, 1, 3};
    for (int i = 0; i < 3; ++i)
    {
        const auto& row = m[rows[i]];
        const double view = (row[0] * sun[0] + row[1] * sun[1] + row[2] * sun[2]) / scales[i];
        expect(close(view, light.direction[i], 1e-3), "The sun's way in the world goes back to the pass's view space through its camera.");
    }

    expect(sun[1] > 0.5f, "The daytime sun is above the horizon.");
}

void jitterShiftsTheImage()
{
    constexpr float eye[3]{93.75f, 314.25f, 244.15f};
    game_constants::SceneConstants scene;
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
    expect(jittered.cameraCenterOffset[0] == camera::jitterNdc(pixels, 1920, 1080)[0]
            && jittered.cameraCenterOffset[1] == camera::jitterNdc(pixels, 1920, 1080)[1],
        "cameraCenterOffset carries the NDC offset the screen-space passes subtract.");
    expect(camera::viewOf(scene).projectionView == view.projectionView, "The camera is captured before the jitter.");
}

void reprojection()
{
    constexpr float eye[3]{93.75f, 314.25f, 244.15f};
    game_constants::SceneConstants scene;
    fill(scene, ViewRows, eye);
    const auto view = camera::viewOf(scene);
    const auto still = camera::previousFromCurrent(view, view);
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            expect(close(still[row * 4 + column], row == column ? 1 : 0, 1e-9), "A camera that stands still has no motion.");

    // Previous frame: the camera 0.2 m to the side, so a point straight ahead was elsewhere on screen.
    game_constants::SceneConstants before;
    const float previousEye[3]{eye[0] - 0.2f * ViewRows[0][0], eye[1] - 0.2f * ViewRows[0][1], eye[2] - 0.2f * ViewRows[0][2]};
    float previousRows[4][4];
    std::memcpy(previousRows, ViewRows, sizeof(previousRows));
    for (int row = 0; row < 3; ++row)
        previousRows[row][3] = -(ViewRows[row][0] * previousEye[0] + ViewRows[row][1] * previousEye[1] + ViewRows[row][2] * previousEye[2]);

    fill(before, previousRows, previousEye);
    const auto moved = camera::previousFromCurrent(camera::viewOf(before), view);

    const double point[3]{eye[0] + 10.0 * ViewRows[2][0], eye[1] + 10.0 * ViewRows[2][1], eye[2] + 10.0 * ViewRows[2][2]};
    const auto now = project(scene, point), then = project(before, point);
    const auto& m = scene.projectionView;
    double clip[4]{};
    for (int row = 0; row < 4; ++row) clip[row] = m[row][0] * point[0] + m[row][1] * point[1] + m[row][2] * point[2] + m[row][3];

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
    game_constants::SceneConstants scene;
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

// DLSS Frame Generation reprojects with these, so they must describe the camera the engine draws with.
void streamlineCamera()
{
    // The eye the view's translation puts the camera at; the recorded one is rounded by millimetres.
    float eye[3]{};
    for (int i = 0; i < 3; ++i)
        for (int row = 0; row < 3; ++row) eye[i] -= ViewRows[row][i] * ViewRows[row][3];

    game_constants::SceneConstants scene;
    fill(scene, ViewRows, eye);
    const auto view = camera::viewOf(scene);
    const auto still = camera::streamlineCamera(view, view);

    // A point ahead, to the side and above: into Streamline's view space along the camera's axes from
    // its position, then v x viewToClip, lands on the pixel the engine draws it on.
    double point[3]{};
    for (int i = 0; i < 3; ++i) point[i] = eye[i] + 10.0 * ViewRows[2][i] + 2.0 * ViewRows[0][i] + 1.0 * ViewRows[1][i];
    double local[4]{0, 0, 0, 1};
    for (int i = 0; i < 3; ++i)
    {
        const double offset = point[i] - still.position[i];
        local[0] += offset * still.right[i];
        local[1] += offset * still.up[i];
        local[2] += offset * still.forward[i];
    }

    double clip[4]{};
    for (int column = 0; column < 4; ++column)
        for (int k = 0; k < 4; ++k) clip[column] += local[k] * still.viewToClip[k * 4 + column];

    const auto engine = project(scene, point);
    expect(close((clip[0] / clip[3] + 1) * 960, engine[0], 0.05) && close((1 - clip[1] / clip[3]) * 540, engine[1], 0.05),
        "Streamline's camera puts a point on the pixel the engine draws it on.");
    expect(engine[0] < 960 && local[0] < 0 && engine[1] < 540 && local[1] > 0,
        "A point the engine draws left of and above the centre is left of and above it in Streamline's view space.");
    expect(still.viewToClip[0] > 0 && still.viewToClip[5] > 0 && still.viewToClip[11] == 1,
        "The projection is the usual one: x right, y up, w = z.");

    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
        {
            double product = 0;
            for (int k = 0; k < 4; ++k) product += still.clipToView[row * 4 + k] * still.viewToClip[k * 4 + column];
            expect(close(product, row == column ? 1 : 0, 1e-4), "clipToView inverts viewToClip.");
            expect(close(still.clipToPreviousClip[row * 4 + column], row == column ? 1 : 0, 1e-6)
                    && close(still.previousClipToClip[row * 4 + column], row == column ? 1 : 0, 1e-6),
                "A camera that stands still maps clip space onto itself.");
        }

    expect(close(still.verticalFov, 2 * std::atan(9.0 / 28)) && close(still.aspectRatio, 16.0 / 9)
            && still.nearPlane == 0.069f && still.farPlane == 4000,
        "The field of view, aspect ratio and planes are the engine's.");
}

void modeMaths()
{
    expect(!upscales(Mode::Off) && !upscales(Mode::Dlaa) && upscales(Mode::Quality) && upscales(Mode::Custom),
        "Only the modes below native resolution upscale.");
    constexpr auto Dlss = Upscaler::Dlss, Fsr = Upscaler::Fsr;
    expect(close(nominalScale(Dlss, Mode::Quality, 0), 2.0 / 3) && close(nominalScale(Dlss, Mode::Balanced, 0), 0.58)
            && close(nominalScale(Dlss, Mode::Performance, 0), 0.5) && close(nominalScale(Dlss, Mode::UltraPerformance, 0), 1.0 / 3)
            && nominalScale(Dlss, Mode::Dlaa, 0.5f) == 1,
        "DLSS's modes keep NVIDIA's render scales.");
    expect(close(nominalScale(Fsr, Mode::Quality, 0), 1 / 1.5) && close(nominalScale(Fsr, Mode::Balanced, 0), 1 / 1.7)
            && close(nominalScale(Fsr, Mode::Performance, 0), 0.5) && close(nominalScale(Fsr, Mode::UltraPerformance, 0), 1.0 / 3)
            && nominalScale(Fsr, Mode::Dlaa, 0.5f) == 1,
        "FSR's modes keep AMD's render scales.");
    expect(nominalScale(Dlss, Mode::Custom, 0.8f) == 0.8f && nominalScale(Fsr, Mode::Custom, 0.1f) == Upscaling::MinRenderScale
            && nominalScale(Dlss, Mode::Custom, std::numeric_limits<float>::quiet_NaN()) == Upscaling{}.customRenderScale,
        "Custom uses its scale within the supported range.");

    expect(mipBias(1, 0) == 0 && close(mipBias(0.5f, 0), -1) && close(mipBias(2.0f / 3, 0), std::log2(2.0 / 3)),
        "The texture LOD bias is log2(render / output).");
    expect(close(mipBias(0.5f, 0.25f), -0.75) && close(mipBias(0.5f, -9), -1 + Upscaling::MinTextureBiasOffset) && mipBias(0, 0) == 0,
        "The offset is added within its range, and invalid scales give no bias.");

    expect(multiplier(FrameGenerationMode::Off) == 0 && multiplier(FrameGenerationMode::X2) == 2 && multiplier(FrameGenerationMode::X6) == 6
            && multiplier(FrameGenerationMode::Dynamic) == 0 && multiplier(FrameGenerationMode::Fsr) == 0,
        "Fixed DLSS frame generation modes present 2 to 6 frames per rendered frame.");
}

void dlssModesTakeDlssSize()
{
    const auto plan = planRenderSize({.mode = Mode::Balanced}, {3840, 2160}, render_scale::Size{2226, 1252});
    expect(plan.render == render_scale::Size{2226, 1252} && plan.output == render_scale::Size{3840, 2160} && plan.quality == DlssQuality::Balanced,
        "DLSS's modes render at DLSS's own size, with its quality.");
}

void nominalSizeWithoutDlssSize()
{
    constexpr render_scale::Size Output{3840, 2160}, FromDlss{2400, 1350}, TwoThirds{2560, 1440};
    expect(planRenderSize({.upscaler = Upscaler::Fsr, .mode = Mode::Quality}, Output, FromDlss).render == TwoThirds, "FSR keeps AMD's scale.");
    expect(planRenderSize({.mode = Mode::Custom, .customRenderScale = 0.75f}, Output, FromDlss).render == render_scale::Size{2880, 1620},
        "Custom keeps its own scale.");
    expect(planRenderSize({.mode = Mode::Quality}, Output, std::nullopt).render == TwoThirds
            && planRenderSize({.mode = Mode::Quality}, Output, render_scale::Size{0, 0}).render == TwoThirds,
        "DLSS without a size from NGX, or with a zero one, keeps NVIDIA's scale.");
}

// The engine rounds its own sizes up to even (0x2496A0).
void oddSizesRoundUpToEven()
{
    expect(planRenderSize({.mode = Mode::Quality}, {3840, 2160}, render_scale::Size{2227, 1253}).render == render_scale::Size{2228, 1254},
        "An odd render size is rounded up to even.");
}

void renderSizeNeverAboveOutput()
{
    const auto fromDlss = planRenderSize({.mode = Mode::Quality}, {1920, 1080}, render_scale::Size{2000, 1200}).render;
    const auto full = planRenderSize({.mode = Mode::Custom, .customRenderScale = 1}, {1919, 1079}, std::nullopt).render;
    expect(fromDlss == render_scale::Size{1920, 1080} && full.width <= 1919 && full.height <= 1079,
        "The render size never exceeds the output size, rounded up or not.");
}

// Custom asks DLSS for DLAA from 99%, Quality from 66%, Balanced from 58% and Performance from 50%.
void customQualityFollowsScale()
{
    using enum DlssQuality;
    const std::pair<float, DlssQuality> cases[]{{1.0f, Dlaa},{0.99f, Dlaa}, {0.98f, MaxQuality}, {0.66f, MaxQuality}, {0.65f, Balanced}, {0.58f, Balanced},
        {0.57f, MaxPerformance}, {0.5f, MaxPerformance}, {0.49f, UltraPerformance}};
    for (const auto& [scale, quality] : cases) expect(dlssQuality(Mode::Custom, scale) == quality, "Custom's DLSS quality follows its scale.");
}

// Anything but what decides the render size would rebuild the engine's targets for nothing.
void planKeptForOtherSettings()
{
    const Upscaling wanted{.mode = Mode::Custom, .customRenderScale = 0.75f};
    constexpr render_scale::Size Output{2560, 1440};
    const auto plan = planRenderSize(wanted, Output, std::nullopt);
    auto other = wanted;
    other.dlssPreset = Preset::K;
    other.sharpness = 1;
    other.textureBiasOffset = -1;
    expect(plan.isFor(other, Output), "The preset, sharpening and the texture bias keep the plan.");

    auto fsr = wanted, quality = wanted, scaled = wanted;
    fsr.upscaler = Upscaler::Fsr;
    quality.mode = Mode::Quality;
    scaled.customRenderScale = 0.7f;
    expect(!plan.isFor(fsr, Output) && !plan.isFor(quality, Output) && !plan.isFor(scaled, Output) && !plan.isFor(wanted, {1920, 1080}),
        "Another upscaler, mode, custom scale or output size makes a new plan.");
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

// A scene constants upload as bufferUnmapped hands it to the frames; returns it as the game's draws
// then read it.
game_constants::SceneConstants upload(Frames& frames, game_constants::SceneConstants scene, const FrameStart& start = {.canJitter = true})
{
    frames.sceneWritten(scene, camera::classify(scene), start);
    return scene;
}

// The frame being run: executed, upscaled at its grading, presented.
void upscaledAndPresented(Frames& frames)
{
    frames.executed();
    frames.upscaled(true);
    frames.presented(true);
}

void frameStartsAfterAnExecute()
{
    Frames frames;
    upload(frames, DumpedMainView, {.gameFrame = 7});
    upload(frames, DumpedMainView, {.gameFrame = 8});
    expect(frames.current().index == 1 && frames.current().gameFrame == 7, "A frame starts at its first main-view upload, with the game loop's frame then.");

    frames.executed();
    upload(frames, DumpedMainView, {.gameFrame = 9});
    expect(frames.current().index == 2 && frames.current().gameFrame == 9, "The first main-view upload after an execute starts the next frame.");
}

// The FXAA pass and the object velocity note the frame they were recorded for.
void passesAfterAnExecuteBelongToTheNextFrame()
{
    Frames frames;
    upload(frames, DumpedMainView);
    expect(frames.recordingFrame() == 1, "Passes recorded after a frame's first upload are its own.");

    frames.executed();
    expect(frames.recordingFrame() == 2 && frames.current().index == 1, "Passes recorded after an execute, before the next upload, are the next frame's.");
}

// The intro draws a smaller view first: a mirror's, 1080x608 of a 2560x1440 frame (2026-10-26).
void mainViewIsTheLargestCamera()
{
    constexpr float eye[3]{93.75f, 314.25f, 244.15f};
    game_constants::SceneConstants main;
    fill(main, ViewRows, eye);
    main.width = 2560;
    main.height = 1440;
    auto smaller = DumpedMainView;
    smaller.width = 1080;
    smaller.height = 608;

    Frames frames;
    upload(frames, smaller);
    expect(frames.current().view && frames.current().view->width == 1080, "A smaller view is the main view until a larger one.");

    upload(frames, main);
    upload(frames, DumpedMainView);
    const auto& view = frames.current().view;
    expect(view && view->width == 2560 && view->projectionView == camera::viewOf(main).projectionView,
        "The main view is the frame's largest camera; of two the same size, the first.");
}

// Shadow maps' views are orthographic.
void otherViewsNeitherStartNorJitter()
{
    Frames frames;
    upload(frames, DumpedMainView);
    upscaledAndPresented(frames);
    auto shadow = DumpedMainView;
    shadow.projection[3][2] = 0;
    shadow.projection[3][3] = 1;
    const auto written = upload(frames, shadow);
    expect(frames.current().index == 1 && std::memcmp(&written, &shadow, sizeof(shadow)) == 0, "A shadow view neither starts a frame nor gets the jitter.");
}

// The motion blur's velocity pass has the previous frame's matrices in its view, and is jittered as well.
void everyMainViewUploadGetsTheFramesJitter()
{
    Frames frames;
    upload(frames, DumpedMainView);
    upscaledAndPresented(frames);
    auto motionBlur = DumpedMainView;
    motionBlur.view[0][3] += 5;
    const auto main = upload(frames, DumpedMainView), velocity = upload(frames, motionBlur);
    const auto ndc = camera::jitterNdc(frames.current().jitter, 2560, 1440);
    expect(ndc[0] != 0 && ndc[1] != 0 && main.cameraCenterOffset[0] == ndc[0] && main.cameraCenterOffset[1] == ndc[1]
            && velocity.cameraCenterOffset[0] == ndc[0] && velocity.cameraCenterOffset[1] == ndc[1],
        "Every main-view upload of a jittered frame, the camera's and the motion blur's, gets the frame's jitter.");
}

// So a 3D view without FXAA never shakes.
void jitterOnlyAfterAnUpscaledFrame()
{
    struct Case
    {
        bool shouldJitterNext = true, wasUpscaled = true, canJitter = true;
    };
    // The frame after one that ran, went through the upscaler at its grading, and was presented.
    const auto isNextFrameJittered = [](Case run)
    {
        Frames frames;
        upload(frames, DumpedMainView);
        frames.executed();
        frames.upscaled(run.shouldJitterNext);
        frames.presented(run.wasUpscaled);
        upload(frames, DumpedMainView, {.canJitter = run.canJitter});
        return frames.current().isJittered;
    };
    expect(isNextFrameJittered({}), "The frame after an upscaled one is jittered.");
    expect(!isNextFrameJittered({.wasUpscaled = false}), "A present without an upscale leaves the next frame unjittered.");
    expect(!isNextFrameJittered({.canJitter = false}), "Nothing is jittered while the upscaler does not replace FXAA.");
    expect(!isNextFrameJittered({.shouldJitterNext = false}), "Diagnostics' Jitter off, or a fallback, leaves the next frame unjittered.");
}

void photoModeSettingsPersist(const fs::path& root)
{
    const auto path = root / L"PhantomFX-photo-mode.ini";
    const Settings chosen{.photoMode = {.fNumber = 2.8f}};
    saveSettings(path, chosen);
    expect(loadSettings(path) == chosen, "Restart restores photo mode's f-stop.");

    write(path, "f_number=0.1\n");
    expect(loadSettings(path).photoMode.fNumber == 0.7f, "An f-stop out of range is clamped.");

    expect(saveRejected(path, chosen, Settings{.photoMode = {.fNumber = 64}}), "An invalid f-stop cannot replace a valid saved one.");
}

// The engine's motion blur toggle and strength persist, and the strength is clamped.
void motionBlurSettingsPersist(const fs::path& root)
{
    const auto path = root / L"PhantomFX-motion-blur.ini";
    const Settings chosen{.motionBlur = {.on = false, .strength = 1.5f}};
    saveSettings(path, chosen);
    expect(loadSettings(path) == chosen, "Restart restores the motion blur toggle and its strength.");

    write(path, "motion_blur_strength=9\n");
    expect(loadSettings(path).motionBlur.strength == 2, "The motion blur strength is clamped.");
}

void hdrSettingsPersist(const fs::path& root)
{
    const auto path = root / L"PhantomFX-hdr.ini";
    const Settings chosen{.hdr = {.on = true, .peakNits = 1500, .paperWhiteNits = 250}};
    saveSettings(path, chosen);
    expect(loadSettings(path) == chosen, "Restart restores HDR output, the peak brightness and the paper white.");

    write(path, "hdr=yes\nhdr_peak_nits=50000\nhdr_paper_white_nits=10\n");
    const auto read = loadSettings(path).hdr;
    expect(!read.on && read.peakNits == 10000 && read.paperWhiteNits == 80,
        "An invalid toggle leaves HDR off, and the peak brightness and paper white are clamped.");
    write(path, "hdr_peak_nits=0\n");
    expect(loadSettings(path).hdr.peakNits == 0, "A peak brightness of 0 keeps the display's own.");

    for (const auto& invalid : {Settings{.hdr = {.peakNits = 100}}, Settings{.hdr = {.peakNits = std::numeric_limits<float>::quiet_NaN()}},
             Settings{.hdr = {.paperWhiteNits = std::numeric_limits<float>::infinity()}}})
        expect(saveRejected(path, chosen, invalid), "Invalid HDR settings cannot replace valid saved ones.");
}

// HDR clamps the 2D pass's writes by patching its bytecode, which Direct3D takes only with a correct
// checksum. Shaders of different lengths exercise both forms of the checksum's last block.
void outputSaturation()
{
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Device> device;
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, device.GetAddressOf(), nullptr, nullptr);
    std::set<std::size_t> lastBlocks;
    std::string body = "float4 c = color;";
    for (int terms = 1; terms <= 24; ++terms)
    {
        body += " c = c * " + std::to_string(terms + 1) + ".5 + " + std::to_string(terms) + ";";
        const auto source = "float4 main(float4 color : COLOR0) : SV_Target { " + body + " return c; }";
        ComPtr<ID3DBlob> code, errors;
        expect(SUCCEEDED(D3DCompile(source.data(), source.size(), "test", nullptr, nullptr, "main", "ps_5_0", 0, 0, code.GetAddressOf(),
                   errors.GetAddressOf())),
            "Compile the test shader.");
        const auto* bytes = code->GetBufferPointer();
        const auto size = code->GetBufferSize();
        expect(dxbcChecksum(bytes, size) == *dxbcHashOf(bytes, size), "The checksum matches the compiler's.");
        lastBlocks.insert((size - 20) % 64 < 56);

        // The first multiply's literal is found among the operands (the HDR tonemap patch finds its
        // literals this way).
        const auto* tokens = static_cast<const std::uint8_t*>(bytes);
        const auto program = instructionsOf(tokens, size);
        bool literalFound{};
        for (const auto& instruction : program.value_or(std::vector<Instruction>{}))
            for (const auto offset : immediatesOf(tokens, instruction))
            {
                float value{};
                std::memcpy(&value, tokens + offset, sizeof(value));
                literalFound = literalFound || value == 2.5f;
            }
        // The compiler folds later terms together, so only the first shader surely keeps 2.5.
        expect(program && (literalFound || terms > 1), "The program's instructions and their literals are read.");

        const auto patched = saturateOutput(bytes, size);
        expect(patched && dxbcChecksum(patched->data(), patched->size()) == *dxbcHashOf(patched->data(), patched->size()),
            "The patched shader carries its new checksum.");
        ComPtr<ID3DBlob> text;
        expect(SUCCEEDED(D3DDisassemble(patched->data(), patched->size(), 0, nullptr, text.GetAddressOf()))
                && std::string_view(static_cast<const char*>(text->GetBufferPointer()), text->GetBufferSize()).find("_sat o0") != std::string_view::npos,
            "The write to the render target is saturated.");

        ComPtr<ID3D11PixelShader> shader;
        expect(!device || SUCCEEDED(device->CreatePixelShader(patched->data(), patched->size(), nullptr, shader.GetAddressOf())),
            "Direct3D takes the patched shader.");
    }

    expect(lastBlocks.size() == 2, "The shaders end in both forms of the checksum's last block.");
}

Microsoft::WRL::ComPtr<ID3D11Device> warpDevice()
{
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    expect(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, device.GetAddressOf(), nullptr,
               nullptr)),
        "Create a WARP device.");

    return device;
}

// The constant capture keeps writes by buffer address, so the buffers live until the tests end: a new
// buffer at a released one's address would read the old one's last write.
std::vector<Microsoft::WRL::ComPtr<ID3D11Buffer>> captureBuffers;

ID3D11Buffer* constantBuffer(ID3D11Device* device, UINT size)
{
    const D3D11_BUFFER_DESC desc{size, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE};
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
    expect(SUCCEEDED(device->CreateBuffer(&desc, nullptr, buffer.GetAddressOf())), "Create a constant buffer.");
    captureBuffers.push_back(buffer);

    return buffer.Get();
}

// What the game's Map and Unmap of `buffer` show the capture.
template<class C> void gameWrites(ID3D11Buffer* buffer, C constants)
{
    constant_capture::bufferMapped(buffer, &constants);
    constant_capture::bufferUnmapped(buffer);
}

// Each of the game's threads records its own command list, whose draws read what that thread wrote.
void writeIsReadOnItsOwnThreadOnly()
{
    const auto device = warpDevice();
    auto* buffer = constantBuffer(device.Get(), sizeof(game_constants::SceneConstants));
    constant_capture::keepLightPasses(true);
    gameWrites(buffer, DumpedMainView);
    const auto read = constant_capture::written<game_constants::SceneConstants>(buffer);
    expect(read && std::memcmp(&*read, &DumpedMainView, sizeof(DumpedMainView)) == 0, "A thread reads what it wrote.");

    bool readElsewhere = true;
    std::thread([&] { readElsewhere = constant_capture::written<game_constants::SceneConstants>(buffer).has_value(); }).join();
    expect(!readElsewhere, "Another thread reads nothing of it.");
    constant_capture::keepLightPasses(false);
}

void lightPassesKeepOnlySceneAndLight()
{
    const auto device = warpDevice();
    auto* scene = constantBuffer(device.Get(), sizeof(game_constants::SceneConstants));
    auto* light = constantBuffer(device.Get(), sizeof(game_constants::LampLightConstants));
    auto* object = constantBuffer(device.Get(), sizeof(game_constants::ObjectConstants));
    constant_capture::keepLightPasses(true);
    gameWrites(scene, DumpedMainView);
    gameWrites(light, game_constants::LampLightConstants{.position = {0, 2, 5}});
    gameWrites(object, game_constants::ObjectConstants{});
    expect(constant_capture::written<game_constants::SceneConstants>(scene) && constant_capture::written<game_constants::LampLightConstants>(light)
            && !constant_capture::written<game_constants::ObjectConstants>(object),
        "For the contact shadows, only the scene and light constants are kept.");

    constant_capture::keepLightPasses(false);
    auto* later = constantBuffer(device.Get(), sizeof(game_constants::SceneConstants));
    gameWrites(later, DumpedMainView);
    expect(!constant_capture::written<game_constants::SceneConstants>(later), "Nothing is kept while nothing reads it.");
}

// The ray tracing traces from the main camera's scene constants as the upscaler jittered them.
void mainCameraWriteNotedOnlyWhileAllKept()
{
    const auto device = warpDevice();
    auto* buffer = constantBuffer(device.Get(), sizeof(game_constants::SceneConstants));
    constant_capture::keepLightPasses(true);
    gameWrites(buffer, DumpedMainView);
    expect(!constant_capture::mainCameraWrite(buffer), "The main camera is noted only while all is kept.");

    constant_capture::keepAll(true);
    auto jittered = DumpedMainView;
    camera::applyJitter(jittered, camera::jitterNdc({0.25f, -0.375f}, 2560, 1440));
    gameWrites(buffer, jittered);
    const auto* noted = constant_capture::mainCameraWrite(buffer);
    expect(noted && noted->cameraCenterOffset[0] == jittered.cameraCenterOffset[0], "The main camera's write is noted as written, jittered.");

    auto shadow = DumpedMainView;
    shadow.projection[3][2] = 0;
    shadow.projection[3][3] = 1;
    gameWrites(buffer, shadow);
    expect(!constant_capture::mainCameraWrite(buffer), "Another view's write is not the main camera's.");
    constant_capture::keepAll(false);
    constant_capture::keepLightPasses(false);
}

// The ray-traced lights and the global illumination's sky and sun once read a write of any size, and
// with it another buffer's values.
void writeOfAnotherSizeReadsNothing()
{
    const auto device = warpDevice();
    auto* buffer = constantBuffer(device.Get(), sizeof(game_constants::SceneConstants));
    constant_capture::keepLightPasses(true);
    gameWrites(buffer, DumpedMainView);
    expect(!constant_capture::written<game_constants::SunLightConstants>(buffer) && !constant_capture::writtenTo<game_constants::ProbeConstants>(buffer),
        "A read of another size than the write reads nothing.");
    constant_capture::keepLightPasses(false);
}
}

void globalIlluminationSettingsPersist(const fs::path& root)
{
    const auto path = root / L"PhantomFX-gi.ini";
    const Settings chosen{.globalIllumination = {.on = true, .strength = 1.5f}};
    saveSettings(path, chosen);
    expect(loadSettings(path) == chosen, "Restart restores the global illumination and its strength.");
    write(path, "global_illumination_strength=99\n");
    expect(loadSettings(path).globalIllumination.strength == 3, "The strength is clamped.");
}

// The scene's geometry and acceleration structures live in large buffers handed out in ranges.
void rangesComeBack()
{
    RangeAllocator allocator(1000);
    const auto a = allocator.allocate(100), b = allocator.allocate(300), c = allocator.allocate(100);
    expect(a == 0u && b == 100u && c == 400u, "Ranges are handed out first fit, one after another.");
    allocator.release(*b, 300);
    expect(allocator.allocate(200) == 100u, "A released range is used again.");
    expect(allocator.allocate(64, 256) == 512u, "An aligned range starts at a multiple of the alignment.");
    expect(!allocator.allocate(1000), "What does not fit is refused.");

    allocator.release(*a, 100);
    allocator.release(100, 200);
    allocator.release(*c, 100);
    allocator.release(512, 64);
    expect(allocator.usedSize() == 0 && allocator.allocate(1000) == 0u, "Released neighbours merge into one range again.");
}

// Traced rays that reach the sky must light a surface nothing blocks as the game's sky light does.
void skyLightMatchesTheGame()
{
    // f(n) = 1 + 0.5 n.y + 0.3 n.y^2 with the game's c = (-n.x, -n.z, n.y, 1): c.z is n.y.
    std::array<std::array<float, 16>, 3> matrices{};
    for (auto& m : matrices)
    {
        m[15] = 1;
        m[11] = m[14] = 0.25f;
        m[10] = 0.3f;
    }

    constexpr float Exposure = 2;
    const auto radiance = gi_lighting::skyRadiance(matrices, Exposure);
    constexpr double Pi = 3.14159265358979323846;
    const std::array<std::array<float, 3>, 4> normals{{{0, 1, 0}, {0, -1, 0}, {1, 0, 0}, {0.6f, 0.8f, 0}}};
    for (const auto& n : normals)
    {
        // Irradiance over pi from the radiance, by brute force.
        double sum = 0;
        constexpr int Rings = 256, Segments = 512;
        for (int ring = 0; ring < Rings; ++ring)
        {
            const double theta = (ring + 0.5) * Pi / Rings;
            for (int segment = 0; segment < Segments; ++segment)
            {
                const double phi = (segment + 0.5) * 2 * Pi / Segments;
                const std::array<float, 3> w{static_cast<float>(std::sin(theta) * std::cos(phi)), static_cast<float>(std::sin(theta) * std::sin(phi)),
                    static_cast<float>(std::cos(theta))};
                const double cosine = std::max(0.0, double{n[0]} * w[0] + double{n[1]} * w[1] + double{n[2]} * w[2]);
                sum += gi_lighting::evaluate(radiance, w)[0] * cosine * std::sin(theta) * (Pi / Rings) * (2 * Pi / Segments);
            }
        }

        const double game = Exposure * (1 + 0.5 * n[1] + 0.3 * n[1] * n[1]);
        expect(std::abs(sum / Pi - game) < 0.01 * game, "An open surface gets the game's sky light from the traced sky.");
    }
}

int main()
{
    const auto root =
        fs::temp_directory_path() / (L"phantom-fx-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    try
    {
        fs::create_directories(root);
        settingsPersist(root);
        effectPresets();
        featuresDescribed();
        effectPlan();
        frameLatch();
        mainViewRecognised();
        dumpedMainViewIsTheMainCamera();
        jitteredMainViewIsStillTheMainCamera();
        sunPassDirectionPointsToTheSun();
        jitterShiftsTheImage();
        reprojection();
        cuts();
        streamlineCamera();
        modeMaths();
        dlssModesTakeDlssSize();
        nominalSizeWithoutDlssSize();
        oddSizesRoundUpToEven();
        renderSizeNeverAboveOutput();
        customQualityFollowsScale();
        planKeptForOtherSettings();
        jitterSequence();
        frameStartsAfterAnExecute();
        passesAfterAnExecuteBelongToTheNextFrame();
        mainViewIsTheLargestCamera();
        otherViewsNeitherStartNorJitter();
        everyMainViewUploadGetsTheFramesJitter();
        jitterOnlyAfterAnUpscaledFrame();
        photoModeSettingsPersist(root);
        hdrSettingsPersist(root);
        motionBlurSettingsPersist(root);
        outputSaturation();
        writeIsReadOnItsOwnThreadOnly();
        lightPassesKeepOnlySceneAndLight();
        mainCameraWriteNotedOnlyWhileAllKept();
        writeOfAnotherSizeReadsNothing();
        globalIlluminationSettingsPersist(root);
        rangesComeBack();
        skyLightMatchesTheGame();
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
