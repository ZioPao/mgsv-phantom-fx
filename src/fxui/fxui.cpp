// fxui: the frame budget settings window (fxui.hpp, README.md).
#define IMGUI_DEFINE_MATH_OPERATORS
#include "fxui.hpp"

#include <imgui.h>
#include <reshade.hpp> // Routes ImGui calls through ReShade's overlay.

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <numbers>
#include <ranges>
#include <string>
#include <unordered_set>
#include <utility>

namespace fxui
{
namespace
{
// Sizes in pixels at ImGui's default 13 px font; everything grows with ReShade's font size.
constexpr float FontBase = 13;
constexpr float RailWidth = 176, CompactRailWidth = 140, Gutter = 34, SummaryWidth = 540;
constexpr float ControlX = 160, ControlMin = 150, ControlMax = 250, ReadoutWidth = 76, ResetSize = 16, ColumnGap = 10;
constexpr float StateWidth = 100, BarWidth = 100, MsWidth = 52, ShareWidth = 40, ChevronSize = 16;
constexpr float RowName = 200; // The name column of cost rows, which do not stretch with the window
// Window widths where the layout changes: below WideWindow the rail narrows and drops its hotkeys,
// below NarrowWindow a dropdown of sections takes its place.
constexpr float WideWindow = 640, NarrowWindow = 500;
// Below these, an item's state and cost go under its name, and the budget drops its share column.
constexpr float OneLineHead = Gutter + 150 + 8 + StateWidth + 56 + MsWidth, FullLedger = 480;
// One millisecond on every cost bar in the window.
constexpr float Millisecond = 48;
constexpr double FlashSeconds = 1.6;

// What the window keeps between frames.
struct Navigation
{
    int page{};
    char search[64]{};
    bool resetScroll{};
    std::string goTo;     // Asked for; the next window chooses its page
    std::string scrollTo; // Scrolled to as it is drawn
    std::string flash;    // Flashing since flashStart, as tall as the frame before drew it
    double flashStart{};
    float flashHeight{};
    std::string hot, hotNext; // The line under the mouse, which the frame bar and the rail light
    std::unordered_set<std::string> open; // Budget rows showing their passes
};

// A setting's row: label, control, readout and reset side by side; or, where that does not fit,
// stacked: the label with the readout and reset on its right, and the control under them, full width.
struct FieldLayout
{
    ImVec2 at;
    bool stacked{};
    ImVec2 control, readout, reset;
    float controlWidth{}, wide{}; // `wide`: a control without a readout takes the readout's place too
    float bottom{};
};

// What one frame of the window draws with.
struct Frame
{
    std::span<const Section> sections;
    const Budget* budget{};
    const char* name{};
    const char* masterKey{};
    float unit{1};
    float windowLeft{}, windowRight{}, windowBottom{};
    // The layout for the window's width.
    bool picker{}; // A dropdown of sections in place of the rail
    bool keys{};   // The rail shows the hotkeys
    float railWidth{};
    ImVec2 pagePadding;
    bool overlay{}; // Drawing the costs over the game: rows keep their width
    bool searching{}, matched{};
    int page{-1};
    bool headingPending{}, firstHeading{true};
    bool inItem{};
    FieldLayout combo; // The field of the open combo, until endCombo()
};

Navigation nav;
Frame frame;

float px(float value)
{
    return std::round(value * frame.unit);
}

// With the style's alpha, so disabled controls fade like ImGui's own.
ImU32 tint(ImU32 colour, float alpha = 1)
{
    return ImGui::GetColorU32(colour, alpha);
}

ImU32 withAlpha(ImU32 colour, float alpha)
{
    const auto a = static_cast<ImU32>(std::clamp(alpha, 0.0f, 1.0f) * ((colour >> IM_COL32_A_SHIFT) & 0xff));

    return (colour & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}

ImDrawList* drawList()
{
    return ImGui::GetWindowDrawList();
}

// During a search only items show.
bool hidden()
{
    return frame.searching && !frame.inItem;
}

float textWidth(std::string_view text)
{
    return ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
}

void addText(ImVec2 at, ImU32 colour, std::string_view text)
{
    drawList()->AddText(at, tint(colour), text.data(), text.data() + text.size());
}

void textLine(std::string_view text, ImU32 colour)
{
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    ImGui::PopStyleColor();
}

// Wrapped at `width` from the cursor.
void wrapped(std::string_view text, ImU32 colour, float width)
{
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::min(px(width), ImGui::GetContentRegionAvail().x));
    textLine(text, colour);
    ImGui::PopTextWrapPos();
}

std::string formatted(const char* format, float value)
{
    char text[64];
    std::snprintf(text, sizeof(text), format, value);

    return text;
}

std::string milliseconds(float ms)
{
    // Timing noise that rounds to nothing reads as nothing.
    return ms >= 0.005f ? formatted("%.2f", ms) : "-";
}

// Whether `text` holds every word of `search`, in any case.
bool containsWords(std::string_view text, std::string_view search)
{
    const auto same = [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); };
    for (const auto word : std::views::split(search, ' '))
        if (!word.empty() && std::ranges::search(text, word, same).empty()) return false;

    return true;
}

ImU32 toneColour(Tone tone)
{
    switch (tone)
    {
    case Tone::Ok: return colour::Ok;
    case Tone::Warn: return colour::Warn;
    case Tone::Bad: return colour::Bad;
    default: return colour::TextDim;
    }
}

Tone toneOf(State state)
{
    switch (state)
    {
    case State::Working: return Tone::Ok;
    case State::Restart:
    case State::NotRunning: return Tone::Warn;
    case State::Unavailable: return Tone::Bad;
    default: return Tone::Dim;
    }
}

std::string stateWord(State state)
{
    switch (state)
    {
    case State::Working: return "Working";
    case State::Held: return frame.masterKey ? std::string("Off (") + frame.masterKey + ")" : "Held off";
    case State::Replaced: return "Replaced";
    case State::Restart: return "Restart";
    case State::NotRunning: return "Not running";
    case State::Unavailable: return "Unavailable";
    default: return "Off";
    }
}

// A square dot and the state's word; hollow while it does nothing. Without the word, the dot tells it
// on hover.
void drawState(ImVec2 at, State state, bool word = true)
{
    const ImU32 colour = toneColour(toneOf(state));
    const float dot = px(5), top = at.y + std::round((ImGui::GetFontSize() - dot) / 2);
    const bool idle = state == State::Off || state == State::Held || state == State::Replaced;
    if (idle) drawList()->AddRect({at.x, top}, {at.x + dot, top + dot}, tint(colour));
    else drawList()->AddRectFilled({at.x, top}, {at.x + dot, top + dot}, tint(colour));
    if (word)
    {
        addText({at.x + dot + px(6), at.y}, colour, stateWord(state));
        return;
    }

    if (!ImGui::IsMouseHoveringRect({at.x - px(4), at.y}, {at.x + dot + px(4), at.y + ImGui::GetFontSize()})) return;

    ImGui::BeginTooltip();
    textLine(stateWord(state), colour);
    ImGui::EndTooltip();
}

// The right-hand columns of items and budget rows. A narrow window gets a shorter bar column; the
// scale stays, so only the longest bars stop short.
struct Columns
{
    float state{}, bar{}, ms{};
};

Columns columnsFor(float width)
{
    return {px(StateWidth), px(width < px(600) ? 56 : BarWidth), px(MsWidth)};
}

// A bar on the window's one scale, at most `maxWidth` long, centred in a row from `at`.
void costBar(ImVec2 at, float rowHeight, float ms, ImU32 colour, float maxWidth, float height = 5)
{
    const float width = std::min(ms * px(Millisecond), maxWidth), thickness = px(height);
    if (width < 1) return;

    const float top = at.y + std::round((rowHeight - thickness) / 2);
    drawList()->AddRectFilled({at.x, top}, {at.x + std::max(width, 2.0f), top + thickness}, tint(colour));
}

void rightText(float right, float y, std::string_view text, ImU32 colour)
{
    addText({right - textWidth(text), y}, colour, text);
}

float keycap(ImVec2 at, const char* key)
{
    const float width = std::max(px(22), textWidth(key) + px(8)), height = ImGui::GetFontSize() + px(3);
    drawList()->AddRectFilled(at, {at.x + width, at.y + height}, tint(colour::Frame));
    addText({at.x + std::round((width - textWidth(key)) / 2), at.y + px(2)}, colour::Text, key);

    return at.x + width;
}

// A switch: a rounded track with a knob, light while on.
bool toggleSwitch(const char* id, bool& value, bool enabled)
{
    const ImVec2 at = ImGui::GetCursorScreenPos(), size{px(26), px(14)};
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    if (pressed) value = !value;

    const float radius = size.y / 2;
    const ImU32 track = value ? (hovered ? IM_COL32_WHITE : colour::Text) : (hovered ? colour::TextFaint : colour::FrameActive);
    drawList()->AddRectFilled(at, at + size, tint(track), radius);
    const ImVec2 knob{value ? at.x + size.x - radius : at.x + radius, at.y + radius};
    drawList()->AddCircleFilled(knob, px(4), tint(value ? colour::Ground : colour::TextDim));
    ImGui::EndDisabled();

    return pressed;
}

// A circular arrow that puts a setting back to its default.
bool resetButton(ImVec2 at)
{
    const float size = px(ResetSize);
    ImGui::SetCursorScreenPos(at);
    const bool pressed = ImGui::InvisibleButton("##reset", {size, size});
    const ImU32 colour = tint(ImGui::IsItemHovered() ? colour::Text : colour::TextDim);
    const ImVec2 centre = at + ImVec2(size / 2, size / 2);
    const float radius = px(4.5f), thickness = std::max(1.0f, px(1.2f));
    auto* list = drawList();
    list->PathArcTo(centre, radius, -std::numbers::pi_v<float> / 2, std::numbers::pi_v<float>, 12);
    list->PathStroke(colour, ImDrawFlags_None, thickness);
    const ImVec2 tip{centre.x - radius, centre.y - px(3)};
    list->AddTriangleFilled(tip, {tip.x - px(2.5f), tip.y + px(3.5f)}, {tip.x + px(2.5f), tip.y + px(3.5f)}, colour);

    return pressed;
}

void dottedUnderline(ImVec2 min, ImVec2 max)
{
    const float dot = std::max(1.0f, px(1)), y = max.y + px(2);
    for (float x = min.x; x < max.x; x += px(3)) drawList()->AddRectFilled({x, y}, {x + dot, y + dot}, tint(colour::TextFaint));
}

// ---- The window's parts ----

constexpr std::pair<ImGuiCol, ImU32> styleColours[]{
    {ImGuiCol_Text, colour::Text},
    {ImGuiCol_TextDisabled, colour::TextDim},
    {ImGuiCol_FrameBg, colour::Frame},
    {ImGuiCol_FrameBgHovered, colour::FrameHover},
    {ImGuiCol_FrameBgActive, colour::FrameActive},
    {ImGuiCol_Button, colour::Frame},
    {ImGuiCol_ButtonHovered, colour::FrameHover},
    {ImGuiCol_ButtonActive, colour::FrameActive},
    {ImGuiCol_Header, colour::FrameActive},
    {ImGuiCol_HeaderHovered, colour::FrameHover},
    {ImGuiCol_HeaderActive, colour::FrameActive},
    {ImGuiCol_PopupBg, colour::Popup},
    {ImGuiCol_Border, colour::LineStrong},
    {ImGuiCol_ChildBg, 0},
    {ImGuiCol_CheckMark, colour::Ground},
    {ImGuiCol_SliderGrab, colour::Text},
    {ImGuiCol_SliderGrabActive, IM_COL32_WHITE},
    {ImGuiCol_ScrollbarBg, 0},
    {ImGuiCol_ScrollbarGrab, colour::FrameActive},
    {ImGuiCol_ScrollbarGrabHovered, colour::TextFaint},
    {ImGuiCol_ScrollbarGrabActive, colour::TextDim},
    {ImGuiCol_Separator, colour::Line},
    {ImGuiCol_TextLink, colour::Text},
    {ImGuiCol_TextSelectedBg, IM_COL32(0xe6, 0xe4, 0xde, 0x48)},
    {ImGuiCol_NavCursor, colour::Text},
};

void pushStyle()
{
    const ImVec2 framePadding{px(6), px(3)};
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, framePadding);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(8), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(px(8), px(4)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(8), px(6)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, px(10));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 0.0f);

    for (const auto& [index, value] : styleColours) ImGui::PushStyleColor(index, value);
}

