#include "devkit/automation.hpp"

#include "devkit/commands.hpp"
#include "devkit/cutscenes.hpp"
#include "devkit/game_lua.hpp"
#include "devkit/host.hpp"
#include "devkit/log.hpp"
#include "devkit/phantom_api.hpp"
#include "devkit/save_redirect.hpp"
#include "devkit/travel.hpp"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace devkit::automation
{
namespace
{
using Script = commands::Script;

// Presents a line waits for the render thread before it gives up: with frame generation several a game frame.
constexpr std::uint64_t InspectionFrames = 3600, SceneFrames = 3600;
// Presents the scene's work to do stays the same before it counts as made: a model that never gets room
// would otherwise hold a case up for SceneFrames.
constexpr std::uint64_t SettledFrames = 60;
// The GPU's frames a `perf` line times without saying.
constexpr std::uint32_t DefaultCaptureFrames = 120;
// Presents after a probe's pixel changes before its text has that pixel: a readback's frames in flight.
constexpr std::string_view ProbeWait = "wait 8";
// Frames between looks at where the game is while its startup logos would show.
constexpr std::uint32_t LogoPollFrames = 30;
// The game shows konamiLogo, kjpLogo and foxLogo six seconds each in this sequence (init_sequence.lua,
// Seq_Demo_ShowKonamiAndFoxLogo). Deleted and the sequence left as Infinite Heaven's skipLogos does, the
// pre-title comes at once.
constexpr std::string_view LogoSkipScript = R"(
if not TppSequence then return "" end
local dropped = 0
for _, name in ipairs({ "konamiLogo", "kjpLogo", "foxLogo" }) do
  local splash = SplashScreen and SplashScreen.GetSplashScreenWithName(name)
  if splash then SplashScreen.Delete(splash); dropped = dropped + 1 end
end
local sequence = TppSequence.GetCurrentSequenceName()
if sequence == "Seq_Demo_ShowKonamiAndFoxLogo" then TppUiCommand.StartPreTitleSequence() end
return sequence .. (dropped > 0 and (" dropped " .. dropped) or "")
)";

std::atomic<bool> caseAsked{};
std::uint32_t framesSinceLogos{};
bool logosPassed{}; // A copy that holds `skip-logos`, once the game is past its startup logos

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

// PhantomFX's dev API, once PhantomFX has loaded: DevKit loads first, so it is looked for when a line needs it.
const PhantomFXDevApi* phantom() noexcept
{
    static const PhantomFXDevApi* found = nullptr;
    if (found) return found;

    const auto module = GetModuleHandleW(L"PhantomFX.addon64");
    if (!module) return nullptr;

    const auto get = reinterpret_cast<PhantomFXGetDevApiFn>(GetProcAddress(module, "PhantomFXGetDevApi"));
    const auto* const api = get ? get() : nullptr;
    if (!api || api->version != PhantomFXDevApiVersion || api->size != sizeof(PhantomFXDevApi))
    {
        devkit::log("The automation: PhantomFX's dev API is not the one this DevKit knows; its lines are unavailable.");
        return nullptr;
    }

    found = api;
    devkit::log("The automation reaches PhantomFX's dev API.");

    return found;
}

// A line that needs PhantomFX, or the answer it says it cannot give.
const PhantomFXDevApi* need(Script& running)
{
    const auto* const api = phantom();
    if (!api) running.output += "error: PhantomFX is not loaded, so it has nothing to answer this line\n";

    return api;
}

// The text a function of the API gives, whatever its length; empty text when it has no answer.
template<class Fetch> std::optional<std::string> fetch(Fetch&& read)
{
    std::string text(512, '\0');
    auto length = read(text.data(), text.size());
    if (length == PhantomFXNone) return std::nullopt;

    if (length >= text.size())
    {
        text.assign(length + 1, '\0');
        length = read(text.data(), text.size());
        if (length == PhantomFXNone) return std::nullopt;
    }

    text.resize(std::min(length, text.size() - 1));

    return text;
}

std::string_view trimmed(std::string_view text)
{
    const auto first = text.find_first_not_of(" \t");
    if (first == text.npos) return {};

    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

std::string utf8(const std::filesystem::path& path)
{
    const auto text = path.u8string();

    return std::string(text.begin(), text.end());
}

std::string text(const PhantomFXPose& pose)
{
    return std::format("{} {} {} {} {} {}", pose.position[0], pose.position[1], pose.position[2], pose.yaw, pose.pitch, pose.focalLength);
}

// A pose's five numbers; its focal length is read apart, as `move` puts its frames before it.
std::optional<PhantomFXPose> readPose(std::istream& numbers)
{
    PhantomFXPose pose{};
    if (numbers >> pose.position[0] >> pose.position[1] >> pose.position[2] >> pose.yaw >> pose.pitch) return pose;

    return std::nullopt;
}

// An optional last number: the focal length, or 0 (the game camera's lens) without one.
float readFocalLength(std::istream& numbers)
{
    float millimetres{};

    return numbers >> millimetres ? millimetres : 0.0f;
}

std::string status(const PhantomFXDevApi& api)
{
    PhantomFXPose camera{};
    const bool hasCamera = api.cameraLast(&camera) != 0;
    const auto held = api.cameraState();
    auto line = std::format("frame {}, presented {}, {}, ", api.frame(), commands::presents(), held == 0 ? "running" : held == 2 ? "held live" : "frozen");
    line += hasCamera ? "camera " + text(camera) : std::string("no camera yet");

    return line + "\n" + fetch([&](char* out, std::size_t cap) { return api.sceneStatus(out, cap); }).value_or("");
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

// What the game is at: its Lua's state, then, with PhantomFX, its camera, sky and scene.
std::string stateText(const std::string& name)
{
    std::string state;
    if (!name.empty()) state += "name=" + name + "\n";
    const auto* const api = phantom();
    if (api)
    {
        PhantomFXPose camera{};
        state += std::format("frame={}\ncamera={}\n", api->frame(), api->cameraLast(&camera) ? text(camera) : std::string("?"));
    }

    if (const auto outcome = game_lua::run(game_lua::StateScript, "=automation")) state += outcome->text + "\n";

    if (api)
    {
        PhantomFXWeather sky{};
        api->weatherState(&sky);
        state += std::format("clock={}\nweather={} {}\n", sky.clock, sky.requested, sky.fogDensity);
        state += "scene=" + fetch([&](char* out, std::size_t cap) { return api->sceneStatus(out, cap); }).value_or("") + "\n";
    }

    return state;
}

// What the game is at into the case's folder, with the settings it ran on.
void writeState(const std::string& name)
{
    const auto caseFolder = commands::folder() / "cases" / name;
    std::error_code error;
    std::filesystem::create_directories(caseFolder, error);

    std::ofstream(caseFolder / "state.txt", std::ios::trunc) << stateText(name);
    std::filesystem::copy_file(commands::folder().parent_path() / "PhantomFX.ini", caseFolder / "PhantomFX.ini", std::filesystem::copy_options::overwrite_existing, error);
}

void collect(const PhantomFXRigidDraw* draw, void* user)
{
    static_cast<std::deque<PhantomFXRigidDraw>*>(user)->push_back(*draw);
}

void run(Script& running, const std::string& line)
{
    std::istringstream words(line);
    std::string command;
    words >> command;
    std::string rest;
    std::getline(words >> std::ws, rest);
    const auto invert = [&](std::string_view off) { return rest != off; };

    // The lines that stand on the game's own scripts alone.
    if (command == "lua")
    {
        const auto outcome = game_lua::run(rest, "=automation", true);
        if (!outcome) running.output += "error: the game's scripts are not running yet\n";
        else running.output += (outcome->ran ? "" : "error: ") + outcome->text + "\n";
        return;
    }
    if (command == "travel")
    {
        constexpr std::pair<std::string_view, travel::Map> Maps[]{
            {"afghanistan", travel::Map::Afghanistan}, {"africa", travel::Map::Africa}, {"motherbase", travel::Map::MotherBase}};
        std::istringstream arguments(rest);
        std::string name;
        arguments >> name;
        std::array<float, 3> place{};
        const bool hasPlace = static_cast<bool>(arguments >> place[0] >> place[1] >> place[2]);
        const auto* const map = std::ranges::find(Maps, name, &std::pair<std::string_view, travel::Map>::first);
        if (map != std::end(Maps)) travel::go(map->second, hasPlace ? std::optional(place) : std::nullopt);
        else running.output += "error: travel afghanistan|africa|motherbase [<x> <y> <z>]\n";
        return;
    }
    if (command == "cutscene")
    {
        std::size_t index{};
        if (std::istringstream(rest) >> index && index < cutscenes::labels().size())
        {
            cutscenes::play(index);
            running.output += "cutscene requested\n";
        }
        else running.output += "error: cutscene <index of the list>\n";
        return;
    }
    if (command == "wait_scene" && !phantom())
    {
        running.output += "error: PhantomFX is not loaded, so there is no scene to wait for\n";
        return;
    }
    if (command == "state")
    {
        if (rest.empty()) running.output += stateText("");
        else writeState(rest);
        return;
    }
    if (command == "peek" && !rest.empty())
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
                SIZE_T read{};
                if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address + at), row.data(), bytes, &read) || read != bytes) running.output += " unreadable";
                else
                    for (std::size_t i = 0; i < bytes; ++i) running.output += std::format(" {:02x}", row[i]);
                running.output += '\n';
            }
        }
        return;
    }
    if (command == "case_done")
    {
        std::ofstream(commands::folder() / "cases" / rest / "log.txt", std::ios::binary | std::ios::trunc) << running.output;
        devkit::log("Debug case " + rest + " saved.");
        devkit::toast(Tone::Ok, "Debug case saved", "The game stays frozen: F9 lets it go.");
        return;
    }
    if (command == "case")
    {
        const auto lines = caseLines(rest.empty() ? commands::timeNow() : rest);
        running.lines.insert(running.lines.begin(), lines.begin(), lines.end());
        return;
    }
    if (command == "reveal")
    {
        // The scene reads its models again with what it takes: restarted by the enhancements switch.
        for (const auto& later : {std::string("wait_scene"), std::string("enhancements 1"), "reveal_now " + rest, std::string("wait 5"), std::string("enhancements 0")})
            running.lines.push_front(later);
        return;
    }
    if (command == "probe")
    {
        std::uint32_t x{}, y{};
        if (!(std::istringstream(rest) >> x >> y))
        {
            running.output += "error: probe <x> <y>\n";
            return;
        }

        const auto* const api = need(running);
        if (!api) return;

        api->probeAt(x, y);
        running.lines.push_front("probe_text");
        running.lines.emplace_front(ProbeWait);
        return;
    }
    if (command == "wait" || command.empty()) return;

    // Everything else is PhantomFX's to answer.
    const auto* const api = need(running);
    if (!api) return;

    if (command == "freeze")
    {
        PhantomFXPose camera{};
        if (api->cameraLast(&camera)) api->cameraHold(&camera);
        else running.output += "error: no camera yet\n";
    }
    else if (command == "release")
    {
        api->cameraRelease();
        api->probeClear();
    }
    else if (command == "camera")
    {
        std::istringstream numbers(rest);
        if (auto pose = readPose(numbers))
        {
            pose->focalLength = readFocalLength(numbers);
            api->cameraHold(&*pose);
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
            api->cameraMove(&*pose, frames);
        }
        else running.output += "error: move <x> <y> <z> <yaw> <pitch> <frames> [<mm>]\n";
    }
    else if (command == "wait_scene")
    {
        waits.sceneDeadline = commands::presents() + SceneFrames;
        waits.sceneWork = api->sceneWork();
        waits.sceneBusyAt = commands::presents();
    }
    else if (command == "probe_text") running.output += fetch([&](char* out, std::size_t cap) { return api->probeText(out, cap); }).value_or("") + "\n";
    else if (command == "owner" && !rest.empty())
    {
        std::uint64_t hash{};
        const auto hex = trimmed(rest);
        if (const auto [stop, error] = std::from_chars(hex.data(), hex.data() + hex.size(), hash, 16); error != std::errc{} || !hash)
            running.output += "error: owner <model hash, hex>\n";
        else
        {
            api->watchOwners(hash);
            running.lines.push_front("owner_text");
            running.lines.emplace_front("wait 10");
        }
    }
    else if (command == "drawn" && !rest.empty())
    {
        std::istringstream numbers(rest);
        float x{}, y{}, z{}, radius{};
        if (!(numbers >> x >> y >> z >> radius)) running.output += "error: drawn <x> <y> <z> <radius>\n";
        else
        {
            api->recordRigidDraws(1);
            running.lines.push_front("drawn_text " + rest);
            running.lines.emplace_front("wait 10");
        }
    }
    else if (command == "drawn_text")
    {
        std::istringstream numbers(rest);
        float x{}, y{}, z{}, radius{};
        std::deque<PhantomFXRigidDraw> draws;
        api->takeRigidDraws(&collect, &draws);
        if (!(numbers >> x >> y >> z >> radius)) running.output += "error: drawn <x> <y> <z> <radius>\n";
        else
        {
            // Each place and mesh once, with how many draws of it the frames had.
            std::map<std::tuple<long, long, long, std::uint32_t, std::uint32_t>, int> seen;
            for (const auto& draw : draws)
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
        running.output += fetch([&](char* out, std::size_t cap) { return api->watchedOwners(out, cap); }).value_or("error: no snapshot came: is the ray-traced scene running?\n");
    else if (command == "inspect" && !rest.empty())
    {
        waits.inspectionsBefore = api->inspectFinished();
        waits.inspectionDeadline = commands::presents() + InspectionFrames;
        api->inspectRequest(utf8(commands::folder() / rest).c_str());
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

        waits.capturesBefore = api->perfFinished();
        waits.captureDeadline = commands::presents() + frames + InspectionFrames;
        api->perfCapture(utf8(commands::folder() / std::string(file)).c_str(), frames);
    }
    else if (command == "record_costs")
    {
        const bool isOn = invert("0");
        api->setPerfRecording(isOn ? 1 : 0);
        if ((api->perfRecording() != 0) != isOn) running.output += "error: the recording switch could not be kept: PhantomFX.log says why\n";
    }
    else if (command == "image" && !rest.empty())
    {
        commands::askPresent([file = utf8(commands::folder() / (rest + ".dds")), rest, api](ID3D11DeviceContext* context, ID3D11Resource* backBuffer) {
            return api->saveBackBuffer(context, backBuffer, file.c_str()) ? "saved " + rest + ".dds\n" : "error: the back buffer could not be saved\n";
        });
    }
    else if (command == "textures" && !rest.empty())
    {
        commands::askPresent([path = utf8(commands::folder() / rest), rest, api](ID3D11DeviceContext* context, ID3D11Resource*) {
            api->saveTextures(context, path.c_str());
            return "saved the textures into " + rest + "\n";
        });
    }
    else if (command == "remaster")
    {
        commands::askPresent([rest, api](ID3D11DeviceContext*, ID3D11Resource*) {
            return fetch([&](char* out, std::size_t cap) { return api->remasterLine(rest.c_str(), out, cap); }).value_or("");
        });
    }
    else if (command == "game_shadow") api->keepGameShadow(invert("0") ? 1 : 0);
    else if (command == "enhancements") api->setEnhancements(invert("0") ? 1 : 0);
    else if (command == "reveal_now") api->reveal(rest.c_str());
    else if (command == "setting")
    {
        const auto separator = rest.find('=');
        const auto key = std::string(trimmed(std::string_view(rest).substr(0, separator)));
        const auto value = separator == rest.npos ? std::string() : std::string(trimmed(std::string_view(rest).substr(separator + 1)));
        switch (api->settingSet(key.c_str(), value.c_str()))
        {
        case 0: running.output += "error: no setting " + key + "\n"; break;
        case 2: running.output += "the setting stays as it was\n"; break;
        case 3: running.output += "error: " + key + " does not take " + value + "\n"; break;
        default: break;
        }
    }
    else if (command == "diagnostic")
    {
        std::istringstream arguments(rest);
        std::string name;
        int value{};
        if (!(arguments >> name >> value) || !api->diagnostic(name.c_str(), value))
            running.output += "error: diagnostic debug_view|show_scene|two_sided_shadows|no_jitter|river_light <value>\n";
    }
    else if (command == "clock")
    {
        std::istringstream arguments(rest);
        float seconds{};
        std::string mode;
        if (arguments >> seconds)
        {
            api->setClock(seconds);
            api->stopClock(!(arguments >> mode && mode == "run") ? 1 : 0);
        }
        else running.output += "error: clock <seconds> [run]\n";
    }
    else if (command == "live")
    {
        if (rest == "0" || rest == "1") api->cameraLive(rest == "1" ? 1 : 0);
        else running.output += "error: live 0|1\n";
    }
    else if (command == "weather" && rest == "off") api->releaseWeather();
    else if (command == "weather")
    {
        std::istringstream arguments(rest);
        int type{};
        float fog{};
        if (arguments >> type && type >= 0 && type < api->weatherTypes())
        {
            arguments >> fog;
            api->forceWeather(type, fog);
        }
        else running.output += "error: weather <type> [<fog>]\n";
    }
    else if (command == "status") running.output += status(*api) + "\n";
    else running.output += "error: unknown command\n";
}

// Whether the line that last ran still has the render thread or the add-on working for it; a line it
// gave up on says so.
bool isWaiting(Script& running)
{
    const auto* const api = phantom();
    if (!api)
    {
        resetWaits();
        return false;
    }

    if (waits.inspectionsBefore)
    {
        if (api->inspectFinished() != *waits.inspectionsBefore) waits.inspectionsBefore.reset();
        else if (commands::presents() < waits.inspectionDeadline) return true;
        else
        {
            running.output += "error: no record came: is the ray-traced scene running?\n";
            waits.inspectionsBefore.reset();
        }
    }

    if (waits.capturesBefore)
    {
        if (api->perfFinished() != *waits.capturesBefore)
        {
            running.output += fetch([&](char* out, std::size_t cap) { return api->perfLast(out, cap); }).value_or("") + "\n";
            waits.capturesBefore.reset();
        }
        else if (commands::presents() < waits.captureDeadline) return true;
        else
        {
            running.output += "error: no capture came: the GPU's frames were not timed\n";
            waits.capturesBefore.reset();
            api->perfCancel();
        }
    }

    if (waits.sceneDeadline)
    {
        if (const auto work = api->sceneWork(); work != waits.sceneWork)
        {
            waits.sceneWork = work;
            waits.sceneBusyAt = commands::presents();
        }

        if (commands::presents() >= *waits.sceneDeadline) running.output += "error: the scene is still changing\n";
        else if (commands::presents() < waits.sceneBusyAt + SettledFrames) return true;
        else if (waits.sceneWork) running.output += std::format("the scene left {} things unmade\n", waits.sceneWork);

        waits.sceneDeadline.reset();
    }

    return false;
}

// F9: a case, or the game let go after one.
void onCaseKey()
{
    if (commands::isRunning())
    {
        devkit::toast(Tone::Warn, "A debug case is running");
        return;
    }

    const auto* const api = phantom();
    if (!api)
    {
        devkit::toast(Tone::Warn, "No debug case", "PhantomFX is not loaded.");
        return;
    }

    std::deque<std::string> lines;
    if (api->cameraState() != 0)
    {
        lines = {"release"};
        devkit::toast(Tone::Ok, "The game is let go");
    }
    else
    {
        lines = {"case"};
        devkit::toast(Tone::Ok, "Debug case", "Hold on while it records: the picture changes a few times.");
    }

    commands::start(std::move(lines));
}

// The game's startup logos, dropped for a copy that holds `skip-logos` in its folder: a fresh run spends
// eighteen seconds on them before the title (init_sequence.lua, LogoSkipScript).
void skipStartupLogos() noexcept
{
    if (logosPassed || ++framesSinceLogos < LogoPollFrames) return;

    framesSinceLogos = 0;
    std::error_code error;
    const auto& folder = commands::folder();
    if (folder.empty() || !std::filesystem::exists(folder / "skip-logos", error)) return;

    const auto outcome = game_lua::run(LogoSkipScript, "=skip logos", false);
    if (!outcome) return;

    const auto& result = outcome->text;
    if (result.find(" dropped ") != std::string::npos || result.starts_with("Seq_Demo_ShowKonamiAndFoxLogo")) devkit::log("Startup logos: " + result);
    logosPassed = result.starts_with("Seq_Game_");
}
}

