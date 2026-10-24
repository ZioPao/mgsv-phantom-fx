#pragma once

#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// fxui: a settings window for ReShade add-ons, drawn with the Dear ImGui that ReShade gives add-ons.
// The window is a frame budget: every effect is a line with its GPU milliseconds on one scale, the
// frame split by effect heads the window, a rail of sections runs down its left, and each section is
// a page of line items with their settings. README.md shows a whole window.
//
// Each frame, inside the overlay callback: fill a Budget, open a Window, then draw the pages:
//
//     fxui::Window window(sections, budget, chrome);
//     if (fxui::Page page(Section::Shadows, "Shadows of the sun."); page)
//         if (fxui::Item item({.id = "softShadows", .name = "Soft shadows"}); item)
//             fxui::slider("Sun size", settings.sunSize, {.min = 0.1f, .max = 4, .standard = 0.53f});
//
// Every call that draws must include <reshade.hpp> after <imgui.h>, which routes ImGui through ReShade.
// One window at a time: it keeps its page, search and scroll target between frames.
namespace fxui
{
// The window's own palette.
namespace colour
{
inline constexpr ImU32 Ground = IM_COL32(0x14, 0x17, 0x1b, 0xff);
inline constexpr ImU32 Rail = IM_COL32(0x0f, 0x11, 0x14, 0xff);
inline constexpr ImU32 Raise = IM_COL32(0x1a, 0x1e, 0x23, 0xff);
inline constexpr ImU32 Popup = IM_COL32(0x1b, 0x1f, 0x25, 0xff);
inline constexpr ImU32 Frame = IM_COL32(0x22, 0x27, 0x2e, 0xff);
inline constexpr ImU32 FrameHover = IM_COL32(0x2b, 0x31, 0x3a, 0xff);
inline constexpr ImU32 FrameActive = IM_COL32(0x35, 0x3c, 0x46, 0xff);
inline constexpr ImU32 Line = IM_COL32(0x26, 0x2b, 0x32, 0xff);
inline constexpr ImU32 LineStrong = IM_COL32(0x39, 0x40, 0x4a, 0xff);
inline constexpr ImU32 Text = IM_COL32(0xe6, 0xe4, 0xde, 0xff);
inline constexpr ImU32 TextDim = IM_COL32(0x9c, 0xa1, 0xa7, 0xff);
inline constexpr ImU32 TextFaint = IM_COL32(0x68, 0x6e, 0x75, 0xff);
inline constexpr ImU32 Ok = IM_COL32(0x8f, 0xd1, 0x8e, 0xff);
inline constexpr ImU32 Warn = IM_COL32(0xef, 0xb3, 0x5c, 0xff);
inline constexpr ImU32 Bad = IM_COL32(0xef, 0x7d, 0x6d, 0xff);
// The host game's own share of the frame.
inline constexpr ImU32 Game = IM_COL32(0x3a, 0x40, 0x48, 0xff);
}

// Section hues: equally light on the ground, and apart from the state colours.
namespace hue
{
inline constexpr ImU32 Teal = IM_COL32(0x5e, 0xc4, 0xb6, 0xff);
inline constexpr ImU32 Straw = IM_COL32(0xd8, 0xcd, 0x78, 0xff);
inline constexpr ImU32 Blue = IM_COL32(0x7a, 0xa8, 0xee, 0xff);
inline constexpr ImU32 Violet = IM_COL32(0xa9, 0x9c, 0xe8, 0xff);
inline constexpr ImU32 Pink = IM_COL32(0xe0, 0x91, 0xb8, 0xff);
inline constexpr ImU32 Sand = IM_COL32(0xb8, 0xae, 0x9e, 0xff);
}

// ---- What the add-on reports each frame ----

enum class State : std::uint8_t
{
    Working,
    Off,
    Held,        // On, but held off by the window's master switch
    Replaced,    // On, but the line in `replacedBy` replaces it while that one is on
    Restart,     // Changed, and the change takes a restart of the game
    NotRunning,  // On, but not running; the message says why
    Unavailable, // Failed; the message says why
};

struct Pass
{
    std::string_view name;
    float ms{};
};

// An effect whose GPU work is timed: a row in the budget, a slice of the frame bar, and the state and
// cost of the line item with the same id.
struct Line
{
    std::string_view id;
    std::string_view name;
    std::string_view tech; // Its technical name, dim beside the name
    int section{};
    State state{State::Off};
    float ms{};               // Measured; 0 where it did not run
    std::vector<Pass> passes; // The budget's breakdown
    bool* toggle{};           // Its on/off setting, switched from the budget and from its item
    std::string_view replacedBy;
    std::string message;      // Why it is not running, or what a restart changes
};

struct Budget
{
    std::vector<Line> lines;
    float gpuMs{};   // The GPU's whole frame
    float frameMs{}; // Between the frames the game renders and presents
    unsigned presented{1}; // Frames shown per rendered frame: more with frame generation