constexpr int StyleVars = 13;

// Our ground over ReShade's window, below its title bar.
void paintGround()
{
    const ImVec2 position = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
    const float border = ImGui::GetStyle().WindowBorderSize;
    const float top = ImGui::GetCursorScreenPos().y - ImGui::GetStyle().WindowPadding.y;
    frame.windowLeft = position.x + border;
    frame.windowRight = position.x + size.x - border;
    frame.windowBottom = position.y + size.y - border;

    auto* list = drawList();
    list->PushClipRect({frame.windowLeft, top}, {frame.windowRight, frame.windowBottom}, false);
    list->AddRectFilled({frame.windowLeft, top}, {frame.windowRight, frame.windowBottom}, colour::Ground);
    list->PopClipRect();
}

// One rendered frame, from present to present: the GPU's work in it (the game's own, then each line
// in order), then the time the GPU waits for the next frame, hatched. Hovering a slice lights its line;
// clicking it goes to its item.
void frameBar(ImVec2 min, ImVec2 max)
{
    const Budget& budget = *frame.budget;
    const float total = budget.total(), game = std::max(0.0f, budget.gpuMs - total);
    // The GPU's frame can outlast the time between presents, as frames overlap on it.
    const float idle = std::max(0.0f, budget.frameMs - budget.gpuMs);
    const float scale = std::max({budget.frameMs, budget.gpuMs, 0.1f}), width = max.x - min.x;
    const auto x = [&](float ms) { return min.x + std::round(ms / scale * width); };

    ImGui::SetCursorScreenPos(min);
    ImGui::InvisibleButton("##fxui-frame", max - min);
    const bool hovered = ImGui::IsItemHovered(), clicked = ImGui::IsItemClicked();
    const float mouse = ImGui::GetIO().MousePos.x;

    enum class Kind { Game, Line, Idle };
    struct Slice { float from, to; ImU32 colour; Kind kind; const Line* line; };
    std::vector<Slice> slices;
    float at = 0;
    const auto slice = [&](float ms, ImU32 colour, Kind kind, const Line* line) {
        if (ms <= 0) return;

        const float from = x(at);
        slices.push_back({from, std::max(from + 2, x(at + ms) - (kind == Kind::Idle ? 0 : 1)), colour, kind, line});
        at += ms;
    };
    slice(game, colour::Game, Kind::Game, nullptr);
    for (const auto& line : budget.lines) slice(line.ms, frame.sections[line.section].hue ? frame.sections[line.section].hue : colour::TextDim, Kind::Line, &line);
    slice(idle, colour::LineStrong, Kind::Idle, nullptr);

    const Slice* under{};
    if (hovered)
        for (const auto& candidate : slices)
            if (mouse >= candidate.from && mouse < candidate.to + 1) under = &candidate;
    if (under && under->line) nav.hotNext = nav.hot = under->line->id;

    auto* list = drawList();
    list->AddRectFilled(min, max, tint(colour::Rail));
    for (const auto& candidate : slices)
    {
        const bool lit = under ? &candidate == under : nav.hot.empty() || (candidate.line && candidate.line->id == nav.hot);
        if (candidate.kind != Kind::Idle)
        {
            list->AddRectFilled({candidate.from, min.y}, {candidate.to, max.y}, tint(candidate.colour, lit ? 1.0f : 0.3f));
            continue;
        }

        // Diagonal hatching: time the frame takes that the GPU does not use.
        list->PushClipRect({candidate.from, min.y}, {candidate.to, max.y}, true);
        const float height = max.y - min.y, step = px(6), thickness = std::max(1.0f, px(1));
        for (float hatch = candidate.from - height; hatch < candidate.to; hatch += step)
            list->AddLine({hatch, max.y}, {hatch + height, min.y}, tint(candidate.colour, lit ? 1.0f : 0.5f), thickness);
        list->PopClipRect();
    }
    // The frame's end.
    const float frameEnd = x(budget.frameMs);
    if (budget.frameMs > 0) list->AddRectFilled({frameEnd, min.y - px(3)}, {frameEnd + 1, max.y + px(3)}, tint(colour::TextFaint));

    if (under)
    {
        ImGui::BeginTooltip();
        const auto share = [&](float ms) { return budget.frameMs > 0 ? formatted("%.0f%%", ms / budget.frameMs * 100) : std::string("-"); };
        if (under->kind == Kind::Line)
        {
            std::string name(under->line->name);
            if (!under->line->tech.empty()) name += " (" + std::string(under->line->tech) + ")";
            textLine(name, colour::Text);
            textLine(formatted("%.2f ms, ", under->line->ms) + share(under->line->ms) + " of the frame. Click to go to it.", colour::TextDim);
        }
        else if (under->kind == Kind::Game)
        {
            textLine("The game's own GPU work", colour::Text);
            textLine(formatted("%.2f ms, ", game) + share(game) + " of the frame.", colour::TextDim);
        }
        else
        {
            textLine("GPU idle", colour::Text);
            textLine(formatted("%.2f ms, ", idle) + share(idle) + " of the frame: the GPU waits for the next one. The CPU or a frame rate cap sets the frame rate.",
                colour::TextDim);
        }
        ImGui::EndTooltip();
        if (under->line && clicked) goTo(under->line->id);
    }

    // Labels under the bar, the frame's length first; later ones give way where they would overlap.
    struct Label { float x; std::string text; ImU32 colour; float anchor; };
    std::vector<Label> labels;
    if (budget.frameMs > 0) labels.push_back({frameEnd, formatted("%.2f ms a frame", budget.frameMs), colour::TextDim, 1});
    if (game > 0) labels.push_back({min.x, "Game", colour::TextDim, 0});
    if (total > 0) labels.push_back({x(game), frame.name, colour::TextDim, 0});
    if (idle > 0) labels.push_back({x(budget.gpuMs), "Idle", colour::TextFaint, 0});
    std::vector<std::pair<float, float>> taken;
    for (const auto& label : labels)
    {
        const float textW = textWidth(label.text), left = std::round(label.x - textW * label.anchor);
        const bool overlaps = std::ranges::any_of(taken, [&](const auto& span) { return left < span.second + px(8) && left + textW + px(8) > span.first; });
        if (overlaps) continue;

        taken.push_back({left, left + textW});
        addText({left, max.y + px(6)}, label.colour, label.text);
    }
}

