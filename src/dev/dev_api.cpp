// PhantomFX's side of the dev API (devkit/phantom_api.hpp): the parts of the automation that run inside its passes,
// as plain functions the DevKit add-on calls. Nothing here parses a command or runs a script; that is DevKit's.
#include "devkit/phantom_api.hpp"

#include "core.hpp"
#include "dev/game_shadow.hpp"
#include "dev/held_camera.hpp"
#include "dev/inspect_trace.hpp"
#include "dev/perf_record.hpp"
#include "dev/remaster/tool.hpp"
#include "features/global_illumination/river_light.hpp"
#include "game_loop.hpp"
#include "passes/ray_tracing/engine_scene.hpp"
#include "passes/ray_tracing/gi_inputs.hpp"
#include "passes/ray_tracing/gi_renderer.hpp"
#include "passes/ray_tracing/ray_tracing.hpp"
#include "resource_dump.hpp"
#include "weather.hpp"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace phantom_fx
{
namespace
{
// Text into the caller's buffer: its whole length back, as much as fits written.
std::size_t give(std::string_view text, char* out, std::size_t cap) noexcept
{
    if (out && cap)
    {
        const auto count = std::min(text.size(), cap - 1);
        std::memcpy(out, text.data(), count);
        out[count] = '\0';
    }

    return text.size();
}

std::filesystem::path pathOf(const char* utf8)
{
    const std::string_view text = utf8 ? utf8 : "";

    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

// PhantomFX.ini's value for `key`, if it has that line.
std::optional<std::string> iniValue(const Settings& settings, std::string_view key)
{
    std::istringstream lines(iniText(settings));
    for (std::string line; std::getline(lines, line);)
        if (line.starts_with(key) && line.size() > key.size() && line[key.size()] == '=') return line.substr(key.size() + 1);

    return std::nullopt;
}

// Whether two of a setting's values say the same: 1 and 1.000000 do.
bool isSameValue(std::string_view a, std::string_view b)
{
    if (a == b) return true;

    float first{}, second{};
    const auto [firstEnd, firstError] = std::from_chars(a.data(), a.data() + a.size(), first);
    const auto [secondEnd, secondError] = std::from_chars(b.data(), b.data() + b.size(), second);

    return firstError == std::errc{} && secondError == std::errc{} && firstEnd == a.data() + a.size() && secondEnd == b.data() + b.size() && first == second;
}

std::string_view trimmed(std::string_view text)
{
    const auto first = text.find_first_not_of(" \t");
    if (first == text.npos) return {};

    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

held_camera::Pose poseOf(const PhantomFXPose& pose)
{
    held_camera::Pose result;
    std::copy(std::begin(pose.position), std::end(pose.position), result.position);
    result.yaw = pose.yaw;
    result.pitch = pose.pitch;
    result.focalLength = pose.focalLength;

    return result;
}

bool probeTurnedOnView{};

void showDebugView(DebugView view)
{
    auto diagnostics = core::currentDiagnostics();
    diagnostics.debugView = view;
    core::applyDiagnostics(diagnostics);
}

std::uint32_t frame() { return game_loop::currentFrame(); }
std::size_t sceneStatus(char* out, std::size_t cap) { return give(ray_tracing::status(), out, cap); }
std::size_t sceneWork() { return engine_scene::waitingFiles() + gi_renderer::waitingWork(); }

std::size_t settingValue(const char* key, char* out, std::size_t cap)
{
    const auto value = iniValue(core::currentSettings(), key ? key : "");
    if (!value) return PhantomFXNone;

    return give(*value, out, cap);
}

int settingSet(const char* keyText, const char* valueText)
{
    const std::string_view key = trimmed(keyText ? keyText : ""), value = trimmed(valueText ? valueText : "");
    const auto settings = core::currentSettings();
    const auto current = iniValue(settings, key);
    if (!current) return 0;

    const auto changed = withLine(settings, key, value);
    core::applySettings(changed);

    // A test that changed nothing would read as costing nothing: asking for the value it has may be meant, a value
    // withLine turned down is not.
    if (changed != settings) return 1;

    return isSameValue(*current, value) ? 2 : 3;
}

int enhancements() { return core::enhancementsOn() ? 1 : 0; }

void setEnhancements(int isOn)
{
    if (core::enhancementsOn() != (isOn != 0)) core::setEnhancements(isOn != 0);
}

int diagnostic(const char* nameText, int value)
{
    const std::string_view name = nameText ? nameText : "";
    auto diagnostics = core::currentDiagnostics();
    const std::pair<std::string_view, bool*> flags[]{{"show_scene", &diagnostics.showTracedScene}, {"two_sided_shadows", &diagnostics.twoSidedShadows},
        {"no_jitter", &diagnostics.noJitter}};
    const auto* const flag = std::ranges::find(flags, name, &std::pair<std::string_view, bool*>::first);
    if (flag != std::end(flags)) *flag->second = value != 0;
    else if (name == "debug_view") diagnostics.debugView = static_cast<DebugView>(value);
    else if (name == "river_light")
    {
        river_light::setEnabled(value != 0);
        return 1;
    }
    else return 0;

    core::applyDiagnostics(diagnostics);

    return 1;
}

void probeAt(std::uint32_t x, std::uint32_t y)
{
    ray_tracing::probeAt(x, y);
    if (core::currentDiagnostics().debugView != DebugView::ShadowRays)
    {
        showDebugView(DebugView::ShadowRays);
        probeTurnedOnView = true;
    }
}

std::size_t probeText(char* out, std::size_t cap) { return give(ray_tracing::shadowProbe(), out, cap); }

void probeClear()
{
    ray_tracing::clearProbe();
    if (std::exchange(probeTurnedOnView, false)) showDebugView(DebugView::FinalImage);
}

int cameraLast(PhantomFXPose* out)
{
    const auto camera = held_camera::lastCamera();
    if (!camera || !out) return 0;

    std::copy(std::begin(camera->position), std::end(camera->position), out->position);
    out->yaw = camera->yaw;
    out->pitch = camera->pitch;
    out->focalLength = camera->focalLength;

    return 1;
}

void cameraHold(const PhantomFXPose* pose) { if (pose) held_camera::hold(poseOf(*pose)); }
void cameraMove(const PhantomFXPose* pose, int updates) { if (pose) held_camera::move(poseOf(*pose), updates); }
void cameraRelease() { held_camera::release(); }
int cameraState() { return !held_camera::isHeld() ? 0 : held_camera::isLive() ? 2 : 1; }
void cameraLive(int isLive) { held_camera::setLive(isLive != 0); }

void weatherState(PhantomFXWeather* out)
{
    if (!out) return;

    const auto sky = weather::state();
    out->available = sky.available ? 1 : 0;
    out->clock = sky.clock;
    out->requested = sky.requested ? static_cast<int>(*sky.requested) : -1;
    out->fogDensity = sky.fogDensity;
}

int weatherTypes() { return static_cast<int>(weather::Type::Count); }
void setClock(float seconds) { weather::setClock(seconds); }
void stopClock(int isStopped) { weather::stopClock(isStopped != 0); }

void forceWeather(int type, float fogDensity)
{
    if (type >= 0 && type < static_cast<int>(weather::Type::Count)) weather::force(static_cast<weather::Type>(type), fogDensity);
}

void releaseWeather() { weather::release(); }

void reveal(const char* whatText)
{
    const std::string_view what = whatText ? whatText : "";
    engine_scene::Reveal revealed;
    if (what == "all") revealed.everything = true;
    else if (what != "off")
    {
        std::istringstream hashes{std::string(what)};
        hashes >> std::hex;
        for (std::uint64_t hash{}; hashes >> hash;) revealed.hashes.push_back(hash);
    }

    engine_scene::setReveal(std::move(revealed));
}

void watchOwners(std::uint64_t hash) { engine_scene::watchOwners(hash); }

std::size_t watchedOwners(char* out, std::size_t cap)
{
    const auto text = engine_scene::watchedOwners();
    if (!text) return PhantomFXNone;

    return give(*text, out, cap);
}

void recordRigidDraws(int isOn) { gi_inputs::recordRigidDraws(isOn != 0); }

void takeRigidDraws(void (*each)(const PhantomFXRigidDraw*, void*), void* user)
{
    for (const auto& draw : gi_inputs::takeRigidDraws())
    {
        if (!each) continue;

        PhantomFXRigidDraw copy{{draw.position[0], draw.position[1], draw.position[2]}, draw.firstIndex, draw.indexCount};
        each(&copy, user);
    }
}

void inspectRequest(const char* file) { inspect_trace::request(pathOf(file)); }
std::uint64_t inspectFinished() { return inspect_trace::finished(); }
void perfCapture(const char* file, std::uint32_t frames) { perf_record::capture(pathOf(file), frames); }
void perfCancel() { perf_record::cancelCapture(); }
std::uint64_t perfFinished() { return perf_record::capturesFinished(); }
std::size_t perfLast(char* out, std::size_t cap) { return give(perf_record::lastCapture(), out, cap); }
int perfRecording() { return perf_record::isRecording() ? 1 : 0; }
void setPerfRecording(int isOn) { perf_record::setRecording(isOn != 0); }

int saveBackBuffer(ID3D11DeviceContext* context, ID3D11Resource* backBuffer, const char* file)
{
    return saveTexture(context, backBuffer, DXGI_FORMAT_UNKNOWN, pathOf(file)) ? 1 : 0;
}

void saveTextures(ID3D11DeviceContext* context, const char* folder)
{
    const auto path = pathOf(folder);
    ray_tracing::saveTextures(context, path);
    game_shadow::save(context, path / "game-shadow.dds");
}

void keepGameShadow(int isKept) { game_shadow::keep(isKept != 0); }
std::size_t remasterLine(const char* line, char* out, std::size_t cap) { return give(remaster::runLine(line ? line : ""), out, cap); }

const PhantomFXDevApi Api{sizeof(PhantomFXDevApi), PhantomFXDevApiVersion, &frame, &sceneStatus, &sceneWork, &settingValue, &settingSet, &enhancements,
    &setEnhancements, &diagnostic, &probeAt, &probeText, &probeClear, &cameraLast, &cameraHold, &cameraMove, &cameraRelease, &cameraState, &cameraLive,
    &weatherState, &weatherTypes, &setClock, &stopClock, &forceWeather, &releaseWeather, &reveal, &watchOwners, &watchedOwners, &recordRigidDraws,
    &takeRigidDraws, &inspectRequest, &inspectFinished, &perfCapture, &perfCancel, &perfFinished, &perfLast, &perfRecording, &setPerfRecording,
    &saveBackBuffer, &saveTextures, &keepGameShadow, &remasterLine};
}
}

extern "C" __declspec(dllexport) const PhantomFXDevApi* PhantomFXGetDevApi()
{
    return &phantom_fx::Api;
}