    float total() const;
    float total(int section) const;
    const Line* find(std::string_view id) const;
};

// ---- The window's shape ----

struct Section
{
    const char* name{};
    ImU32 hue{};     // Its lines' colour; 0 for a section without timed lines
    bool overview{}; // The budget page: no items, and the rail shows every section's share
    bool apart{};    // A rule above it in the rail, such as for diagnostics
};

struct Key
{
    const char* key{};
    const char* action{};
};

struct Chrome
{
    const char* name{"Effects"}; // The add-on's name, for its share of the frame
    // The master switch in the top strip, such as all visual effects on F1; none without `master`.
    bool* master{};
    const char* masterLabel{};
    const char* masterKey{};
    const char* masterTip{};
    const char* masterOff{}; // Beside the switch while it is off
    std::span<const Key> keys; // Hotkeys at the foot of the rail
};

// The strip, the frame bar and the rail, then the page area until it goes out of scope. `sections`
// and `budget` must outlive it.
class Window
{
public:
    Window(std::span<const Section> sections, const Budget& budget, const Chrome& chrome = {});
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
};

// A section's page: true while it shows, when its section is chosen or a search runs. During a search
// only items show, so a page's settings belong in its items.
class Page
{
public:
    explicit Page(int section, const char* description = nullptr);
    template<class E> requires std::is_enum_v<E>
    explicit Page(E section, const char* description = nullptr) : Page(static_cast<int>(section), description) {}
    ~Page();
    Page(const Page&) = delete;
    Page& operator=(const Page&) = delete;

    explicit operator bool() const { return shown; }

private:
    bool shown{};
};

struct ItemSpec
{
    const char* id{};       // Unique; a budget line with the same id gives the item its state and cost
    const char* name{};
    const char* tech{};     // Its technical name, dim beside the name
    const char* tag{};      // An outlined tag, such as "Experimental"
    const char* tip{};      // The long explanation, on the name's tooltip
    const char* summary{};  // One plain line under the name
    const char* keywords{}; // More words a search finds it by, such as its settings' labels
    // Without a budget line of the same id:
    bool* toggle{};
    std::optional<State> state;
    std::string_view message;
};

// A line item: a rule, its switch, name, state and cost, and its summary and messages. True while
// its settings show: it is on (or has no switch) and not replaced.
class Item
{
public:
    explicit Item(const ItemSpec& spec);
    ~Item();
    Item(const Item&) = delete;
    Item& operator=(const Item&) = delete;