void drawStrip(const Chrome& chrome)
{
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float left = start.x + px(6), right = start.x + ImGui::GetContentRegionAvail().x - px(6);
    const float top = start.y + px(2);

    float masterRight = left;
    if (chrome.master)
    {
        ImGui::SetCursorScreenPos({left, top});
        toggleSwitch("##fxui-master", *chrome.master, true);
        ImGui::SetCursorScreenPos({left + px(Gutter), top + px(1)});
        textLine(chrome.masterLabel ? chrome.masterLabel : "", colour::Text);
        tooltip(chrome.masterTip);
        masterRight = ImGui::GetItemRectMax().x;
        if (chrome.masterKey) masterRight = keycap({masterRight + px(8), top - px(1)}, chrome.masterKey);
    }

    // "Effects 4.07 ms of 9.12 ms GPU · 107 fps" against the right edge; shorter where the switch
    // leaves too little room, and on a line of its own where even that does not fit.
    using Readout = std::vector<std::pair<std::string, ImU32>>;
    const Budget& budget = *frame.budget;
    // With frame generation, the frames shown, and the ones rendered beside them.
    const float rendered = budget.frameMs > 0 ? 1000 / budget.frameMs : 0;
    const auto fps = rendered > 0 ? formatted("%.0f fps", rendered * static_cast<float>(budget.presented)) : std::string("- fps");
    Readout full{{std::string(frame.name) + " ", colour::TextDim}, {formatted("%.2f ms", budget.total()), colour::Text}, {" of ", colour::TextDim},
        {formatted("%.2f ms", budget.gpuMs), colour::Text}, {" GPU \xC2\xB7 ", colour::TextDim}, {fps, colour::Text}};
    if (budget.presented > 1 && rendered > 0) full.push_back({formatted(" (%.0f rendered)", rendered), colour::TextDim});
    const Readout brief{{formatted("%.2f", budget.total()), colour::Text}, {" / ", colour::TextDim}, {formatted("%.2f ms", budget.gpuMs), colour::Text},
        {" \xC2\xB7 ", colour::TextDim}, {fps, colour::Text}};
    const auto widthOf = [](const Readout& readout) {
        float width{};
        for (const auto& [text, colour] : readout) width += textWidth(text);
        return width;
    };
    const float room = right - masterRight - px(16);
    const Readout& readout = widthOf(full) <= room ? full : brief;
    const bool ownLine = widthOf(readout) > room;
    const float readoutTop = ownLine ? top + px(15 + 6) : top;
    float x = ownLine ? left : right - widthOf(readout);
    for (const auto& [text, colour] : readout)
    {
        addText({x, readoutTop + px(1)}, colour, text);
        x += textWidth(text);
    }
    if (chrome.master && !*chrome.master && chrome.masterOff)
    {
        const float offRight = ownLine ? right : right - widthOf(readout) - px(16);
        if (masterRight + px(8) + textWidth(chrome.masterOff) <= offRight) addText({masterRight + px(8), top + px(1)}, colour::Warn, chrome.masterOff);
    }

    const float barTop = readoutTop + px(15 + 11);
    frameBar({left, barTop}, {right, barTop + px(12)});

    const float rule = barTop + px(12 + 6 + 13 + 8);
    auto* list = drawList();
    list->PushClipRect({frame.windowLeft, start.y}, {frame.windowRight, frame.windowBottom}, false);
    list->AddRectFilled({frame.windowLeft, rule}, {frame.windowRight, rule + 1}, colour::Line);
    list->PopClipRect();
    ImGui::SetCursorScreenPos(start);
    ImGui::Dummy({1, rule + 1 - start.y});
}

void searchBox(float width)
{
    const float side = ImGui::GetFrameHeight();
    ImGui::SetNextItemWidth(width - side - 1);
    if (ImGui::InputTextWithHint("##fxui-search", "Search settings", nav.search, sizeof(nav.search))) nav.resetScroll = true;
    ImGui::SameLine(0, 1);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const bool searching = nav.search[0] != 0;
    if (ImGui::InvisibleButton("##fxui-clear", {side, side}) && searching)
    {
        nav.search[0] = 0;
        nav.resetScroll = true;
    }
    const bool hovered = ImGui::IsItemHovered();
    drawList()->AddRectFilled(at, at + ImVec2(side, side), tint(hovered && searching ? colour::FrameHover : colour::Frame));
    if (searching)
    {
        const ImVec2 centre = at + ImVec2(side / 2, side / 2);
        const float arm = px(3.5f), thickness = std::max(1.0f, px(1.2f));
        const ImU32 colour = tint(hovered ? colour::Text : colour::TextDim);
        drawList()->AddLine(centre - ImVec2(arm, arm), centre + ImVec2(arm, arm), colour, thickness);
        drawList()->AddLine(centre + ImVec2(-arm, arm), centre + ImVec2(arm, -arm), colour, thickness);
        tooltip("Clear the search");
    }
}

// A section in the rail or the sections dropdown: its name, its milliseconds, and its bar on the
// window's scale. True when chosen.
bool navItem(int index)
{
    const Section& section = frame.sections[static_cast<std::size_t>(index)];
    const Budget& budget = *frame.budget;
    const float width = ImGui::GetContentRegionAvail().x, height = px(33);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::PushID(index);
    const bool chosen = ImGui::InvisibleButton("##section", {width, height});
    if (chosen)
    {
        if (nav.page != index || nav.search[0]) nav.resetScroll = true;
        nav.page = index;
        nav.search[0] = 0;
    }
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();

    const Line* hotLine = nav.hot.empty() ? nullptr : budget.find(nav.hot);
    const bool current = nav.page == index && !nav.search[0];
    const bool lit = hovered || (hotLine && hotLine->section == index);
    auto* list = drawList();
    if (current) list->AddRectFilled(at, at + ImVec2(width, height), tint(colour::Frame));
    else if (lit) list->AddRectFilled(at, at + ImVec2(width, height), tint(colour::Raise));

    const float inner = at.x + px(8), innerRight = at.x + width - px(8), barTop = at.y + px(25);
    addText({inner, at.y + px(6)}, current || lit ? colour::Text : colour::TextDim, section.name);
    if (section.overview || section.hue)
    {
        const float ms = section.overview ? budget.total() : budget.total(index);
        rightText(innerRight, at.y + px(6), milliseconds(ms), current || lit ? colour::TextDim : colour::TextFaint);
    }

    const float barMax = innerRight - inner;
    const float alpha = current || lit ? 1.0f : 0.5f;
    if (section.overview)
    {
        // Every section's share of the effects' total, side by side.
        const float total = budget.total();
        float x = inner;
        for (int other = 0; other < static_cast<int>(frame.sections.size()) && total > 0; ++other)
        {
            const float ms = budget.total(other);
            if (ms <= 0 || !frame.sections[static_cast<std::size_t>(other)].hue) continue;

            const float to = std::min(innerRight, x + std::max(2.0f, std::round(ms / total * barMax) - 1));
            list->AddRectFilled({x, barTop}, {to, barTop + px(2)}, tint(frame.sections[static_cast<std::size_t>(other)].hue, alpha));
            x = to + 1;
        }
    }
    else if (section.hue)
    {
        const float bar = std::min(budget.total(index) * px(Millisecond), barMax);
        if (bar >= 1) list->AddRectFilled({inner, barTop}, {inner + std::max(2.0f, bar), barTop + px(2)}, tint(section.hue, alpha));
    }

    return chosen;
}

// Every section, with a rule above those set apart.
void navItems(bool closePopup)
{
    for (int index = 0; index < static_cast<int>(frame.sections.size()); ++index)
    {
        if (frame.sections[static_cast<std::size_t>(index)].apart)
        {
            gap(4);
            const ImVec2 at = ImGui::GetCursorScreenPos();
            drawList()->AddRectFilled(at, {at.x + ImGui::GetContentRegionAvail().x, at.y + 1}, tint(colour::Line));
            gap(5);
        }
        if (navItem(index) && closePopup) ImGui::CloseCurrentPopup();
    }
}

