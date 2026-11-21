// DevKit.addon64: the developer tools as their own ReShade add-on, beside PhantomFX and not inside
// it. It links only devkit and the game's build check, draws its page in ReShade's default ImGui
// style, and hooks the main loop for travel, cutscenes, the Lua console and command files.
#include "devaddon/game_loop.hpp"

#include "devkit/commands.hpp"
#include "devkit/cutscenes.hpp"
#include "devkit/game.hpp"
#include "devkit/game_lua.hpp"
#include "devkit/host.hpp"
#include "devkit/log.hpp"
#include "devkit/page.hpp"
#include "devkit/save_redirect.hpp"
#include "devkit/travel.hpp"
#include "device_check.hpp"
#include "log.hpp"

#include <Windows.h>
#include <MinHook.h>
#include <d3d11.h>
// Before reshade.hpp, so register_addon also connects the ImGui calls of the overlay to ReShade's.
#include <imgui.h>
#include <reshade.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string.h>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

extern "C" __declspec(dllexport) const char* NAME = "DevKit";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Developer tools for MGSV add-ons: travel, cutscenes, Lua, command files and saves redirection, with no PhantomFX inside.";

namespace
{
using namespace reshade::api;

HMODULE self{};

using AutomationInstall = void (*)(const wchar_t*);
using AutomationFrameStart = void (*)();
using AutomationPresent = void (*)(void*, void*);
AutomationInstall installAutomation{};
AutomationFrameStart frameStartAutomation{};
AutomationPresent presentAutomation{};
bool automationInstalled{};
std::filesystem::path gameFolder{};

// PhantomFX keeps the automation's commands; DevKit owns when it is installed and run.
bool connectAutomation()
{
    if (automationInstalled) return true;
    if (!installAutomation || !frameStartAutomation || !presentAutomation)
    {
        const auto module = GetModuleHandleW(L"PhantomFX.addon64");
        if (!module) return false;

        installAutomation = reinterpret_cast<AutomationInstall>(GetProcAddress(module, "PhantomFXAutomationInstall"));
        frameStartAutomation = reinterpret_cast<AutomationFrameStart>(GetProcAddress(module, "PhantomFXAutomationFrameStart"));
        presentAutomation = reinterpret_cast<AutomationPresent>(GetProcAddress(module, "PhantomFXAutomationPresent"));
        if (!installAutomation || !frameStartAutomation || !presentAutomation) return false;
    }

    installAutomation(gameFolder.c_str());
    devaddon::setAutomationFrameStart(frameStartAutomation);
    automationInstalled = true;
    phantom_fx::log("PhantomFX automation is hosted here: " + (gameFolder / "PhantomFX-automation").string() + ".");
    return true;
}

// The add-on's folder, as ReShade keeps its own files beside it.
std::filesystem::path dataDirectory()
{
    std::wstring path(MAX_PATH, L'\0');
    for (;;)
    {
        const auto length = GetModuleFileNameW(self, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) return std::filesystem::current_path();
        if (length < path.size()) return std::filesystem::path(path.substr(0, length)).parent_path();

        path.resize(path.size() * 2);
    }
}

void writeLog(std::string_view message)
{
    phantom_fx::log(message);
}

void toast(devkit::Tone tone, std::string_view text, std::string_view detail)
{
    const char* kind = "note";
    switch (tone)
    {
    case devkit::Tone::Dim: kind = "note"; break;
    case devkit::Tone::Ok: kind = "ok"; break;
    case devkit::Tone::Warn: kind = "warn"; break;
    case devkit::Tone::Bad: kind = "error"; break;
    }

    std::string line = std::string(kind) + ": " + std::string(text);
    if (!detail.empty()) line += " (" + std::string(detail) + ")";
    writeLog(line);
}

void append(devkit::commands::Script& script, const std::optional<devkit::game_lua::Outcome>& outcome)
{
    if (!outcome)
    {
        script.output += "error: the game's Lua is not available\n";
        return;
    }
    if (!outcome->ran)
    {
        script.output += "error: " + outcome->text + "\n";
        return;
    }
    if (!outcome->text.empty()) script.output += outcome->text + "\n";
}

// The command files' own lines: `lua`, `state`, `travel` and `cutscene`; `wait` is the runner's.
void runLine(devkit::commands::Script& script, const std::string& line)
{
    if (line == "state")
    {
        append(script, devkit::game_lua::run(devkit::game_lua::StateScript, "DevKit state", false));
        return;
    }
    if (line.starts_with("lua "))
    {
        append(script, devkit::game_lua::run(line.substr(4), "DevKit command", true));
        return;
    }
    if (line.starts_with("travel "))
    {
        const auto wanted = line.substr(7);
        for (int i = 0; i < static_cast<int>(devkit::travel::Map::Count); ++i)
        {
            const auto map = static_cast<devkit::travel::Map>(i);
            if (_stricmp(wanted.c_str(), devkit::travel::name(map)) != 0) continue;
            devkit::travel::go(map);
            script.output += "travel requested\n";
            return;
        }
        script.output += "error: travel Afghanistan|Africa|MotherBase\n";
        return;
    }
    if (line.starts_with("cutscene "))
    {
        std::istringstream indexText(line.substr(9));
        std::size_t index{};
        if (indexText >> index && index < devkit::cutscenes::labels().size())
        {
            devkit::cutscenes::play(index);
            script.output += "cutscene requested\n";
        }
        else
        {
            script.output += "error: cutscene <index>\n";
        }
        return;
    }

    script.output += "error: unknown command\n";
}

bool isWaiting(devkit::commands::Script&) noexcept
{
    return false;
}

ID3D11DeviceContext* native(command_list* list)
{
    return reinterpret_cast<ID3D11DeviceContext*>(list->get_native());
}

// The command runner's present jobs, and the frame count `wait` counts in.
void onPresent(command_queue* queue, swapchain* swapChain, const rect*, const rect*, uint32_t, const rect*)
{
    devaddon::onPresent();

    auto* list = queue->get_immediate_command_list();
    if (!list) return;

    if (connectAutomation())
        presentAutomation(native(list), reinterpret_cast<ID3D11Resource*>(swapChain->get_current_back_buffer().handle));

    devkit::commands::onPresent(native(list), reinterpret_cast<ID3D11Resource*>(swapChain->get_current_back_buffer().handle));
}

void onOverlay(effect_runtime*)
{
    devkit::page::draw();
}
}