    explicit operator bool() const { return open; }

private:
    const char* id{};
    bool drawn{}, open{}, held{}, hot{};
    float top{}, left{}, right{};
};

// An item without settings of its own.
void item(const ItemSpec& spec);

// ---- Settings, in an item: a label, the control, its readout and a reset button ----

struct SliderSpec
{
    float min{}, max{1};
    std::optional<float> standard; // The default the reset button returns to
    const char* format{"%.2f"};    // printf format of the readout
    float shown{1};                // Multiplies the value in the readout, such as 100 for a percentage
    std::string (*text)(float value){}; // A readout that a format cannot write, such as a clock
    bool logarithmic{};
    bool onRelease{}; // Changes the value only when released, for values that are slow to apply
    const char* tip{};
};

// True when the value changed.
bool slider(const char* label, float& value, const SliderSpec& spec);

using Icon = void (*)(ImDrawList* draw, ImVec2 centre, float radius, ImU32 colour);

struct Option
{
    const char* label{};
    Icon icon{};
    bool enabled{true};
    const char* tip{};
};

template<class T>
struct Choice
{
    T value{};
    const char* label{};
    Icon icon{};
    bool enabled{true};
    const char* tip{};
};

// Side-by-side buttons for a few choices. Returns the index pressed, or -1.
int segmented(const char* label, int selected, std::span<const Option> options, const char* tip = nullptr);

// A combo box: true while open, then option() for each entry and endCombo().
bool beginCombo(const char* label, const char* preview, const char* tip = nullptr);
bool option(const char* label, bool selected, bool enabled = true, const char* tip = nullptr);
void endCombo();

bool checkbox(const char* label, bool& value, const char* tip = nullptr);
bool button(const char* label, const char* tip = nullptr, bool enabled = true);
// Text under a control, in the control's column.
void hint(std::string_view text);

enum class Tone : std::uint8_t { Dim, Ok, Warn, Bad };

// A line of text at the current indent: grey, or in a state colour.
void note(Tone tone, std::string_view text);
// A page's one line on what it does now, with a dot in its tone. Hidden during a search.
void statusLine(Tone tone, std::string_view text);
// A label with a rule to its right, above a group of settings.
void subheading(const char* text);
// Vertical space, in pixels at the 13 px font.
void gap(float pixels);
// A label and its value, in the settings' columns.
void fact(const char* label, std::string_view value);
// Underlined text that jumps to the item `id`, and its page.
bool link(const char* label, std::string_view id);
// The item `id`: chooses its page, scrolls to it and flashes it.
void goTo(std::string_view id);
// The long explanation of the last control, while the mouse rests on it.
void tooltip(const char* text);

// ---- The budget page ----

// Every line with its switch, state, cost and share of the frame; each opens to its passes, and its
// name jumps to its item. Then the total, the game's own work and the GPU frame. True when a switch
// changed.
bool ledger();
// A row of another table of costs, such as measured ones: a name, a bar and milliseconds.
void costRow(std::string_view name, float ms, ImU32 colour = colour::TextDim);

// The costs in a corner over the game, also with ReShade's overlay closed: for the add-on's
// reshade_overlay event. True while it shows, to add rows before it goes out of scope.
class CostOverlay
{
public:
    CostOverlay(std::span<const Section> sections, const Budget& budget, const char* name);
    ~CostOverlay();
    CostOverlay(const CostOverlay&) = delete;
    CostOverlay& operator=(const CostOverlay&) = delete;

    explicit operator bool() const { return open; }

private:
    bool open{};
    int colours{}, vars{};
};

// A short message over the game for a few seconds, such as "Frame captured", with a dot in its tone
// and a dim detail after it. A new message replaces the one showing. Any thread may call it.
void toast(Tone tone, std::string_view text, std::string_view detail = {});

// A side or corner of the screen.
enum class Placement : std::uint8_t { TopLeft, Top, TopRight, Left, Right, BottomLeft, Bottom, BottomRight };

// Draws the message at `placement`, over the game and the window, also with ReShade's overlay closed:
// for the add-on's reshade_overlay event.
void drawToast(Placement placement = Placement::Right);

// ---- Typed conveniences ----

template<class T>
bool segmented(const char* label, T& value, std::span<const Choice<T>> choices, const char* tip = nullptr)
{
    std::vector<Option> options;
    int selected = -1;
    for (const auto& choice : choices)
    {
        if (choice.value == value) selected = static_cast<int>(options.size());
        options.push_back({choice.label, choice.icon, choice.enabled, choice.tip});
    }

    const int pressed = segmented(label, selected, options, tip);
    if (pressed < 0 || pressed == selected) return false;

    value = choices[static_cast<std::size_t>(pressed)].value;
    return true;
}

template<class T>
bool combo(const char* label, T& value, std::span<const Choice<T>> choices, const char* tip = nullptr)
{
    const char* preview = "";
    for (const auto& choice : choices)
        if (choice.value == value) preview = choice.label;

    if (!beginCombo(label, preview, tip)) return false;

    bool changed{};
    for (const auto& choice : choices)
        if (option(choice.label, choice.value == value, choice.enabled, choice.tip) && choice.value != value)
        {
            value = choice.value;
            changed = true;
        }

    endCombo();
    return changed;
}

template<class T, std::size_t N>
bool segmented(const char* label, T& value, const Choice<T> (&choices)[N], const char* tip = nullptr)
{
    return segmented(label, value, std::span<const Choice<T>>(choices), tip);
}

template<class T, std::size_t N>
bool combo(const char* label, T& value, const Choice<T> (&choices)[N], const char* tip = nullptr)
{
    return combo(label, value, std::span<const Choice<T>>(choices), tip);
}

// A combo whose list starts with a search box and shows the names that hold every word of it, in any
// case: for long lists. Returns the index of the name picked.
std::optional<std::size_t> searchCombo(const char* label, const char* preview, std::span<const std::string_view> names, std::optional<std::size_t> selected,
    const char* tip = nullptr);
}