// In a narrow window: the sections in a dropdown beside the search, over the full-width page.
void drawPicker()
{
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float left = start.x + px(6), right = start.x + ImGui::GetContentRegionAvail().x - px(6), top = start.y + px(8);
    const auto page = static_cast<std::size_t>(nav.page);
    const Section& section = frame.sections[page];
    std::string preview = nav.search[0] ? "Search results" : section.name;
    if (!nav.search[0] && (section.overview || section.hue))
        preview += "  " + milliseconds(section.overview ? frame.budget->total() : frame.budget->total(nav.page)) + " ms";

    const float comboWidth = std::round((right - left) * 0.55f);
    ImGui::SetCursorScreenPos({left, top});
    ImGui::SetNextItemWidth(comboWidth);
    if (ImGui::BeginCombo("##fxui-sections", preview.c_str(), ImGuiComboFlags_HeightLargest))
    {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(8), px(2)));
        navItems(true);
        ImGui::PopStyleVar();
        ImGui::EndCombo();
    }
    ImGui::SetCursorScreenPos({left + comboWidth + px(6), top});
    searchBox(right - left - comboWidth - px(6));

    const float rule = top + ImGui::GetFrameHeight() + px(8);
    auto* list = drawList();
    list->PushClipRect({frame.windowLeft, start.y}, {frame.windowRight, frame.windowBottom}, false);
    list->AddRectFilled({frame.windowLeft, rule}, {frame.windowRight, rule + 1}, colour::Line);
    list->PopClipRect();
    ImGui::SetCursorScreenPos(start);
    ImGui::Dummy({1, rule + 1 - start.y});
}

void drawRail(std::span<const Key> keys)
{
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float width = frame.railWidth;
    auto* list = drawList();
    list->PushClipRect({frame.windowLeft, start.y}, {frame.windowRight, frame.windowBottom}, false);
    list->AddRectFilled({frame.windowLeft, start.y}, {start.x + width, frame.windowBottom}, colour::Rail);
    list->AddRectFilled({start.x + width, start.y}, {start.x + width + 1, frame.windowBottom}, colour::Line);
    list->PopClipRect();

    // The rail reaches the window's edge, so its padding makes up what ReShade's leaves.
    const float padding = std::max(0.0f, px(8) - (start.x - frame.windowLeft));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padding, px(10)));
    ImGui::BeginChild("##fxui-rail", {width, 0}, ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(8), px(2)));

    searchBox(ImGui::GetContentRegionAvail().x);
    gap(6);
    navItems(false);

    // The hotkeys against the rail's foot.
    if (frame.keys && !keys.empty())
    {
        const float row = ImGui::GetFontSize() + px(3) + px(6);
        const float height = row * static_cast<float>(keys.size()) + px(4);
        const float bottom = ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - px(10);
        if (ImGui::GetCursorScreenPos().y < bottom - height) ImGui::SetCursorScreenPos({ImGui::GetCursorScreenPos().x, bottom - height});
        for (const auto& key : keys)
        {
            const ImVec2 at = ImGui::GetCursorScreenPos() + ImVec2(px(8), 0);
            const float x = keycap(at, key.key);
            addText({x + px(6), at.y + px(2)}, colour::TextDim, key.action);
            ImGui::Dummy({1, row - px(2)});
        }
    }

    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::SetCursorScreenPos({start.x + width + 1, start.y});
}

