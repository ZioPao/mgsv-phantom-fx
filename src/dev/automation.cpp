#include "dev/automation.hpp"

#include "core.hpp"
#include "dev/game_shadow.hpp"
#include "dev/held_camera.hpp"
#include "dev/inspect_trace.hpp"
#include "dev/perf_record.hpp"
#include "dev/remaster/tool.hpp"
#include "devkit/commands.hpp"
#include "devkit/game_lua.hpp"
#include "devkit/save_redirect.hpp"
#include "devkit/travel.hpp"
#include "features/global_illumination/river_light.hpp"
#include "fxui/fxui.hpp"
#include "game_memory.hpp"
#include "game_loop.hpp"
#include "log.hpp"
#include "passes/ray_tracing/engine_scene.hpp"
#include "passes/ray_tracing/gi_inputs.hpp"
#include "passes/ray_tracing/gi_renderer.hpp"
#include "passes/ray_tracing/ray_tracing.hpp"
#include "resource_dump.hpp"
#include "weather.hpp"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <deque>
#include <format>
#include <cmath>
#include <fstream>
#include <map>
#include <tuple>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace phantom_fx::automation
{
namespace
{
using Script = devkit::commands::Script;

// Presents a line waits for the render thread before it gives up: with frame generation several a game frame.
constexpr std::uint64_t InspectionFrames = 3600, SceneFrames = 3600;
// Presents the scene's work to do stays the same before it counts as made: a model that never gets room
// would otherwise hold a case up for SceneFrames.
constexpr std::uint64_t SettledFrames = 60;
// The GPU's frames a `perf` line times without saying.
constexpr std::uint32_t DefaultCaptureFrames = 120;
// Presents after a probe's pixel changes before its text has that pixel: a readback's frames in flight.
constexpr std::string_view ProbeWait = "wait 8";

std::atomic<bool> caseAsked{};
bool probeTurnedOnView{};

// The waits a line leaves: the lines after it run once these are done.
struct Waits
{
    std::optional<std::uint64_t> inspectionsBefore;
    std::uint64_t inspectionDeadline{};
    std::optional<std::uint64_t> capturesBefore;
    std::uint64_t captureDeadline{};
    std::optional<std::uint64_t> sceneDeadline;
    std::size_t sceneWork{};
    std::uint64_t sceneBusyAt{};
};
Waits waits;

void resetWaits()
{
    waits = {};
}

std::string_view trimmed(std::string_view text)
{
    const auto first = text.find_first_not_of(" \t");
    if (first == text.npos) return {};

    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

// What the scene has yet to make.
std::size_t sceneWork()
{
    return engine_scene::waitingFiles() + gi_renderer::waitingWork();
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

std::string text(const held_camera::Pose& pose)
{
    return std::format("{} {} {} {} {} {}", pose.position[0], pose.position[1], pose.position[2], pose.yaw, pose.pitch, pose.focalLength);
}

// A pose's five numbers; its focal length is read apart, as `move` puts its frames before it.
std::optional<held_camera::Pose> readPose(std::istream& numbers)
{
    held_camera::Pose pose;
    if (numbers >> pose.position[0] >> pose.position[1] >> pose.position[2] >> pose.yaw >> pose.pitch) return pose;

    return std::nullopt;
}

// An optional last number: the focal length, or 0 (the game camera's lens) without one.
float readFocalLength(std::istream& numbers)
{
    float millimetres{};

    return numbers >> millimetres ? millimetres : 0.0f;
}

std::string status()
{
    const auto camera = held_camera::lastCamera();
    auto line = std::format("frame {}, presented {}, {}, ", game_loop::currentFrame(), devkit::commands::presents(), !held_camera::isHeld() ? "running" : held_camera::isLive() ? "held live" : "frozen");
    line += camera ? "camera " + text(*camera) : std::string("no camera yet");

    return line + "\n" + ray_tracing::status();
}

// A case: the frame as traced with what the game itself says, as the game draws it, then the scene with
// everything it leaves out, to name what the game drew and it lacks. The game stays frozen for more
// questions. tools/automation/case.py reads it.
std::deque<std::string> caseLines(const std::string& name)
{
    const auto path = "cases/" + name;
    return {"freeze", "wait 30", "perf " + path + "/perf", "game_shadow 1", "wait 30", "state " + name, "inspect " + path + "/scene", "image " + path + "/traced", "textures " + path, "game_shadow 0",
        "enhancements 0", "wait 45", "image " + path + "/vanilla", "enhancements 1", "reveal all", "inspect " + path + "/revealed", "reveal off", "case_done " + name};
}

// What the game is at into the case's folder.
void writeState(const std::string& name)
{
    const auto caseFolder = devkit::commands::folder() / "cases" / name;
    std::error_code error;
    std::filesystem::create_directories(caseFolder, error);

    std::ofstream state(caseFolder / "state.txt", std::ios::trunc);
    const auto camera = held_camera::lastCamera();
    state << "name=" << name << "\nframe=" << game_loop::currentFrame() << "\ncamera=" << (camera ? text(*camera) : std::string("?")) << "\n";
    if (const auto outcome = devkit::game_lua::run(devkit::game_lua::StateScript, "=automation")) state << outcome->text << "\n";

    const auto sky = weather::state();
    state << "clock=" << sky.clock << "\nweather=" << (sky.requested ? static_cast<int>(*sky.requested) : -1) << " " << sky.fogDensity << "\n";
    state << "scene=" << ray_tracing::status() << "\n";
    std::filesystem::copy_file(devkit::commands::folder().parent_path() / "PhantomFX.ini", caseFolder / "PhantomFX.ini", std::filesystem::copy_options::overwrite_existing, error);
}

void setEnhancements(bool isOn)
{
    if (core::enhancementsOn() != isOn) core::setEnhancements(isOn);
}

void reveal(std::string_view what)
{
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

void setDiagnostic(Script& running, const std::string& rest)
{
    std::istringstream arguments(rest);
    std::string name;
    int value{};
    auto diagnostics = core::currentDiagnostics();
    const std::pair<std::string_view, bool*> flags[]{{"show_scene", &diagnostics.showTracedScene}, {"two_sided_shadows", &diagnostics.twoSidedShadows},
        {"no_jitter", &diagnostics.noJitter}};
    const bool isRead = static_cast<bool>(arguments >> name >> value);
    const auto* const flag = std::ranges::find(flags, name, &std::pair<std::string_view, bool*>::first);
    if (isRead && flag != std::end(flags)) *flag->second = value != 0;
    else if (isRead && name == "debug_view") diagnostics.debugView = static_cast<DebugView>(value);
    else if (isRead && name == "river_light") river_light::setEnabled(value != 0);
    else running.output += "error: diagnostic debug_view|show_scene|two_sided_shadows|no_jitter|river_light <value>\n";

    core::applyDiagnostics(diagnostics);
}

void showDebugView(DebugView view)
{
    auto diagnostics = core::currentDiagnostics();
    diagnostics.debugView = view;
    core::applyDiagnostics(diagnostics);
}

void run(Script& running, const std::string& line)
{
    std::istringstream words(line);
    std::string command;
    words >> command;
    std::string rest;
    std::getline(words >> std::ws, rest);
    const auto invert = [&](std::string_view off) { return rest != off; };

    if (command == "case")
    {
        const auto lines = caseLines(rest.empty() ? devkit::commands::timeNow() : rest);
        running.lines.insert(running.lines.begin(), lines.begin(), lines.end());
    }
    else if (command == "freeze")
    {
        if (const auto camera = held_camera::lastCamera()) held_camera::hold(*camera);
        else running.output += "error: no camera yet\n";
    }
    else if (command == "release")
    {
        held_camera::release();
        ray_tracing::clearProbe();
        if (std::exchange(probeTurnedOnView, false)) showDebugView(DebugView::FinalImage);
    }
    else if (command == "camera")
    {
        std::istringstream numbers(rest);
        if (auto pose = readPose(numbers))
        {
            pose->focalLength = readFocalLength(numbers);
            held_camera::hold(*pose);
        }
        else running.output += "error: camera <x> <y> <z> <yaw> <pitch> [<mm>]\n";
    }
    else if (command == "move")
    {
        std::istringstream numbers(rest);
        auto pose = readPose(numbers);
        int frames{};
        if (pose && numbers >> frames)
        {
            pose->focalLength = readFocalLength(numbers);
            held_camera::move(*pose, frames);
        }
        else running.output += "error: move <x> <y> <z> <yaw> <pitch> <frames> [<mm>]\n";
    }
    else if (command == "wait_scene")
    {
        waits.sceneDeadline = devkit::commands::presents() + SceneFrames;
        waits.sceneWork = sceneWork();
        waits.sceneBusyAt = devkit::commands::presents();
    }
    else if (command == "probe")
    {
        std::uint32_t x{}, y{};
        if (!(std::istringstream(rest) >> x >> y))
        {
            running.output += "error: probe <x> <y>\n";
            return;
        }

        ray_tracing::probeAt(x, y);
        if (core::currentDiagnostics().debugView != DebugView::ShadowRays)
        {
            showDebugView(DebugView::ShadowRays);
            probeTurnedOnView = true;
        }

        running.lines.push_front("probe_text");
        running.lines.emplace_front(ProbeWait);
    }
    else if (command == "probe_text") running.output += ray_tracing::shadowProbe() + "\n";
    else if (command == "owner" && !rest.empty())
    {
        std::uint64_t hash{};
        const auto text = trimmed(rest);
        if (const auto [stop, error] = std::from_chars(text.data(), text.data() + text.size(), hash, 16); error != std::errc{} || !hash)
            running.output += "error: owner <model hash, hex>\n";
        else
        {
            engine_scene::watchOwners(hash);
            running.lines.push_front("owner_text");
            running.lines.emplace_front("wait 10");
        }
    }
    else if (command == "peek" && !rest.empty())
    {
        // `game+` takes an offset from the executable's base.
        std::string where;
        std::size_t length = 256;
        std::istringstream arguments(rest);
        arguments >> where >> std::dec >> length;
        std::uintptr_t address{};
        const bool isRva = where.starts_with("game+");
        const std::string_view digits = std::string_view(where).substr(isRva ? 5 : 0);
        const auto [stop, error] = std::from_chars(digits.data(), digits.data() + digits.size(), address, 16);
        if (error != std::errc{} || length == 0 || length > 4096) running.output += "error: peek <hex address or game+hex> [bytes, at most 4096]\n";
        else
        {
            if (isRva) address += reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            std::array<std::uint8_t, 16> row{};
            for (std::size_t at = 0; at < length; at += 16)
            {
                const std::size_t bytes = std::min<std::size_t>(row.size(), length - at);
                running.output += std::format("{:#014x}", address + at);
                if (!game_memory::readRaw(row.data(), address + at, bytes)) running.output += " unreadable";
                else
                    for (std::size_t i = 0; i < bytes; ++i) running.output += std::format(" {:02x}", row[i]);
                running.output += '\n';
            }
        }
    }
    else if (command == "drawn" && !rest.empty())
    {
        std::istringstream numbers(rest);
        float x{}, y{}, z{}, radius{};
        if (!(numbers >> x >> y >> z >> radius)) running.output += "error: drawn <x> <y> <z> <radius>\n";
        else
        {
            gi_inputs::recordRigidDraws(true);
            running.lines.push_front("drawn_text " + rest);
            running.lines.emplace_front("wait 10");
        }
    }
    else if (command == "drawn_text")
    {
        std::istringstream numbers(rest);
        float x{}, y{}, z{}, radius{};
        if (!(numbers >> x >> y >> z >> radius))
        {
            gi_inputs::takeRigidDraws();
            running.output += "error: drawn <x> <y> <z> <radius>\n";
        }
        else
        {
            // Each place and mesh once, with how many draws of it the frames had.
            std::map<std::tuple<long, long, long, std::uint32_t, std::uint32_t>, int> seen;
            for (const auto& draw : gi_inputs::takeRigidDraws())
            {
                const float dx = draw.position[0] - x, dy = draw.position[1] - y, dz = draw.position[2] - z;
                if (dx * dx + dy * dy + dz * dz > radius * radius) continue;

                ++seen[{std::lround(draw.position[0] * 100), std::lround(draw.position[1] * 100), std::lround(draw.position[2] * 100), draw.firstIndex,
                    draw.indexCount}];
            }
            for (const auto& [key, count] : seen)
            {
                const auto& [px, py, pz, first, indices] = key;
                running.output += std::format("({:.2f} {:.2f} {:.2f}) indices {} from {}: {} draws\n", px / 100.0, py / 100.0, pz / 100.0, indices, first, count);
            }
            if (seen.empty()) running.output += "no rigid draw there\n";
        }
    }
    else if (command == "owner_text")
        running.output += engine_scene::watchedOwners().value_or("error: no snapshot came: is the ray-traced scene running?\n");
    else if (command == "inspect" && !rest.empty())
    {
        waits.inspectionsBefore = inspect_trace::finished();
        waits.inspectionDeadline = devkit::commands::presents() + InspectionFrames;
        inspect_trace::request(devkit::commands::folder() / rest);
    }
    else if (command == "perf" && !rest.empty())
    {
        // The frame count comes last, if at all: a case's name can have spaces.
        auto file = trimmed(rest);
        std::uint32_t frames = DefaultCaptureFrames;
        if (const auto space = file.find_last_of(" \t"); space != file.npos)
        {
            const auto* const end = file.data() + file.size();
            std::uint32_t asked{};
            if (const auto [stop, error] = std::from_chars(file.data() + space + 1, end, asked); error == std::errc{} && stop == end)
            {
                file = trimmed(file.substr(0, space));
                frames = asked;
            }
        }

        waits.capturesBefore = perf_record::capturesFinished();
        waits.captureDeadline = devkit::commands::presents() + frames + InspectionFrames;
        perf_record::capture(devkit::commands::folder() / file, frames);
    }
    else if (command == "record_costs")
    {
        const bool isOn = invert("0");
        perf_record::setRecording(isOn);
        if (perf_record::isRecording() != isOn) running.output += "error: the recording switch could not be kept: PhantomFX.log says why\n";
    }
    else if (command == "image" && !rest.empty())
    {
        devkit::commands::askPresent([file = devkit::commands::folder() / (rest + ".dds"), rest](ID3D11DeviceContext* context, ID3D11Resource* backBuffer) {
            return saveTexture(context, backBuffer, DXGI_FORMAT_UNKNOWN, file) ? "saved " + rest + ".dds\n" : "error: the back buffer could not be saved\n";
        });
    }
    else if (command == "textures" && !rest.empty())
    {
        devkit::commands::askPresent([path = devkit::commands::folder() / rest, rest](ID3D11DeviceContext* context, ID3D11Resource*) {
            ray_tracing::saveTextures(context, path);
            game_shadow::save(context, path / "game-shadow.dds");
            return "saved the textures into " + rest + "\n";
        });
    }
    else if (command == "remaster")
        devkit::commands::askPresent([rest](ID3D11DeviceContext*, ID3D11Resource*) { return remaster::runLine(rest); });
    else if (command == "game_shadow") game_shadow::keep(invert("0"));
    else if (command == "enhancements") setEnhancements(invert("0"));
    else if (command == "reveal")
    {
        // The scene reads its models again with what it takes: restarted by the enhancements switch.
        for (const auto& later : {std::string("wait_scene"), std::string("enhancements 1"), "reveal_now " + rest, std::string("wait 5"), std::string("enhancements 0")})
            running.lines.push_front(later);
    }
    else if (command == "reveal_now") reveal(rest);
    else if (command == "setting")
    {
        const auto separator = rest.find('=');
        const auto key = trimmed(std::string_view(rest).substr(0, separator));
        const auto value = separator == rest.npos ? std::string_view{} : trimmed(std::string_view(rest).substr(separator + 1));
        const auto settings = core::currentSettings();
        const auto current = iniValue(settings, key);
        if (!current)
        {
            running.output += "error: no setting " + std::string(key) + "\n";
            return;
        }

        const auto changed = withLine(settings, key, value);
        core::applySettings(changed);

        // A test that changed nothing would read as costing nothing: asking for the value it has may be meant, a value
        // withLine turned down is not.
        if (changed != settings) return;

        if (isSameValue(*current, value)) running.output += "the setting stays as it was\n";
        else running.output += "error: " + std::string(key) + " does not take " + std::string(value) + "\n";
    }
    else if (command == "diagnostic") setDiagnostic(running, rest);
    else if (command == "lua")
    {
        const auto outcome = devkit::game_lua::run(rest, "=automation", true);
        if (!outcome) running.output += "error: the game's scripts are not running yet\n";
        else running.output += (outcome->ran ? "" : "error: ") + outcome->text + "\n";
    }
    else if (command == "travel")
    {
        constexpr std::pair<std::string_view, devkit::travel::Map> Maps[]{
            {"afghanistan", devkit::travel::Map::Afghanistan}, {"africa", devkit::travel::Map::Africa}, {"motherbase", devkit::travel::Map::MotherBase}};
        std::istringstream arguments(rest);
        std::string name;
        arguments >> name;
        std::array<float, 3> place{};
        const bool hasPlace = static_cast<bool>(arguments >> place[0] >> place[1] >> place[2]);
        const auto* const map = std::ranges::find(Maps, name, &std::pair<std::string_view, devkit::travel::Map>::first);
        if (map != std::end(Maps)) devkit::travel::go(map->second, hasPlace ? std::optional(place) : std::nullopt);
        else running.output += "error: travel afghanistan|africa|motherbase [<x> <y> <z>]\n";
    }
    else if (command == "clock")
    {
        std::istringstream arguments(rest);
        float seconds{};
        std::string run;
        if (arguments >> seconds)
        {
            weather::setClock(seconds);
            weather::stopClock(!(arguments >> run && run == "run"));
        }
        else running.output += "error: clock <seconds> [run]\n";
    }
    else if (command == "live")
    {
        if (rest == "0" || rest == "1") held_camera::setLive(rest == "1");
        else running.output += "error: live 0|1\n";
    }
    else if (command == "weather" && rest == "off") weather::release();
    else if (command == "weather")
    {
        std::istringstream arguments(rest);
        int type{};
        float fog{};
        if (arguments >> type && type >= 0 && type < static_cast<int>(weather::Type::Count))
        {
            arguments >> fog;
            weather::force(static_cast<weather::Type>(type), fog);
        }
        else running.output += "error: weather <type> [<fog>]\n";
    }
    else if (command == "state") writeState(rest);
    else if (command == "case_done")
    {
        std::ofstream(devkit::commands::folder() / "cases" / rest / "log.txt", std::ios::binary | std::ios::trunc) << running.output;
        log("Debug case " + rest + " saved.");
        fxui::toast(fxui::Tone::Ok, "Debug case saved", "The game stays frozen: F9 lets it go.");
    }
    else if (command == "status") running.output += status() + "\n";
    else running.output += "error: unknown command\n";
}

// Whether the line that last ran still has the render thread or the add-on working for it; a line it
// gave up on says so.
bool isWaiting(Script& running)
{
    if (waits.inspectionsBefore)
    {
        if (inspect_trace::finished() != *waits.inspectionsBefore) waits.inspectionsBefore.reset();
        else if (devkit::commands::presents() < waits.inspectionDeadline) return true;
        else
        {
            running.output += "error: no record came: is the ray-traced scene running?\n";
            waits.inspectionsBefore.reset();
        }
    }

    if (waits.capturesBefore)
    {
        if (perf_record::capturesFinished() != *waits.capturesBefore)
        {
            running.output += perf_record::lastCapture() + "\n";
            waits.capturesBefore.reset();
        }
        else if (devkit::commands::presents() < waits.captureDeadline) return true;
        else
        {
            running.output += "error: no capture came: the GPU's frames were not timed\n";
            waits.capturesBefore.reset();
            perf_record::cancelCapture();
        }
    }

    if (waits.sceneDeadline)
    {
        if (const auto work = sceneWork(); work != waits.sceneWork)
        {
            waits.sceneWork = work;
            waits.sceneBusyAt = devkit::commands::presents();
        }

        if (devkit::commands::presents() >= *waits.sceneDeadline) running.output += "error: the scene is still changing\n";
        else if (devkit::commands::presents() < waits.sceneBusyAt + SettledFrames) return true;
        else if (waits.sceneWork) running.output += std::format("the scene left {} things unmade\n", waits.sceneWork);

        waits.sceneDeadline.reset();
    }

    return false;
}

// F9: a case, or the game let go after one.
void onCaseKey()
{
    if (devkit::commands::isRunning())
    {
        fxui::toast(fxui::Tone::Warn, "A debug case is running");
        return;
    }

    std::deque<std::string> lines;
    if (held_camera::isHeld())
    {
        lines = {"release"};
        fxui::toast(fxui::Tone::Ok, "The game is let go");
    }
    else
    {
        lines = {"case"};
        fxui::toast(fxui::Tone::Ok, "Debug case", "Hold on while it records: the picture changes a few times.");
    }

    devkit::commands::start(std::move(lines));
}
}

void install(const std::filesystem::path& addonDirectory) noexcept
{
    std::error_code error;
    const auto candidate = addonDirectory / "PhantomFX-automation";

    // The automation copy must not change the player's saves. Left running without its own it would, even with
    // no commands, so it quits. Without a `saves` folder this is the player's game.
    const auto saves = candidate / "saves", userData = candidate / "local";
    if (std::filesystem::is_directory(saves, error) && !(std::filesystem::is_directory(userData, error) && devkit::save_redirect::install(saves, userData)))
    {
        log("The automation copy quits: it needs " + saves.string() + " with the saves to start from and " + userData.string() + ", and the game's files redirected there.");
        TerminateProcess(GetCurrentProcess(), 1);
    }

    std::filesystem::create_directories(candidate / "cases", error);
    if (error) return;

    devkit::commands::install(candidate, run, isWaiting, resetWaits);
    log("Automation reads command files from " + candidate.string() + ".");
}

void caseKey() noexcept
{
    if (!devkit::commands::folder().empty()) caseAsked = true;
}

void onFrameStart() noexcept
{
    if (caseAsked.exchange(false)) onCaseKey();

    devkit::commands::onFrameStart();
}

void onPresent(ID3D11DeviceContext* context, ID3D11Resource* backBuffer) noexcept
{
    devkit::commands::onPresent(context, backBuffer);
}
}
