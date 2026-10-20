// The costs in the overlay's Diagnostics, over the game, and as text for PhantomFX-performance.txt.
#include "performance.hpp"
#include "menu_style.hpp"

#include <Windows.h>
#include <imgui.h>
#include <reshade.hpp> // Routes ImGui calls through ReShade's overlay.
#include <algorithm>
#include <format>
#include <vector>

namespace phantom_fx::performance
{
namespace
{
using namespace menu;

const ImVec4 Blue{0.5f, 0.65f, 0.95f, 1}, Teal{0.4f, 0.8f, 0.75f, 1}, Orange{0.95f, 0.6f, 0.3f, 1}, Purple{0.75f, 0.55f, 0.95f, 1},
    Pink{0.95f, 0.5f, 0.7f, 1};

struct Part { const char* name; Pass pass; };
struct Group { const char* name; ImVec4 colour; std::vector<Part> parts; };

// One row per effect, each with its passes.
const std::vector<Group>& groups()
{
    static const std::vector<Group> all{
        {"Upscaling", Green, {{"Motion vectors and inputs", Pass::UpscalerInputs}, {"DLSS or FSR", Pass::Upscaler},
            {"Sharpening and output", Pass::UpscalerOutput}}},
        {"Sun shadows", Yellow, {{"Contact shadow trace", Pass::SunTrace}, {"Sun light pass (with the game's lighting)", Pass::SunLight}}},
        {"Lamp contact shadows", Orange, {{"Traces", Pass::LampTraces}, {"Shadow draws", Pass::LampShadows}}},
        {"GTVBAO", Teal, {{"March", Pass::OcclusionMarch}, {"Denoise", Pass::OcclusionDenoise},
            {"Applying to the indirect light", Pass::OcclusionApply}}},
        {"SSILVB", Pink, {{"March", Pass::LightMarch}, {"Denoise", Pass::LightDenoise}}},
        {"Volumetric fog", Blue, {{"The game's fog pass, larger", Pass::FogPass}, {"Sky tables (physical look)", Pass::SkyTables},
            {"Sun shadow in the fog", Pass::FogShadow}, {"Scattering", Pass::FogScattering}, {"Integration", Pass::FogIntegration},
            {"Physical sky", Pass::Sky}}},
        {"Depth of field", Purple, {{"Setup and tile dilate", Pass::DofSetup}, {"Gather", Pass::DofGather}, {"Composite", Pass::DofComposite},
            {"Lens shot", Pass::LensShot}}},
        {"HDR", Grey, {{"Encoding the scene", Pass::HdrEncode}, {"Decoding copies", Pass::HdrDecode}, {"Grading", Pass::HdrGrade}}},
    };

    return all;
}

float millisecondsOf(const Costs& costs, Pass pass)
{
    return costs.passes[static_cast<std::size_t>(pass)];
}

float millisecondsOf(const Costs& costs, const Group& group)
{
    float sum{};
    for (const auto& part : group.parts) sum += millisecondsOf(costs, part.pass);

    return sum;
}

// A horizontal bar filled to `fraction` of `width`.
void bar(float fraction, const ImVec4& colour, float width)
{
    const auto start = ImGui::GetCursorScreenPos();
    const float height = ImGui::GetTextLineHeight();
    const ImVec2 top{start.x, start.y + height * 0.25f}, bottom{start.x + width, start.y + height * 0.8f};
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(top, bottom, IM_COL32(255, 255, 255, 24), 2);
    const float filled = width * std::clamp(fraction, 0.0f, 1.0f);
    if (filled >= 1) draw->AddRectFilled(top, ImVec2(start.x + filled, bottom.y), ImGui::ColorConvertFloat4ToU32(colour), 2);
    ImGui::Dummy(ImVec2(width, height));
}

void rightAligned(const std::string& text, bool dim = false)
{
    const float offset = ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(text.c_str()).x;
    if (offset > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
    if (dim) ImGui::TextDisabled("%s", text.c_str());
    else ImGui::TextUnformatted(text.c_str());
}

// One row per effect: its bar against the costliest, its milliseconds and its share of the GPU
// frame. With details, each opens to its passes.
void drawCostTable(const Costs& costs, bool details)
{
    float largest = 0.01f;
    for (const auto& group : groups()) largest = std::max(largest, millisecondsOf(costs, group));

    const auto share = [&](float milliseconds) { return costs.gpuFrame > 0 ? std::format("{:.0f}%", milliseconds / costs.gpuFrame * 100) : std::string(); };
    if (!ImGui::BeginTable("costs", 4, ImGuiTableFlags_SizingFixedFit)) return;

    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 7);
    ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 3);
    ImGui::TableSetupColumn("frame", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 2.5f);
    for (const auto& group : groups())
    {
        const float milliseconds = millisecondsOf(costs, group);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushStyleColor(ImGuiCol_Text, group.colour);
        bool open{};
        if (details) open = ImGui::TreeNodeEx(group.name, ImGuiTreeNodeFlags_SpanAllColumns);
        else ImGui::TextUnformatted(group.name);
        ImGui::PopStyleColor();
        ImGui::TableNextColumn();
        bar(milliseconds / largest, group.colour, ImGui::GetFontSize() * 7);
        ImGui::TableNextColumn();
        rightAligned(std::format("{:.2f}", milliseconds));
        ImGui::TableNextColumn();
        rightAligned(share(milliseconds), true);
        if (!open) continue;

        for (const auto& part : group.parts)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", part.name);
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            rightAligned(std::format("{:.2f}", millisecondsOf(costs, part.pass)), true);
        }

        ImGui::TreePop();
    }

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted("Phantom FX total");
    ImGui::TableNextColumn();
    ImGui::TableNextColumn();
    rightAligned(std::format("{:.2f}", costs.total()));
    ImGui::TableNextColumn();
    rightAligned(share(costs.total()), true);
    ImGui::EndTable();
}