void beginPages()
{
    if (std::exchange(nav.resetScroll, false)) ImGui::SetNextWindowScroll({0, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, frame.pagePadding);
    ImGui::BeginChild("##fxui-page", {0, 0}, ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
}

// A page's title at twice the size, with its section's milliseconds, then its description.
void heading(int index, const char* description)
{
    const Section& section = frame.sections[static_cast<std::size_t>(index)];
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float right = at.x + ImGui::GetContentRegionAvail().x;
    const float size = ImGui::GetFontSize(), big = size * 2;
    ImFont* font = ImGui::GetFont();
    drawList()->AddText(font, big, at, tint(colour::Text), section.name);
    ImGui::Dummy({1, big});

    // The total at the title's size against the right edge, or under the title where they would meet.
    if (section.overview || section.hue)
    {
        const Budget& budget = *frame.budget;
        const auto total = formatted("%.2f", section.overview ? budget.total() : budget.total(index));
        const float unitWidth = textWidth("ms");
        // The font scales evenly, so text is twice as wide at twice the size.
        const float number = textWidth(total) * 2, title = textWidth(section.name) * 2;
        if (at.x + title + px(16) + number + px(6) + unitWidth <= right)
        {
            drawList()->AddText(font, big, {right - unitWidth - px(6) - number, at.y}, tint(colour::Text), total.c_str());
            // On the number's baseline.
            addText({right - unitWidth, at.y + std::round(ImGui::GetFontBaked()->Ascent)}, colour::TextDim, "ms");
        }
        else
        {
            gap(4);
            textLine(total + " ms", colour::TextDim);
        }
    }

    if (description)
    {
        gap(9);
        wrapped(description, colour::TextDim, SummaryWidth);
    }
    gap(16);
}

// During a search: the page's name over its first matching item.
void searchHeading()
{
    frame.headingPending = false;
    if (!std::exchange(frame.firstHeading, false)) gap(18);
    textLine(frame.sections[static_cast<std::size_t>(frame.page)].name, colour::TextDim);
    gap(8);
}

bool matches(const ItemSpec& spec)
{
    std::string text;
    for (const char* part : {spec.name, spec.tech, spec.tag, spec.summary, spec.keywords, frame.sections[static_cast<std::size_t>(frame.page)].name})
        if (part) (text += part) += ' ';

    return containsWords(text, nav.search);
}

// Settings stack where a row would squeeze the control below its least width.
bool stacked(float avail)
{
    return avail < px(ControlX + ControlMin + ColumnGap + ReadoutWidth + ColumnGap + ResetSize);
}

FieldLayout layoutField()
{
    FieldLayout field;
    field.at = ImGui::GetCursorScreenPos();
    const float avail = ImGui::GetContentRegionAvail().x, height = ImGui::GetFrameHeight();
    field.stacked = stacked(avail);
    if (!field.stacked)
    {
        field.controlWidth = std::min(avail - px(ControlX + ColumnGap + ReadoutWidth + ColumnGap + ResetSize), px(ControlMax));
        field.control = {field.at.x + px(ControlX), field.at.y};
        field.readout = {field.control.x + field.controlWidth + px(ColumnGap), field.at.y};
        field.reset = {field.readout.x + px(ReadoutWidth + ColumnGap), field.at.y};
        field.wide = field.controlWidth + px(ColumnGap + ReadoutWidth);
        field.bottom = field.at.y + height;
        return field;
    }

    const float right = field.at.x + avail;
    field.reset = {right - px(ResetSize), field.at.y};
    field.readout = {field.reset.x - px(ColumnGap + ReadoutWidth), field.at.y};
    field.control = {field.at.x, field.at.y + height + px(4)};
    field.controlWidth = field.wide = avail;
    field.bottom = field.control.y + height;

    return field;
}

// The label column; a square in the gutter marks a setting that is not at its default.
void fieldLabel(const FieldLayout& field, const char* label, const char* tip, bool changed)
{
    if (changed) drawList()->AddRectFilled({field.at.x - px(12), field.at.y + px(8)}, {field.at.x - px(8), field.at.y + px(12)}, tint(colour::Text));
    ImGui::SetCursorScreenPos({field.at.x, field.at.y + ImGui::GetStyle().FramePadding.y});
    textLine(label, changed ? colour::Text : colour::TextDim);
    tooltip(tip);
}

void endField(const FieldLayout& field)
{
    ImGui::SetCursorScreenPos(field.at);
    ImGui::Dummy({1, field.bottom - field.at.y});
    gap(field.stacked ? 10.0f : 6.0f);
}
}

// ---- Budget ----

float Budget::total() const
{
    float sum{};
    for (const auto& line : lines) sum += line.ms;

    return sum;
}

float Budget::total(int section) const
{
    float sum{};
    for (const auto& line : lines)
        if (line.section == section) sum += line.ms;

    return sum;
}

const Line* Budget::find(std::string_view id) const
{
    const auto line = std::ranges::find(lines, id, &Line::id);

    return line == lines.end() ? nullptr : &*line;
}

// ---- Window ----

Window::Window(std::span<const Section> sections, const Budget& budget, const Chrome& chrome)
{
    frame = {};
    frame.sections = sections;
    frame.budget = &budget;
    frame.name = chrome.name ? chrome.name : "Effects";
    frame.masterKey = chrome.masterKey;
    frame.unit = ImGui::GetFontSize() / FontBase;
    frame.searching = nav.search[0] != 0;
    nav.hot = std::exchange(nav.hotNext, {});

    if (!nav.goTo.empty())
    {
        if (const Line* line = budget.find(nav.goTo); line && line->section != nav.page)
        {
            nav.page = line->section;
            nav.resetScroll = true;
        }
        nav.search[0] = 0;
        nav.scrollTo = nav.flash = std::exchange(nav.goTo, {});
        nav.flashStart = ImGui::GetTime();
        nav.flashHeight = 0;
    }
    nav.page = std::clamp(nav.page, 0, std::max(0, static_cast<int>(sections.size()) - 1));

    // The layout for the window's width.
    const float width = ImGui::GetContentRegionAvail().x;
    frame.picker = width < px(NarrowWindow);
    frame.keys = width >= px(WideWindow);
    frame.railWidth = px(frame.keys ? RailWidth : CompactRailWidth);
    frame.pagePadding = frame.picker ? ImVec2(px(12), px(12)) : ImVec2(px(22), px(16));

    // ReShade's padding and border, before ours are pushed.
    paintGround();
    pushStyle();
    drawStrip(chrome);
    if (frame.picker) drawPicker();
    else drawRail(chrome.keys);
    frame.searching = nav.search[0] != 0;
    beginPages();
}

Window::~Window()
{
    if (frame.searching && !frame.matched) textLine("No setting matches that search.", colour::TextDim);
    ImGui::EndChild();
    ImGui::PopStyleColor(static_cast<int>(std::size(styleColours)));
    ImGui::PopStyleVar(StyleVars);
    frame.inItem = false;
    frame.page = -1;
}

// ---- Page ----

Page::Page(int section, const char* description)
{
    if (frame.page >= 0 || section < 0 || section >= static_cast<int>(frame.sections.size())) return;

    if (frame.searching)
    {
        // A search shows matching items from every page; the overview has none.
        if (frame.sections[static_cast<std::size_t>(section)].overview) return;

        shown = true;
        frame.page = section;
        frame.headingPending = true;
        return;
    }

    if (section != nav.page) return;

    shown = true;
    frame.page = section;
    heading(section, description);
}

Page::~Page()
{
    if (!shown) return;

    frame.page = -1;
    frame.headingPending = false;
}

// ---- Item ----

Item::Item(const ItemSpec& spec) : id(spec.id)
{
    if (frame.page < 0 || frame.inItem || !spec.id) return;
    if (frame.searching && !matches(spec)) return;

    if (frame.headingPending) searchHeading();
    drawn = true;
    frame.inItem = true;
    frame.matched = true;
    ImGui::PushID(spec.id);

    const Line* line = frame.budget->find(spec.id);
    bool* toggle = line ? line->toggle : spec.toggle;
    const std::optional<State> state = line ? std::optional(line->state) : spec.state;
    const std::string_view message = line ? std::string_view(line->message) : spec.message;
    const ImU32 hue = frame.sections[static_cast<std::size_t>(frame.page)].hue ? frame.sections[static_cast<std::size_t>(frame.page)].hue : colour::TextDim;
    const bool replaced = state == State::Replaced;
    held = state == State::Held;
    hot = line && line->ms > 0;

    const ImVec2 start = ImGui::GetCursorScreenPos();
    left = start.x;
    right = start.x + ImGui::GetContentRegionAvail().x;
    top = start.y;
    auto* list = drawList();
    if (nav.flash == spec.id)
    {
        const double t = (ImGui::GetTime() - nav.flashStart) / FlashSeconds;
        if (t < 1) list->AddRectFilled({left - px(8), top}, {right, top + nav.flashHeight}, tint(withAlpha(hue, 0.24f * static_cast<float>(std::pow(1 - t, 3)))));
        else nav.flash.clear();
    }
    if (nav.scrollTo == spec.id)
    {
        ImGui::SetScrollHereY(0.3f);
        nav.scrollTo.clear();
    }
    list->AddRectFilled({left, top}, {right, top + 1}, tint(colour::Line));
    gap(10);

    // The head: switch, name, technical name, tag, then state, cost and milliseconds on the right.
    const float row = ImGui::GetCursorScreenPos().y, rowHeight = px(15), textTop = row + px(1);
    if (toggle)
    {
        ImGui::SetCursorScreenPos({left, row});
        toggleSwitch("##on", *toggle, !held && !replaced);
    }
    // The name keeps clear of the state column; the technical name and tag show where they fit. In a
    // narrow page the state and cost go on a line of their own, under the name.
    const auto columns = columnsFor(right - left);
    const bool twoLines = right - left < px(OneLineHead) && (state || line);
    const float statusRow = twoLines ? row + rowHeight + px(5) : row;
    const float stateLeft = twoLines ? left + px(Gutter) : right - columns.state - columns.bar - columns.ms;
    const float nameRight = twoLines ? right : stateLeft - px(8);
    ImGui::PushClipRect({left, row - px(2)}, {nameRight, row + rowHeight + px(4)}, true);
    ImGui::SetCursorScreenPos({left + px(Gutter), textTop});
    const bool idle = state == State::Off || held || replaced;
    textLine(spec.name, idle ? colour::TextDim : colour::Text);
    if (spec.tip)
    {
        dottedUnderline(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        tooltip(spec.tip);
    }
    float x = ImGui::GetItemRectMax().x;
    if (spec.tech && x + px(8) + textWidth(spec.tech) <= nameRight)
    {
        addText({x + px(8), textTop}, colour::TextDim, spec.tech);
        x += px(8) + textWidth(spec.tech);
    }
    if (spec.tag && x + px(16) + textWidth(spec.tag) <= nameRight)
    {
        const float tagLeft = x + px(8), tagRight = tagLeft + textWidth(spec.tag) + px(8);
        list->AddRect({tagLeft, row - px(1)}, {tagRight, row + rowHeight}, tint(colour::LineStrong));
        addText({tagLeft + px(4), textTop}, colour::TextDim, spec.tag);
    }
    ImGui::PopClipRect();
    if (state) drawState({stateLeft, statusRow + px(1)}, *state);
    if (line)
    {
        const float barLeft = twoLines ? stateLeft + columns.state : right - columns.bar - columns.ms;
        const float barMax = twoLines ? right - columns.ms - barLeft : columns.bar - px(8);
        costBar({barLeft, statusRow}, rowHeight, line->ms, hue, barMax);
        rightText(right, statusRow + px(1), milliseconds(line->ms), line->ms >= 0.005f ? colour::Text : colour::TextFaint);
    }
    ImGui::SetCursorScreenPos({left, row});
    ImGui::Dummy({1, statusRow + rowHeight - row});

    // The summary, then what replaces it or why it does not run.
    if (spec.summary)
    {
        gap(7);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + px(Gutter));
        wrapped(spec.summary, colour::TextDim, SummaryWidth);
    }
    if (replaced && line && !line->replacedBy.empty())
    {
        gap(7);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + px(Gutter));
        const Line* by = frame.budget->find(line->replacedBy);
        textLine("Replaced by", colour::TextDim);
        ImGui::SameLine(0, textWidth(" "));
        link(by ? std::string(by->name).c_str() : std::string(line->replacedBy).c_str(), line->replacedBy);
        ImGui::SameLine(0, 0);
        textLine(" while on.", colour::TextDim);
    }
    if (!message.empty())
    {
        gap(7);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + px(Gutter));
        wrapped(message, toneColour(state ? toneOf(*state) : Tone::Dim), SummaryWidth);
    }

    open = !replaced && (!toggle || *toggle);
    if (!open) return;

    gap(11);
    ImGui::Indent(px(Gutter));
    if (held) ImGui::BeginDisabled();
}

Item::~Item()
{
    if (!drawn) return;

    if (open)
    {
        if (held) ImGui::EndDisabled();
        ImGui::Unindent(px(Gutter));
        gap(5);
    }
    else gap(11);

    const float bottom = ImGui::GetCursorScreenPos().y;
    if (nav.flash == id) nav.flashHeight = bottom - top;
    if (hot && ImGui::IsMouseHoveringRect({left, top}, {right, bottom})) nav.hotNext = id;
    frame.inItem = false;
    ImGui::PopID();
}

void item(const ItemSpec& spec)
{
    const Item drawn(spec);
}

// ---- Settings ----

