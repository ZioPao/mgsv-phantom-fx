#include "dev/dev.hpp"

#include "core.hpp"
#include "dev/automation.hpp"
#include "device_check.hpp"
#include "dev/perf_record.hpp"
#include "dev/remaster/tool.hpp"
#include "devkit/cutscenes.hpp"
#include "devkit/game.hpp"
#include "devkit/game_lua.hpp"
#include "devkit/host.hpp"
#include "devkit/log.hpp"
#include "devkit/travel.hpp"
#include "features/photo_mode/photo_mode.hpp"
#include "fxui/fxui.hpp"
#include "log.hpp"

#include <string_view>

// The devkit's game layer: the build check the add-on makes.
namespace devkit::game
{
bool isSupportedBuild() noexcept
{
    return phantom_fx::isSupportedGameBuild();
}
}

namespace phantom_fx::dev
{
namespace
{
using fxui::Tone;

fxui::Tone toneOf(devkit::Tone tone)
{
    switch (tone)
    {
    case devkit::Tone::Dim: return fxui::Tone::Dim;
    case devkit::Tone::Ok: return fxui::Tone::Ok;
    case devkit::Tone::Warn: return fxui::Tone::Warn;
    case devkit::Tone::Bad: return fxui::Tone::Bad;
    }

    return fxui::Tone::Dim;
}

// What frames cost, on file.
void drawCostRecording()
{
    const fxui::Item item({.id = "costRecording", .name = "Costs on file", .summary = "A row a second of what frames cost, to read after playing.",
        .keywords = "performance recorder record frame time gpu log"});
    if (!item) return;

    bool isOn = perf_record::isRecording();
    if (fxui::checkbox("Record the costs", isOn,
            "Writes every pass, the rays, the lights, the map and the clock to PhantomFX-automation/perf once a second, until turned off, across starts. "
            "tools/automation/perf.py log reads it. Debug cases keep their own costs either way."))
        perf_record::setRecording(isOn);
}

}

void install(const std::filesystem::path& addonDirectory) noexcept
{
    devkit::setName("PhantomFX");
    devkit::setToast([](devkit::Tone tone, std::string_view text, std::string_view detail) { fxui::toast(toneOf(tone), text, detail); });
    devkit::setLogger(&log);
    automation::setHosted(GetModuleHandleW(L"DevKit.addon64") != nullptr);
    if (!automation::isHosted()) automation::install(addonDirectory);
    perf_record::install(addonDirectory / "PhantomFX-automation" / "perf");
}

/** The game thread, at the main loop's frame start: each tool that runs there, in order. */
void onFrameStart() noexcept
{
    devkit::travel::onFrameStart();
    devkit::cutscenes::onFrameStart();
    devkit::game_lua::onFrameStart();
    if (!automation::isHosted()) automation::onFrameStart();
    perf_record::onFrameStart();
    photo_mode::onFrameStart();
}

void onPresent(ID3D11DeviceContext* context, ID3D11Resource* backBuffer) noexcept
{
    if (!automation::isHosted()) automation::onPresent(context, backBuffer);
}

void onCaseKey() noexcept
{
    automation::caseKey();
}

void drawPage()
{
    const fxui::Page page(Section::Developer);
    if (!page) return;

    drawCostRecording();
    remaster::drawTool();
    if constexpr (DiagnosticLog)
    {
        auto options = core::currentDiagnostics();
        const fxui::Item item({.id = "globalIlluminationLog", .name = "Global illumination log", .keywords = "log often light fade"});
        if (item
            && fxui::checkbox("Log global illumination often", options.logGlobalIlluminationOften,
                "With global illumination on, its light and each step's largest value go to the log every few frames, not every ten seconds: to see "
                "how fast light fades from each step after a light goes away."))
            core::applyDiagnostics(options);
    }
}
}
