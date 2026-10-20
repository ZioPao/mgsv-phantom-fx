#pragma once

#include <array>
#include <cstdint>
#include <string>

// Photo mode (F2): the game freezes through its own pause requests while a free camera flies through
// the frozen scene, with the UI hidden and the depth of field focused on the centre of the screen
// (focus.hpp). Hiding the UI (F3) also works on its own, at any time. docs/DEVELOPMENT.md, "Photo mode".
//
// A lens shot (F5) renders the frozen scene from points across the lens's aperture, one frame each,
// and the depth of field (depth_of_field.hpp) shows their average: a real lens's blur, which sees
// past the edges of what is out of focus.
namespace phantom_fx::photo_mode
{
// The keys held this frame, as the free camera reads them.
struct Controls
{
    bool forward{}, back{}, left{}, right{}, up{}, down{}, fast{};
};

// From the settings.
struct LensSettings
{
    float fNumber{};
    int blades{}; // 0 is round
    std::uint32_t shotFrames{};
};

// Photo mode's lens as the frames being recorded use it.
struct Lens
{
    bool active{};         // In photo mode: the depth of field follows this lens, not the game's camera
    float aperture{};      // Diameter in metres: focal length / f-number; 0 with the blur off (F4)
    float focusDistance{}; // Metres along the view
    std::uint32_t shot{};  // Counts up with each lens shot
    bool shooting{};       // A lens shot is adding up frames, or showing its result
    std::uint32_t shotFrames{};
    std::array<float, 3> centre{}; // The camera's position in a shot; the lens points lie around it
};

// Hooks the camera selector and the game's raw input on the supported build, with MinHook
// initialised; returns what was done, for the log. Without the hooks photo mode is unavailable;
// hiding the UI does not need them.
std::string hook() noexcept;
// Applies at the game's next camera update. The free camera starts where the game's camera is.
void toggle() noexcept;
void toggleUi() noexcept;
// Photo mode's depth of field blur (F4): on by default; off, everything is sharp.
void toggleBlur() noexcept;
void configure(const LensSettings& settings) noexcept;
Lens lens() noexcept;
// F5 in photo mode starts a lens shot, or ends it; so does moving the camera.
void toggleShot() noexcept;
// The depth of field has added up all of the shot's frames; the camera goes back to its centre.
void shotFinished(std::uint32_t shot) noexcept;
void setControls(const Controls& controls) noexcept;
// In photo mode, or hidden with F3: the 2D pass does not draw into the scene.
bool uiHidden() noexcept;
// Unfreezes the game, for when the add-on unloads in photo mode.
void leave() noexcept;
}