bool slider(const char* label, float& value, const SliderSpec& spec)
{
    if (hidden()) return false;

    // The slider being dragged, for those that change the value only when released.
    static ImGuiID editing{};
    static float edited{};

    ImGui::PushID(label);
    const auto field = layoutField();
    const ImGuiID id = ImGui::GetID("##track");
    float shown = spec.onRelease && editing == id ? edited : value;
    const auto readout = [&](float v) { return spec.text ? spec.text(v) : formatted(spec.format, v * spec.shown); };
    const bool changed = spec.standard && readout(shown) != readout(*spec.standard);
    fieldLabel(field, label, spec.tip, changed);

    // Logarithmic sliders spread a wide range evenly, as ImGuiSliderFlags_Logarithmic does.
    const bool logarithmic = spec.logarithmic && spec.min > 0;
    const auto toPosition = [&](float v) {
        const float t = logarithmic ? std::log(v / spec.min) / std::log(spec.max / spec.min) : (v - spec.min) / (spec.max - spec.min);
        return std::clamp(t, 0.0f, 1.0f);
    };
    const auto fromPosition = [&](float t) { return logarithmic ? spec.min * std::pow(spec.max / spec.min, t) : spec.min + (spec.max - spec.min) * t; };

    const float height = ImGui::GetFrameHeight(), grab = px(5);
    ImGui::SetCursorScreenPos(field.control);
    ImGui::InvisibleButton("##track", {field.controlWidth, height});
    const bool active = ImGui::IsItemActive(), hovered = ImGui::IsItemHovered(), released = ImGui::IsItemDeactivated();
    if (active) shown = fromPosition(std::clamp((ImGui::GetIO().MousePos.x - field.control.x - grab / 2) / (field.controlWidth - grab), 0.0f, 1.0f));

    bool result{};
    if (spec.onRelease)
    {
        if (active)
        {
            editing = id;
            edited = shown;
        }
        else if (released && editing == id)
        {
            editing = 0;
            result = value != edited;
            value = edited;
        }
    }
    else if (active && shown != value)
    {
        value = shown;
        result = true;
    }

    // The track, filled to the grab.
    auto* list = drawList();
    const float centre = field.control.y + height / 2, track = std::max(2.0f, px(3)), trackTop = std::round(centre - track / 2);
    const float grabX = std::round(field.control.x + toPosition(shown) * (field.controlWidth - grab));
    list->AddRectFilled({field.control.x, trackTop}, {grabX, trackTop + track}, tint(colour::Text));
    list->AddRectFilled({grabX, trackTop}, {field.control.x + field.controlWidth, trackTop + track}, tint(colour::FrameActive));
    const float grabTop = std::round(centre - px(13) / 2);
    const ImVec2 grabMin{grabX, grabTop}, grabMax{grabX + grab, grabTop + px(13)};
    if (hovered || active) list->AddRectFilled(grabMin - ImVec2(px(3), px(3)), grabMax + ImVec2(px(3), px(3)), tint(colour::TextFaint));
    list->AddRectFilled(grabMin - ImVec2(px(2), px(2)), grabMax + ImVec2(px(2), px(2)), tint(colour::Ground));
    list->AddRectFilled(grabMin, grabMax, tint(hovered || active ? IM_COL32_WHITE : colour::Text));

    // The readout, then the reset while it differs from the default.
    list->AddRectFilled(field.readout, field.readout + ImVec2(px(ReadoutWidth), height), tint(colour::Frame));
    rightText(field.readout.x + px(ReadoutWidth) - px(6), field.readout.y + ImGui::GetStyle().FramePadding.y, readout(shown), colour::Text);
    if (changed)
    {
        if (resetButton({field.reset.x, field.reset.y + std::round((height - px(ResetSize)) / 2)}))
        {
            value = *spec.standard;
            editing = 0;
            result = true;
        }
        tooltip(("Back to " + readout(*spec.standard)).c_str());
    }

    endField(field);
    ImGui::PopID();

    return result;
}

int segmented(const char* label, int selected, std::span<const Option> options, const char* tip)
{
    if (hidden() || options.empty()) return -1;

    ImGui::PushID(label);
    const auto field = layoutField();
    fieldLabel(field, label, tip, false);

    const float height = ImGui::GetFrameHeight(), count = static_cast<float>(options.size());
    const float each = std::floor((field.wide - (count - 1)) / count);
    int pressed = -1;
    for (std::size_t index = 0; index < options.size(); ++index)
    {
        const auto& option = options[index];
        const ImVec2 at{field.control.x + static_cast<float>(index) * (each + 1), field.control.y};
        // The last button takes what the rounding left.
        const float width = index + 1 == options.size() ? field.control.x + field.wide - at.x : each;
        ImGui::PushID(static_cast<int>(index));
        ImGui::SetCursorScreenPos(at);
        ImGui::BeginDisabled(!option.enabled);
        if (ImGui::InvisibleButton("##option", {width, height})) pressed = static_cast<int>(index);
        const bool hovered = ImGui::IsItemHovered();
        const bool chosen = static_cast<int>(index) == selected;

        auto* list = drawList();
        list->AddRectFilled(at, at + ImVec2(width, height), tint(chosen ? colour::Text : hovered ? colour::FrameHover : colour::Frame));
        const ImU32 ink = chosen ? colour::Ground : hovered ? colour::Text : colour::TextDim;
        const float iconSize = option.icon ? px(12) : 0;
        const bool withLabel = option.label && (!option.icon || iconSize + px(6) + textWidth(option.label) <= width - px(4));
        const float iconGap = option.icon && withLabel ? px(6) : 0;
        const float content = iconSize + iconGap + (withLabel ? textWidth(option.label) : 0);
        float x = std::round(at.x + (width - content) / 2);
        if (option.icon)
        {
            option.icon(list, {x + iconSize / 2, at.y + height / 2}, px(5), tint(ink));
            x += iconSize + iconGap;
        }
        // Text that does not fit the button stops at its edge.
        ImGui::PushClipRect(at, at + ImVec2(width, height), true);
        if (withLabel) addText({std::max(x, at.x + px(4)), at.y + ImGui::GetStyle().FramePadding.y}, ink, option.label);
        ImGui::PopClipRect();
        ImGui::EndDisabled();
        tooltip(option.tip ? option.tip : withLabel ? nullptr : option.label);
        ImGui::PopID();
    }

    endField(field);
    ImGui::PopID();

    return pressed;
}

bool beginCombo(const char* label, const char* preview, const char* tip)
{
    if (hidden()) return false;

    const auto field = layoutField();
    fieldLabel(field, label, tip, false);
    ImGui::SetCursorScreenPos(field.control);
    ImGui::SetNextItemWidth(field.wide);
    ImGui::PushID(label);
    const bool open = ImGui::BeginCombo("##combo", preview, ImGuiComboFlags_HeightLarge);
    ImGui::PopID();
    if (!open)
    {
        endField(field);
        return false;
    }

    frame.combo = field;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(8), px(4)));

    return true;
}

bool option(const char* label, bool selected, bool enabled, const char* tip)
{
    const bool pressed = ImGui::Selectable(label, selected, enabled ? ImGuiSelectableFlags_None : ImGuiSelectableFlags_Disabled);
    if (selected && ImGui::IsWindowAppearing()) ImGui::SetItemDefaultFocus();
    tooltip(tip);

    return pressed;
}

void endCombo()
{
    ImGui::PopStyleVar();
    ImGui::EndCombo();
    endField(frame.combo);
}

std::optional<std::size_t> searchCombo(const char* label, const char* preview, std::span<const std::string_view> names, std::optional<std::size_t> selected,
    const char* tip)
{
    if (!beginCombo(label, preview, tip)) return std::nullopt;

    // One list is open at a time, and each opens with an empty search.
    static char search[64];
    const bool appearing = ImGui::IsWindowAppearing();
    if (appearing)
    {
        search[0] = 0;
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##search", "Search", search, sizeof(search));

    std::optional<std::size_t> picked;
    bool any{};
    ImGui::BeginChild("##names", {0, ImGui::GetTextLineHeightWithSpacing() * 16});
    for (std::size_t index = 0; index < names.size(); ++index)
    {
        if (!containsWords(names[index], search)) continue;

        any = true;
        const bool chosen = index == selected;
        ImGui::PushID(static_cast<int>(index));
        if (option(std::string(names[index]).c_str(), chosen)) picked = index;
        ImGui::PopID();
        if (chosen && appearing) ImGui::SetScrollHereY();
    }
    if (!any) textLine("Nothing matches.", colour::TextDim);
    ImGui::EndChild();
    if (picked) ImGui::CloseCurrentPopup();
    endCombo();

    return picked;
}

bool checkbox(const char* label, bool& value, const char* tip)
{
    if (hidden()) return false;

    // Centred on a setting's row in an item, and on a button's beside one.
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (frame.inItem ? ImGui::GetStyle().FramePadding.y : px(4)));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, value ? colour::Text : colour::FrameActive);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, value ? IM_COL32_WHITE : colour::TextFaint);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, value ? IM_COL32_WHITE : colour::TextFaint);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
    const bool changed = ImGui::Checkbox(label, &value);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    tooltip(tip);
    if (frame.inItem) gap(3 + 6);

    return changed;
}

bool button(const char* label, const char* tip, bool enabled)
{
    if (hidden()) return false;

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(px(10), px(4)));
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::Button(label);
    ImGui::EndDisabled();
    ImGui::PopStyleVar();
    tooltip(tip);

    return pressed;
}