// The frame above the table, and the measurement while it runs.
void drawHeadline(const Status& state)
{
    const auto& c = state.costs;
    ImGui::Text("GPU frame %.2f ms; frame %.2f ms (%.0f fps)", c.gpuFrame, c.frame, c.frame > 0 ? 1000 / c.frame : 0.0f);
    if (state.measurement.running)
        statusLine(Yellow, std::format("Measuring the real cost: step {} of {}. Hold still.", state.measurement.step + 1, state.measurement.stepCount));
}

// What the last measurement found each effect really costs, everything included.
void drawRealCost(const Measurement& m)
{
    if (m.steps.size() < 2)
    {
        if (!m.running) ImGui::TextDisabled("Real cost: not measured yet.");
        return;
    }

    float largest = 0.01f;
    for (std::size_t i = 1; i < m.steps.size(); ++i) largest = std::max(largest, m.steps[i - 1].gpuFrame - m.steps[i].gpuFrame);

    const auto minutes = (GetTickCount64() - m.finishedAt) / 60000;
    const auto age = minutes ? std::format("{} min ago", minutes) : std::string("just now");
    ImGui::Text("Real cost");
    ImGui::SameLine();
    ImGui::TextDisabled("(GPU ms saved by leaving out, %s)", age.c_str());
    if (!ImGui::BeginTable("realCost", 3, ImGuiTableFlags_SizingFixedFit)) return;

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextDisabled("GPU frame with everything on");
    ImGui::TableNextColumn();
    ImGui::TableNextColumn();
    rightAligned(std::format("{:.2f}", m.steps.front().gpuFrame), true);

    for (std::size_t i = 1; i < m.steps.size(); ++i)
    {
        const auto saved = m.steps[i - 1].gpuFrame - m.steps[i].gpuFrame;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(m.steps[i].removed.c_str());
        ImGui::TableNextColumn();
        bar(saved / largest, Red, ImGui::GetFontSize() * 7);
        ImGui::TableNextColumn();
        rightAligned(std::format("{:.2f}", saved));
    }

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted("All of these");
    ImGui::TableNextColumn();
    ImGui::TableNextColumn();
    rightAligned(std::format("{:.2f}", m.steps.front().gpuFrame - m.steps.back().gpuFrame));
    ImGui::EndTable();
}
}

void drawDiagnostics()
{
    watch();
    const auto state = status();
    bool shown = state.overlay;
    if (ImGui::Checkbox("Show the costs over the game", &shown)) showOverlay(shown);
    tooltip("Keeps the cost table in the top right corner, also with this window closed. PhantomFX-performance.txt\n"
            "beside the log holds the same numbers meanwhile, rewritten every second.");
    ImGui::SameLine();
    ImGui::BeginDisabled(state.measurement.running);
    if (ImGui::Button("Measure the real cost")) measure();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Times the GPU frame with everything on, then leaves out one effect at a time, the upscaler last: about\n"
                          "4 seconds a step. The differences include what the timed passes miss, such as the game's SSAO that GTVBAO\n"
                          "leaves out. Hold still (photo mode) for steady numbers; your settings come back at the end. HDR takes a\n"
                          "restart, so it is not measured.");

    ImGui::Spacing();
    drawHeadline(state);
    drawCostTable(state.costs, true);
    ImGui::TextDisabled("Timed on the GPU, averaged over about half a second.");
    ImGui::Spacing();
    drawRealCost(state.measurement);
}

void drawOverlay()
{
    const auto state = status();
    if (!state.overlay) return;

    const auto& display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(display.x - 24, 24), ImGuiCond_Always, ImVec2(1, 0));
    ImGui::SetNextWindowBgAlpha(0.75f);
    constexpr auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings
        | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("Phantom FX costs###phantomFxCosts", nullptr, flags))
    {
        drawHeadline(state);
        drawCostTable(state.costs, false);
        drawRealCost(state.measurement);
    }

    ImGui::End();
}

// A heading, the frame, one line per effect with its passes, and the real cost.
std::string report(const Status& state)
{
    const auto& c = state.costs;
    std::string text = "Phantom FX GPU costs in milliseconds, averaged over about half a second. The timed passes miss what they change in "
                       "the game's own work; the real cost includes it.\n";
    text += std::format("Frame {:.2f} ms ({:.1f} fps); GPU frame {:.2f} ms; Phantom FX timed {:.2f} ms\n", c.frame, c.frame > 0 ? 1000 / c.frame : 0,
        c.gpuFrame, c.total());
    for (const auto& group : groups())
    {
        auto line = std::format("{} {:.2f}:", group.name, millisecondsOf(c, group));
        for (std::size_t i = 0; i < group.parts.size(); ++i)
            line += std::format("{} {} {:.2f}", i ? "," : "", group.parts[i].name, millisecondsOf(c, group.parts[i].pass));

        text += line + '\n';
    }

    const auto& m = state.measurement;
    if (m.running) text += std::format("Real cost: measuring, step {} of {}\n", m.step + 1, m.stepCount);
    if (m.steps.size() < 2)
    {
        if (!m.running) text += "Real cost: not measured\n";
        return text;
    }

    auto line = std::format("Real cost (GPU frame): everything on {:.2f}; leaving out", m.steps.front().gpuFrame);
    for (std::size_t i = 1; i < m.steps.size(); ++i)
        line += std::format("{} {} saves {:.2f}", i > 1 ? "," : "", m.steps[i].removed, m.steps[i - 1].gpuFrame - m.steps[i].gpuFrame);

    return text + line + std::format("; total {:.2f}\n", m.steps.front().gpuFrame - m.steps.back().gpuFrame);
}
}