namespace devkit::game
{
bool isSupportedBuild() noexcept
{
    return phantom_fx::isSupportedGameBuild();
}
}

extern "C" __declspec(dllexport) bool AddonInit(HMODULE addon, HMODULE reshadeModule)
{
    if (!reshade::register_addon(addon, reshadeModule)) return false;

    self = addon;
    const auto directory = dataDirectory();
    gameFolder = directory;
    const auto data = directory / "DevKit";
    std::error_code error;
    std::filesystem::create_directories(data / "commands", error);

    phantom_fx::openLog(directory / "DevKit.log");
    phantom_fx::log("DevKit: the developer tools as their own add-on.");
    phantom_fx::log(phantom_fx::isSupportedGameBuild() ? "Known game build." : "Unknown game build; the tools that need its code cannot run.");
    phantom_fx::log(phantom_fx::passDeviceCheck());

    devkit::setName("DevKit");
    devkit::setLogger(&writeLog);
    devkit::setToast(&toast);

    MH_Initialize();
    devaddon::defer();
    phantom_fx::log("Waiting for the first frame to register on PhantomFX's frame start.");

    const auto saves = data / "saves", local = data / "local";
    if (std::filesystem::is_directory(saves, error))
        phantom_fx::log(devkit::save_redirect::install(saves, local) ? "The saves are redirected into " + saves.string() + "."
                                                                    : "The saves could not be redirected; do not run the game on this copy.");

    devkit::commands::install(data / "commands", &runLine, &isWaiting);
    phantom_fx::log("Command files are read from " + (data / "commands").string() + ".");

    reshade::register_event<reshade::addon_event::present>(onPresent);
    reshade::register_overlay("DevKit", onOverlay);

    return true;
}

extern "C" __declspec(dllexport) void AddonUninit(HMODULE addon, HMODULE reshadeModule)
{
    devaddon::unhook();
    reshade::unregister_overlay("DevKit", onOverlay);
    reshade::unregister_event<reshade::addon_event::present>(onPresent);
    MH_Uninitialize();
    reshade::unregister_addon(addon, reshadeModule);
}