bool textBox(const char* id, std::span<char> text, float lines, bool readOnly)
{
    if (hidden() || text.empty()) return false;

    const ImVec2 padding{px(8), px(6)};
    const ImVec2 size{std::min(px(SummaryWidth), ImGui::GetContentRegionAvail().x), std::round(ImGui::GetTextLineHeight() * lines + padding.y * 2)};
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, padding);
    // Ctrl+Enter submits a multi-line box; Enter starts a new line.
    const ImGuiInputTextFlags flags = readOnly ? ImGuiInputTextFlags_ReadOnly : ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AllowTabInput;
    const bool submitted = ImGui::InputTextMultiline(id, text.data(), text.size(), size, flags, nullptr, nullptr);
    ImGui::PopStyleVar();
    // Submitting ends the edit; the box takes the keyboard again for the next one.
    if (submitted) ImGui::SetKeyboardFocusHere(-1);
    gap(6);

    return submitted && !readOnly;
}

void hint(std::string_view text)
{
    if (hidden()) return;

    // Under the control: in its column, or under the whole stacked setting.
    if (!stacked(ImGui::GetContentRegionAvail().x)) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + px(ControlX));
    wrapped(text, colour::TextDim, ControlMax + ColumnGap + ReadoutWidth);
    gap(6);
}

void note(Tone tone, std::string_view text)
{
    if (hidden()) return;

    wrapped(text, toneColour(tone), SummaryWidth);
    gap(6);
}

void statusLine(Tone tone, std::string_view text)
{
    if (frame.searching) return;

    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float dot = px(5), top = at.y + std::round((ImGui::GetFontSize() - dot) / 2);
    drawList()->AddRectFilled({at.x, top}, {at.x + dot, top + dot}, tint(toneColour(tone)));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + dot + px(6));
    wrapped(text, toneColour(tone), SummaryWidth);
    gap(14);
}

void subheading(const char* text)
{
    if (hidden()) return;

    gap(20);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float right = at.x + ImGui::GetContentRegionAvail().x;
    textLine(text, colour::Text);
    const float ruleLeft = ImGui::GetItemRectMax().x + px(8), middle = at.y + std::round(ImGui::GetFontSize() / 2);
    if (ruleLeft < right) drawList()->AddRectFilled({ruleLeft, middle}, {right, middle + 1}, tint(colour::LineStrong));
    gap(8);
}

void gap(float pixels)
{
    ImGui::Dummy({0, px(pixels)});
}

void fact(const char* label, std::string_view value)
{
    if (hidden()) return;

    // The value in the controls' column, or under the label where settings stack.
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const bool under = stacked(ImGui::GetContentRegionAvail().x);
    textLine(label, colour::TextDim);
    if (under) gap(3);
    else ImGui::SetCursorScreenPos({at.x + px(ControlX), at.y});
    wrapped(value, colour::Text, ControlMax + ColumnGap + ReadoutWidth);
    gap(under ? 10.0f : 7.0f);
}

bool link(const char* label, std::string_view id)
{
    if (!ImGui::TextLink(label)) return false;

    goTo(id);
    return true;
}

void goTo(std::string_view id)
{
    nav.goTo = id;
}

void tooltip(const char* text)
{
    if (!text || !ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) return;

    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

// ---- The budget page ----

bool ledger()
{
    if (hidden()) return false;

    const Budget& budget = *frame.budget;
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float left = start.x, right = left + ImGui::GetContentRegionAvail().x;
    // A narrow budget drops the share of the frame and shows each state as its dot.
    const auto columns = columnsFor(right - left);
    const bool compact = right - left < px(FullLedger);
    const float msRight = right - (compact ? 0 : px(ShareWidth)), barLeft = msRight - columns.ms - columns.bar;
    const float stateLeft = barLeft - (compact ? px(14) : columns.state), nameRight = stateLeft - px(8);
    const float barMax = columns.bar - px(8);
    const float nameLeft = left + px(ChevronSize + Gutter);
    const float textHeight = ImGui::GetFontSize();
    const auto share = [&](float ms) { return budget.gpuMs > 0 && ms > 0 ? formatted("%.0f%%", ms / budget.gpuMs * 100) : std::string(); };
    const auto rule = [&](float y, ImU32 colour) { drawList()->AddRectFilled({left, y}, {right, y + 1}, tint(colour)); };

    // The head, with the bars' scale.
    addText({nameLeft, start.y}, colour::TextDim, "Effect");
    if (!compact) addText({stateLeft, start.y}, colour::TextDim, "State");
    const int lastTick = static_cast<int>(barMax / px(Millisecond));
    for (int ms = 0; ms <= lastTick; ++ms)
    {
        const float x = barLeft + static_cast<float>(ms) * px(Millisecond);
        const std::string text = ms == lastTick ? std::to_string(ms) + " ms" : std::to_string(ms);
        addText({ms == 0 ? x : x - std::round(textWidth(std::to_string(ms)) / 2), start.y}, colour::TextDim, text);
        drawList()->AddRectFilled({x, start.y + textHeight + px(2)}, {x + 1, start.y + textHeight + px(6)}, tint(colour::TextFaint));
    }
    rightText(msRight, start.y, "ms", colour::TextDim);
    if (!compact) rightText(right, start.y, "frame", colour::TextDim);
    ImGui::Dummy({1, px(19)});

    bool changed{};
    const float rowHeight = px(25), passHeight = px(19);
    for (const auto& line : budget.lines)
    {
        const ImU32 hue = frame.sections[static_cast<std::size_t>(line.section)].hue ? frame.sections[static_cast<std::size_t>(line.section)].hue : colour::TextDim;
        const std::string key(line.id);
        const bool open = nav.open.contains(key);
        const float top = ImGui::GetCursorScreenPos().y, textTop = top + std::round((rowHeight - textHeight) / 2);
        ImGui::PushID(line.id.data(), line.id.data() + line.id.size());
        rule(top, colour::Line);

        ImGui::SetCursorScreenPos({left, top + std::round((rowHeight - px(ChevronSize)) / 2)});
        if (ImGui::InvisibleButton("##passes", {px(ChevronSize), px(ChevronSize)}))
        {
            if (open) nav.open.erase(key);
            else nav.open.insert(key);
        }
        const ImU32 chevron = tint(ImGui::IsItemHovered() ? colour::Text : colour::TextDim);
        const ImVec2 centre{left + px(ChevronSize) / 2, top + rowHeight / 2};
        const float arm = px(3.5f);
        if (open) drawList()->AddTriangleFilled({centre.x - arm, centre.y - arm / 2}, {centre.x + arm, centre.y - arm / 2}, {centre.x, centre.y + arm}, chevron);
        else drawList()->AddTriangleFilled({centre.x - arm / 2, centre.y - arm}, {centre.x - arm / 2, centre.y + arm}, {centre.x + arm, centre.y}, chevron);
        tooltip(open ? "Hide its passes" : "Show its passes");

        if (line.toggle)
        {
            ImGui::SetCursorScreenPos({left + px(ChevronSize + 2), top + std::round((rowHeight - px(14)) / 2)});
            changed |= toggleSwitch("##on", *line.toggle, line.state != State::Held && line.state != State::Replaced);
        }

        // The name goes to its item.
        const float swatch = px(7);
        drawList()->AddRectFilled({nameLeft, textTop + std::round((textHeight - swatch) / 2)}, {nameLeft + swatch, textTop + std::round((textHeight - swatch) / 2) + swatch}, tint(hue));
        const float nameX = nameLeft + swatch + px(8);
        const float nameWidth = std::min(textWidth(line.name), nameRight - nameX);
        ImGui::SetCursorScreenPos({nameX, textTop});
        if (ImGui::InvisibleButton("##name", {std::max(1.0f, nameWidth), textHeight})) goTo(line.id);
        const bool nameHovered = ImGui::IsItemHovered();
        ImGui::PushClipRect({nameX, top}, {nameRight, top + rowHeight}, true);
        addText({nameX, textTop}, line.state == State::Working ? colour::Text : colour::TextDim, line.name);
        ImGui::PopClipRect();
        if (nameHovered) drawList()->AddRectFilled({nameX, textTop + textHeight + px(1)}, {nameX + nameWidth, textTop + textHeight + px(1) + std::max(1.0f, px(1))}, tint(colour::TextDim));
        tooltip(nameWidth < textWidth(line.name) ? (std::string(line.name) + ": go to its settings").c_str() : "Go to its settings");
        const float techX = nameX + textWidth(line.name) + px(8);
        if (!line.tech.empty() && techX + textWidth(line.tech) <= nameRight) addText({techX, textTop}, colour::TextDim, line.tech);

        drawState({stateLeft, textTop}, line.state, !compact);
        costBar({barLeft, top}, rowHeight, line.ms, hue, barMax);
        rightText(msRight, textTop, milliseconds(line.ms), line.ms >= 0.005f ? colour::Text : colour::TextFaint);
        if (!compact) rightText(right, textTop, share(line.ms), colour::TextDim);
        if (line.ms > 0 && ImGui::IsMouseHoveringRect({left, top}, {right, top + rowHeight})) nav.hotNext = line.id;
        ImGui::SetCursorScreenPos({left, top});
        ImGui::Dummy({1, rowHeight});

        if (open)
            for (const auto& pass : line.passes)
            {
                const float passTop = ImGui::GetCursorScreenPos().y, passText = passTop + std::round((passHeight - textHeight) / 2);
                ImGui::PushClipRect({nameX, passTop}, {barLeft - px(8), passTop + passHeight}, true);
                addText({nameX + px(13), passText}, colour::TextDim, pass.name);
                ImGui::PopClipRect();
                costBar({barLeft, passTop}, passHeight, pass.ms, withAlpha(hue, 0.55f), barMax, 3);
                rightText(msRight, passText, milliseconds(pass.ms), colour::TextDim);
                ImGui::Dummy({1, passHeight});
            }
        ImGui::PopID();
    }

    // The effects' total, the game's own work, and the whole GPU frame.
    const float total = budget.total(), game = std::max(0.0f, budget.gpuMs - total);
    const auto sumRow = [&](std::string_view name, float ms, ImU32 colour, bool strong, bool withShare) {
        const float top = ImGui::GetCursorScreenPos().y, textTop = top + std::round((rowHeight - textHeight) / 2);
        rule(top, strong ? colour::LineStrong : colour::Line);
        float nameX = nameLeft;
        if (colour)
        {
            const float swatch = px(7);
            drawList()->AddRectFilled({nameLeft, textTop + std::round((textHeight - swatch) / 2)}, {nameLeft + swatch, textTop + std::round((textHeight - swatch) / 2) + swatch}, tint(colour));
            nameX += swatch + px(8);
            costBar({barLeft, top}, rowHeight, ms, colour, barMax);
        }
        addText({nameX, textTop}, colour::Text, name);
        rightText(msRight, textTop, milliseconds(ms), colour::Text);
        if (withShare && !compact) rightText(right, textTop, share(ms), colour::TextDim);
        ImGui::Dummy({1, rowHeight});
    };
    sumRow(frame.name, total, 0, true, true);
    sumRow("The game's own work", game, colour::Game, false, true);
    sumRow("GPU frame", budget.gpuMs, 0, false, false);
    rule(ImGui::GetCursorScreenPos().y, colour::Line);
    ImGui::Dummy({1, 1});

    return changed;
}

void costRow(std::string_view name, float ms, ImU32 colour)
{
    if (hidden()) return;

    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float rowHeight = px(23), textTop = at.y + std::round((rowHeight - ImGui::GetFontSize()) / 2);
    const float full = px(RowName + BarWidth + MsWidth);
    const float width = frame.overlay ? full : std::min(full, ImGui::GetContentRegionAvail().x);
    const float bar = width < full ? px(56) : px(BarWidth);
    const float right = at.x + width, barLeft = right - px(MsWidth) - bar;
    drawList()->AddRectFilled(at, {right, at.y + 1}, tint(colour::Line));
    ImGui::PushClipRect(at, {barLeft - px(8), at.y + rowHeight}, true);
    addText({at.x, textTop}, colour::Text, name);
    ImGui::PopClipRect();
    if (colour) costBar({barLeft, at.y}, rowHeight, ms, colour, bar - px(8));
    rightText(right, textTop, milliseconds(ms), colour::Text);
    ImGui::Dummy({right - at.x, rowHeight});
}

// ---- The costs over the game ----

CostOverlay::CostOverlay(std::span<const Section> sections, const Budget& budget, const char* name)
{
    frame = {};
    frame.unit = ImGui::GetFontSize() / FontBase;
    frame.name = name;
    frame.overlay = true;

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos({display.x - px(24), px(24)}, ImGuiCond_Always, {1, 0});
    ImGui::SetNextWindowBgAlpha(0.75f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, colour::Ground);
    ImGui::PushStyleColor(ImGuiCol_Text, colour::Text);
    colours = 2;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(10), px(8)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(8), 0));
    vars = 4;

    constexpr auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings
        | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove;
    const std::string title = std::string(name) + " costs###fxui-costs-" + name;
    open = ImGui::Begin(title.c_str(), nullptr, flags);
    if (!open) return;

    const float rendered = budget.frameMs > 0 ? 1000 / budget.frameMs : 0;
    auto headline = formatted("GPU frame %.2f ms, ", budget.gpuMs)
        + (rendered > 0 ? formatted("%.0f fps", rendered * static_cast<float>(budget.presented)) : std::string("- fps"));
    if (budget.presented > 1 && rendered > 0) headline += formatted(" (%.0f rendered)", rendered);
    textLine(headline, colour::TextDim);
    gap(4);
    for (const auto& line : budget.lines)
        if (line.ms > 0)
        {
            const bool known = line.section >= 0 && line.section < static_cast<int>(sections.size());
            costRow(line.name, line.ms, known && sections[static_cast<std::size_t>(line.section)].hue ? sections[static_cast<std::size_t>(line.section)].hue : colour::TextDim);
        }
    costRow(std::string(name) + " total", budget.total(), 0);
}