void install(const std::filesystem::path& addonDirectory) noexcept
{
    std::error_code error;
    const auto candidate = addonDirectory / "PhantomFX-automation";

    // The automation copy must not change the player's saves. Left running without its own it would, even with
    // no commands, so it quits. Without a `saves` folder this is the player's game.
    const auto saves = candidate / "saves", userData = candidate / "local";
    if (std::filesystem::is_directory(saves, error) && !(std::filesystem::is_directory(userData, error) && save_redirect::install(saves, userData)))
    {
        devkit::log("The automation copy quits: it needs " + saves.string() + " with the saves to start from and " + userData.string() + ", and the game's files redirected there.");
        TerminateProcess(GetCurrentProcess(), 1);
    }

    std::filesystem::create_directories(candidate / "cases", error);
    if (error) return;

    commands::install(candidate, run, isWaiting, resetWaits);
    devkit::log("Automation reads command files from " + candidate.string() + ".");
}

void caseKey() noexcept
{
    if (!commands::folder().empty()) caseAsked = true;
}

void onFrameStart() noexcept
{
    skipStartupLogos();
    if (caseAsked.exchange(false)) onCaseKey();

    commands::onFrameStart();
}

void onPresent(ID3D11DeviceContext* context, ID3D11Resource* backBuffer) noexcept
{
    commands::onPresent(context, backBuffer);
}
}
