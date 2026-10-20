#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// What Phantom FX's passes cost on the GPU, for the overlay's Diagnostics (docs/DEVELOPMENT.md,
// "Costs"). While someone looks (the Diagnostics section open, the costs shown over the game, or a
// measurement running), each pass ends a timestamp query before and after its work, in the context
// it records in. The game records a frame on its deferred contexts while the immediate context
// executes the one before, so a deferred context's timestamps follow its command list and count for
// the frame whose present follows the list's execution. They are read back a few frames later,
// without waiting, and averaged over about half a second.
//
// A timed pass misses what it changes in the game's own work: GTVBAO leaves the game's SSAO out,
// the fog makes the game's fog texture larger. A measurement therefore times the GPU frame with
// everything on, then leaves out one effect after another; each difference is what that effect
// really costs.
namespace phantom_fx::performance
{
// The add-on's passes, grouped by effect in the cost table (performance_menu.cpp).
enum class Pass : std::size_t
{
    UpscalerInputs, Upscaler, UpscalerOutput,
    SunTrace, SunLight, LampTraces, LampShadows,
    OcclusionMarch, OcclusionDenoise, OcclusionApply,
    LightMarch, LightDenoise,
    FogPass, SkyTables, FogShadow, FogScattering, FogIntegration, Sky,
    DofSetup, DofGather, DofComposite, LensShot,
    HdrEncode, HdrDecode, HdrGrade,
    Count
};

using Milliseconds = std::array<float, static_cast<std::size_t>(Pass::Count)>;

// In milliseconds, averaged over about half a second.
struct Costs
{
    float frame{};    // Between presents: the whole frame, as the frame rate shows it
    float gpuFrame{}; // The GPU's work on a frame, from its first command list to its present
    Milliseconds passes{}; // 0 where a pass did not run

    float total() const;
};

// The real cost of each effect: the GPU frame with everything on, then with the effects left out
// one by one, cumulatively. Each step settles, then is timed. Effects that are off are skipped.
struct Measurement
{
    struct Step { std::string removed; float gpuFrame{}; }; // The first step removes nothing
    std::vector<Step> steps;
    std::uint64_t finishedAt{}; // GetTickCount64 when `steps` were measured
    bool running{};
    unsigned step{}, stepCount{};
};

struct Status
{
    bool overlay{}; // The costs show over the game
    Costs costs;
    Measurement measurement;
};

// ---- Setup, on the thread running the immediate context ----
// `report` is PhantomFX-performance.txt, which holds the costs while they show over the game or are measured.
void attach(ID3D11Device* device, ID3D11DeviceContext* immediate, const std::filesystem::path& report) noexcept;
// Releases everything; only when no Direct3D work can run (device teardown, unload).
void detach() noexcept;

// ---- Recording, on any thread ----
struct TimedRun
{
    Pass pass{};
    Microsoft::WRL::ComPtr<ID3D11Query> start, end;
};

// Times the GPU work recorded on `context` from its construction until end() or its destruction.
// While no one looks at the costs it does nothing.
class Timed
{
public:
    Timed(ID3D11DeviceContext* context, Pass pass) noexcept;
    ~Timed() { end(); }
    Timed(const Timed&) = delete;
    Timed& operator=(const Timed&) = delete;

    // Ends the pass being timed, if any, and starts timing `pass`.
    void next(Pass pass) noexcept;
    void end() noexcept;

private:
    ID3D11DeviceContext* context;
    std::optional<TimedRun> run;
};

// FinishCommandList: what `context` recorded now belongs to `list`.
void recorded(std::uint64_t list, std::uint64_t context) noexcept;
void forget(std::uint64_t list) noexcept;

// ---- Immediate context ----
// Before the immediate context executes `list`.
void executing(std::uint64_t list) noexcept;
// Each present, after the add-on's own present work.
void present() noexcept;

// ---- Overlay (performance_menu.cpp) ----
Status status();
// The costs are on screen: frames are timed for the next second.
void watch() noexcept;
void showOverlay(bool show) noexcept;
// Starts measuring each effect's real cost, unless a measurement is running.
void measure() noexcept;
// The Diagnostics section's costs, and the corner table over the game, drawn every frame.
void drawDiagnostics();
void drawOverlay();
// The costs as plain text, for PhantomFX-performance.txt.
std::string report(const Status& state);
}