CostOverlay::~CostOverlay()
{
    ImGui::End();
    ImGui::PopStyleVar(vars);
    ImGui::PopStyleColor(colours);
}

// ---- A message over the game ----

namespace
{
constexpr double ToastSeconds = 2.5, ToastFadeIn = 0.15, ToastFadeOut = 0.4;

struct Toast
{
    Tone tone{};
    std::string text, detail;
    std::chrono::steady_clock::time_point shown;
};

// toast() may come from another thread than the overlay's.
std::mutex toastMutex;
std::optional<Toast> toastShowing;

// Where a toast sits in the screen's free space: 0 at the left or top, 1 at the right or bottom.
ImVec2 toastAlignment(Placement placement)
{
    switch (placement)
    {
    case Placement::TopLeft: return {0, 0};
    case Placement::Top: return {0.5f, 0};
    case Placement::TopRight: return {1, 0};
    case Placement::Left: return {0, 0.5f};
    case Placement::BottomLeft: return {0, 1};
    case Placement::Bottom: return {0.5f, 1};
    case Placement::BottomRight: return {1, 1};
    case Placement::Right:
    default: return {1, 0.5f};
    }
}
}

void toast(Tone tone, std::string_view text, std::string_view detail)
{
    std::lock_guard lock(toastMutex);
    toastShowing = Toast{tone, std::string(text), std::string(detail), std::chrono::steady_clock::now()};
}

void drawToast(Placement placement)
{
    std::lock_guard lock(toastMutex);
    if (!toastShowing) return;

    const double age = std::chrono::duration<double>(std::chrono::steady_clock::now() - toastShowing->shown).count();
    if (age >= ToastSeconds)
    {
        toastShowing.reset();
        return;
    }

    frame.unit = ImGui::GetFontSize() / FontBase;

    // It eases into place from the edge it sits by, holds, then fades out.
    const float in = 1 - static_cast<float>(std::pow(1 - std::min(age / ToastFadeIn, 1.0), 3));
    const float alpha = std::min(in, static_cast<float>((ToastSeconds - age) / ToastFadeOut));
    const Toast& message = *toastShowing;

    const float fontSize = ImGui::GetFontSize(), dot = px(5), padX = px(12), padY = px(8), spacing = px(8);
    const float textLength = textWidth(message.text);
    float width = padX + dot + spacing + textLength + padX;
    if (!message.detail.empty()) width += spacing + textWidth(message.detail);

    const float height = padY + fontSize + padY, margin = px(24);
    const ImVec2 room = ImGui::GetIO().DisplaySize - ImVec2(margin * 2 + width, margin * 2 + height);
    const ImVec2 along = toastAlignment(placement);
    ImVec2 min{std::round(margin + room.x * along.x), std::round(margin + room.y * along.y)};

    // From the side at the left and right, else from the top or bottom.
    const float slide = std::round(px(6) * (1 - in));
    if (placement == Placement::Left || placement == Placement::Right)
        min.x += placement == Placement::Left ? -slide : slide;
    else
        min.y += along.y == 0 ? -slide : slide;

    const ImVec2 max = min + ImVec2(width, height);
    ImDrawList* draw = ImGui::GetForegroundDrawList(nullptr);
    draw->AddRectFilled(min, max, withAlpha(colour::Ground, 0.9f * alpha));

    const float textTop = min.y + padY, dotTop = textTop + std::round((fontSize - dot) / 2);
    float x = min.x + padX;
    draw->AddRectFilled({x, dotTop}, {x + dot, dotTop + dot}, withAlpha(toneColour(message.tone), alpha));
    x += dot + spacing;
    draw->AddText({x, textTop}, withAlpha(colour::Text, alpha), message.text.data(), message.text.data() + message.text.size());
    if (message.detail.empty()) return;

    x += textLength + spacing;
    draw->AddText({x, textTop}, withAlpha(colour::TextDim, alpha), message.detail.data(), message.detail.data() + message.detail.size());
}
}
