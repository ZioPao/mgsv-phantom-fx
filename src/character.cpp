#include "character.hpp"

#include "device_check.hpp"
#include "game_code.hpp"
#include "log.hpp"

#include <Windows.h>
#include <algorithm>
#include <cstring>
#include <format>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace phantom_fx::character
{
namespace
{
// The script variables ("vars") live in one block whose address is at VarsRva. The game registers
// each with its offset into the block (0x996B70), and the Lua setter only stores the value, so a
// write here is what `vars.playerType = x` in a script does.
constexpr std::uintptr_t VarsRva = 0x2C16920;
constexpr std::size_t MissionCode = 0x12, PartsType = 0xF8, CamoType = 0xF9, Type = 0xFB, FaceId = 0xFC, FaceEquipId = 0xFE;

// The registrations' "mov r8, [VarsRva]; add r8, offset", checked before the menu touches anything.
constexpr unsigned char MissionCodeCode[]{0x4C, 0x8B, 0x05, 0x61, 0xF9, 0x27, 0x02, 0x49, 0x83, 0xC0, 0x12};
constexpr unsigned char PartsTypeCode[]{0x4C, 0x8B, 0x05, 0x7C, 0xE9, 0x27, 0x02, 0x49, 0x81, 0xC0, 0xF8, 0x00, 0x00, 0x00};
constexpr unsigned char CamoTypeCode[]{0x4C, 0x8B, 0x05, 0x11, 0xE9, 0x27, 0x02, 0x49, 0x81, 0xC0, 0xF9, 0x00, 0x00, 0x00};
constexpr unsigned char TypeCode[]{0x4C, 0x8B, 0x05, 0x3B, 0xE8, 0x27, 0x02, 0x49, 0x81, 0xC0, 0xFB, 0x00, 0x00, 0x00};
constexpr unsigned char FaceIdCode[]{0x4C, 0x8B, 0x05, 0xD0, 0xE7, 0x27, 0x02, 0x49, 0x81, 0xC0, 0xFC, 0x00, 0x00, 0x00};
constexpr unsigned char FaceEquipIdCode[]{0x4C, 0x8B, 0x05, 0x65, 0xE7, 0x27, 0x02, 0x49, 0x81, 0xC0, 0xFE, 0x00, 0x00, 0x00};

const game_code::Expected Checked[]{{0x996FB8, MissionCodeCode}, {0x997F9D, PartsTypeCode}, {0x998008, CamoTypeCode}, {0x9980DE, TypeCode},
    {0x998149, FaceIdCode}, {0x9981B4, FaceEquipIdCode}};

// The player update (0x9CD030) reads vars.playerType and keeps Ocelot or Quiet only while one of two
// player system flags is set; otherwise it writes Venom Snake back and clears their outfit and camo.
// One flag follows game status 0x1B, which Infinite Heaven sets (as S_IS_SORTIE_PREPARATION) to pass
// the test, at the cost of the pause menu. The patch makes the first flag's jump to the keeping branch
// unconditional instead.
constexpr std::uintptr_t FobCheckRva = 0x9CD358, FobJumpOffset = 0x18;
// lea ecx, [rax - 5]; cmp cl, 1; ja; mov ecx, [rsi + 0x224]; shr ecx, 18; test cl, 1; jne 0x9CDB12
constexpr unsigned char FobCheckCode[]{0x8D, 0x48, 0xFB, 0x80, 0xF9, 0x01, 0x0F, 0x87, 0xCD, 0x07, 0x00, 0x00, 0x8B, 0x8E, 0x24, 0x02,
    0x00, 0x00, 0xC1, 0xE9, 0x12, 0xF6, 0xC1, 0x01, 0x0F, 0x85, 0x9C, 0x07, 0x00, 0x00};
// jmp 0x9CDB12; nop
constexpr unsigned char KeepFobCharacter[]{0xE9, 0x9D, 0x07, 0x00, 0x00, 0x90};

// vars.playerPartsType. The game's PlayerPartsType names only the first few; these follow Infinite
// Heaven's InfFova.lua, which also says which character each outfit has models for.
enum Parts : std::uint8_t
{
    Normal, NormalScarf, SneakingSuitGz, Hospital, Mgs1, Ninja, Raiden, Naked, SneakingSuit, Battledress, Parasite, Leather, Gold,
    Silver, AvatarEditMan, Mgs3, Mgs3Naked, Mgs3Sneaking, Mgs3Tuxedo, EvaClose, EvaOpen, BossClose, BossOpen, Swimwear, SwimwearGoblin,
    SwimwearMegalodon, OcelotParts, QuietParts
};

// The camo (vars.playerCamoType) each outfit wears, by vars.playerPartsType: its own, or Pattern for
// the fatigues, which take any camo pattern. A swimsuit takes the first of its twelve patterns.
constexpr std::uint8_t Pattern = 0xFF, OliveDrab = 0;
constexpr std::uint8_t OwnCamo[]{Pattern, Pattern, 14, 23, 20, 21, 22, Pattern, 15, 16, 17, 19, 24, 25, 27, 28, 29, 30, 31, 32, 33, 34,
    35, 79, 91, 103, 115, 116};
static_assert(std::size(OwnCamo) == QuietParts + 1);

// The patterns: Olive Drab to Sandstorm, Black, Animals and the later ones (36 to 78).
bool isPattern(std::uint8_t camo)
{
    return camo <= 10 || camo == 13 || camo == 26 || (camo >= 36 && camo <= 78);
}

// Every outfit each character has a model for, from InfFova.lua; the others break the model or crash.
constexpr Character Characters[]{
    {PlayerType::Snake, Normal, "Venom Snake: fatigues"},
    {PlayerType::Snake, NormalScarf, "Venom Snake: fatigues with scarf"},
    {PlayerType::Snake, Naked, "Venom Snake: naked fatigues"},
    {PlayerType::Snake, SneakingSuit, "Venom Snake: sneaking suit"},
    {PlayerType::Snake, SneakingSuitGz, "Venom Snake: Ground Zeroes sneaking suit"},
    {PlayerType::Snake, Battledress, "Venom Snake: battle dress"},
    {PlayerType::Snake, Parasite, "Venom Snake: parasite suit"},
    {PlayerType::Snake, Leather, "Venom Snake: leather jacket"},
    {PlayerType::Snake, Gold, "Venom Snake: gold"},
    {PlayerType::Snake, Silver, "Venom Snake: silver"},
    {PlayerType::Snake, Hospital, "Venom Snake: hospital gown"},
    {PlayerType::Snake, Mgs1, "Venom Snake: Solid Snake suit (MGS1)"},
    {PlayerType::Snake, Ninja, "Venom Snake: Cyborg Ninja suit"},
    {PlayerType::Snake, Raiden, "Venom Snake: Raiden suit"},
    {PlayerType::Snake, Mgs3, "Venom Snake: Naked Snake fatigues (MGS3)"},
    {PlayerType::Snake, Mgs3Naked, "Venom Snake: Naked Snake naked fatigues (MGS3)"},
    {PlayerType::Snake, Mgs3Sneaking, "Venom Snake: Naked Snake sneaking suit (MGS3)"},
    {PlayerType::Snake, Mgs3Tuxedo, "Venom Snake: tuxedo"},
    {PlayerType::Avatar, Normal, "Avatar: fatigues"},
    {PlayerType::Avatar, NormalScarf, "Avatar: fatigues with scarf"},
    {PlayerType::Avatar, Naked, "Avatar: naked fatigues"},
    {PlayerType::Avatar, SneakingSuit, "Avatar: sneaking suit"},
    {PlayerType::Avatar, SneakingSuitGz, "Avatar: Ground Zeroes sneaking suit"},
    {PlayerType::Avatar, Battledress, "Avatar: battle dress"},
    {PlayerType::Avatar, Parasite, "Avatar: parasite suit"},
    {PlayerType::Avatar, Leather, "Avatar: leather jacket"},
    {PlayerType::Avatar, Gold, "Avatar: gold"},
    {PlayerType::Avatar, Silver, "Avatar: silver"},
    {PlayerType::Avatar, Mgs1, "Avatar: Solid Snake suit (MGS1)"},
    {PlayerType::Avatar, Ninja, "Avatar: Cyborg Ninja suit"},
    {PlayerType::Avatar, Raiden, "Avatar: Raiden suit"},
    {PlayerType::Avatar, Mgs3, "Avatar: Naked Snake fatigues (MGS3)"},
    {PlayerType::Avatar, Mgs3Naked, "Avatar: Naked Snake naked fatigues (MGS3)"},
    {PlayerType::Avatar, Mgs3Sneaking, "Avatar: Naked Snake sneaking suit (MGS3)"},
    {PlayerType::Avatar, Mgs3Tuxedo, "Avatar: tuxedo"},
    {PlayerType::MaleSoldier, Normal, "Male soldier: fatigues"},
    {PlayerType::MaleSoldier, SneakingSuit, "Male soldier: sneaking suit"},
    {PlayerType::MaleSoldier, Battledress, "Male soldier: battle dress"},
    {PlayerType::MaleSoldier, Parasite, "Male soldier: parasite suit"},
    {PlayerType::MaleSoldier, Mgs1, "Male soldier: Solid Snake suit (MGS1)"},
    {PlayerType::MaleSoldier, Ninja, "Male soldier: Cyborg Ninja suit"},
    {PlayerType::MaleSoldier, Raiden, "Male soldier: Raiden suit"},
    {PlayerType::MaleSoldier, Mgs3, "Male soldier: Naked Snake fatigues (MGS3)"},
    {PlayerType::MaleSoldier, Mgs3Naked, "Male soldier: Naked Snake naked fatigues (MGS3)"},
    {PlayerType::MaleSoldier, Mgs3Sneaking, "Male soldier: Naked Snake sneaking suit (MGS3)"},
    {PlayerType::MaleSoldier, Mgs3Tuxedo, "Male soldier: tuxedo"},
    {PlayerType::MaleSoldier, Swimwear, "Male soldier: swimsuit"},
    {PlayerType::MaleSoldier, SwimwearGoblin, "Male soldier: Goblin swimsuit"},
    {PlayerType::MaleSoldier, SwimwearMegalodon, "Male soldier: Megalodon swimsuit"},
    {PlayerType::FemaleSoldier, Normal, "Female soldier: fatigues"},
    {PlayerType::FemaleSoldier, SneakingSuit, "Female soldier: sneaking suit"},
    {PlayerType::FemaleSoldier, Battledress, "Female soldier: battle dress"},
    {PlayerType::FemaleSoldier, Parasite, "Female soldier: parasite suit"},
    {PlayerType::FemaleSoldier, Mgs1, "Female soldier: Solid Snake suit (MGS1)"},
    {PlayerType::FemaleSoldier, Ninja, "Female soldier: Cyborg Ninja suit"},
    {PlayerType::FemaleSoldier, Raiden, "Female soldier: Raiden suit"},
    {PlayerType::FemaleSoldier, EvaClose, "Female soldier: EVA jumpsuit (MGS3)"},
    {PlayerType::FemaleSoldier, EvaOpen, "Female soldier: EVA jumpsuit, open (MGS3)"},
    {PlayerType::FemaleSoldier, BossClose, "Female soldier: The Boss sneaking suit (MGS3)"},
    {PlayerType::FemaleSoldier, BossOpen, "Female soldier: The Boss sneaking suit, open (MGS3)"},
    {PlayerType::FemaleSoldier, Swimwear, "Female soldier: swimsuit"},
    {PlayerType::FemaleSoldier, SwimwearGoblin, "Female soldier: Goblin swimsuit"},
    {PlayerType::FemaleSoldier, SwimwearMegalodon, "Female soldier: Megalodon swimsuit"},
    {PlayerType::Ocelot, OcelotParts, "Ocelot"},
    {PlayerType::Quiet, QuietParts, "Quiet"},
};

struct FaceRun
{
    std::uint16_t first, last;
    std::string_view description;
};

// The soldiers' faces in the game's table (Soldier2FaceAndBodyData.faceDefinition), with the skin the
// body takes to match (its skinFova), the headgear some wear, and the FOB events' tattoos. Left out are
// the ones Infinite Heaven found broken outside their mission and the duplicates.
constexpr FaceRun MaleFaces[]{{0, 9, "light skin"}, {10, 19, "olive skin"}, {20, 39, "fair skin"}, {40, 49, "olive skin"},
    {50, 81, "light skin"}, {82, 92, "olive skin"}, {93, 141, "light skin"}, {142, 158, "olive skin"}, {159, 199, "light skin"},
    {200, 216, "brown skin"}, {217, 234, "dark skin"}, {235, 252, "brown skin"}, {253, 282, "dark skin"}, {283, 303, "brown skin"},
    {320, 349, "olive skin"}, {550, 551, "balaclava"}, {552, 552, "green-top helmet"}, {558, 558, "gas mask and balaclava"},
    {560, 560, "gas mask and helmet"}, {561, 561, "gas mask and green-top helmet"}, {564, 564, "night vision goggles"},
    {565, 565, "night vision goggles and gas mask"}, {600, 612, "from a mission"}, {614, 621, "from a mission"},
    {623, 629, "from a mission"}, {635, 635, "from a mission"}, {637, 642, "from a mission"}, {644, 649, "from a mission"},
    {680, 680, "FOXHOUND tattoo"}, {683, 683, "Diamond Dogs tattoo, red hair"}, {684, 684, "fox tattoo"}, {687, 687, "skull tattoo, bald"}};
constexpr FaceRun FemaleFaces[]{{350, 386, "light skin"}, {387, 399, "fair skin"}, {450, 459, "brown skin"}, {460, 479, "dark skin"},
    {500, 506, "olive skin"}, {507, 513, "light skin"}, {514, 519, "olive skin"}, {555, 555, "green-top helmet"},
    {559, 559, "gas mask and balaclava"}, {562, 562, "gas mask and helmet"}, {563, 563, "gas mask and green-top helmet"},
    {566, 566, "night vision goggles"}, {567, 567, "night vision goggles and gas mask"}, {613, 613, "from a mission"},
    {643, 643, "from a mission"}, {681, 681, "FOXHOUND tattoo"}, {682, 682, "Diamond Dogs tattoo, red hair"}, {685, 685, "fox tattoo"},
    {686, 686, "skull tattoo, white hair"}};

std::vector<Face> listFaces(std::span<const FaceRun> runs)
{
    std::vector<Face> faces;
    for (const auto& run : runs)
        for (auto id = run.first; id <= run.last; ++id) faces.push_back({id, std::format("Face {}, {}", id, run.description)});

    return faces;
}

bool contains(std::span<const Face> faces, std::uint16_t id)
{
    return std::ranges::find(faces, id, &Face::id) != faces.end();
}

// What the menu asked for since the last frame start.
struct Pending
{
    std::optional<Character> character;
    std::optional<std::uint16_t> face;
};

std::mutex mutex;
Pending pending;
State published;

template<class Value> Value& at(std::uint8_t* object, std::size_t offset)
{
    return *reinterpret_cast<Value*>(object + offset);
}

// Story missions are 1xxxx and free roam 3xxxx (TppMission.lua). The helicopter (4xxxx) holds a
// second player that, by Infinite Heaven's notes, fails to load a different model; FOB (5xxxx) is online.
bool inMissionOrFreeRoam(std::uint16_t missionCode)
{
    const auto kind = missionCode / 10000;

    return kind == 1 || kind == 3;
}

void apply(std::uint8_t* vars, const Pending& wanted)
{
    auto& type = at<PlayerType>(vars, Type);
    auto& face = at<std::uint16_t>(vars, FaceId);
    if (wanted.character)
    {
        const auto& character = *wanted.character;
        auto& camo = at<std::uint8_t>(vars, CamoType);
        // Headgear (a bandana, a balaclava) is each character's own.
        if (character.type != type) at<std::uint16_t>(vars, FaceEquipId) = 0;

        type = character.type;
        at<std::uint8_t>(vars, PartsType) = character.parts;
        const auto own = OwnCamo[character.parts];
        camo = own != Pattern ? own : isPattern(camo) ? camo : OliveDrab;
        // A soldier keeps their face when it suits the new body.
        const auto soldierFaces = faces(type);
        if (!soldierFaces.empty() && !contains(soldierFaces, face)) face = soldierFaces.front().id;
    }

    if (wanted.face && contains(faces(type), *wanted.face)) face = *wanted.face;
    if (!wanted.character && !wanted.face) return;

    log(std::format("Character: player type {}, outfit {}, camo {}, face {}.", static_cast<int>(type), at<std::uint8_t>(vars, PartsType),
        at<std::uint8_t>(vars, CamoType), face));
}

State read(std::uint8_t* vars)
{
    return {.available = true, .type = at<PlayerType>(vars, Type), .parts = at<std::uint8_t>(vars, PartsType), .face = at<std::uint16_t>(vars, FaceId)};
}
}

std::string allowFobCharacters() noexcept
{
    if (!isSupportedGameBuild()) return "Unknown game build; Ocelot and Quiet stay FOB characters.";

    auto* const check = game_code::image() + FobCheckRva;
    if (std::memcmp(check, FobCheckCode, sizeof(FobCheckCode))) return "The player update has unexpected code; Ocelot and Quiet stay FOB characters.";

    if (!game_code::patch(check + FobJumpOffset, KeepFobCharacter)) return "Could not unprotect the player update; Ocelot and Quiet stay FOB characters.";

    return "Ocelot and Quiet can be played outside FOB.";
}

bool supported() noexcept
{
    static const bool expected = isSupportedGameBuild() && game_code::matches(Checked);

    return expected;
}

State state() noexcept
{
    std::scoped_lock lock(mutex);

    return published;
}

std::span<const Character> characters() noexcept
{
    return Characters;
}

std::span<const Face> faces(PlayerType type) noexcept
{
    static const auto male = listFaces(MaleFaces);
    static const auto female = listFaces(FemaleFaces);
    if (type == PlayerType::MaleSoldier) return male;
    if (type == PlayerType::FemaleSoldier) return female;

    return {};
}

void choose(const Character& character) noexcept
{
    std::scoped_lock lock(mutex);
    pending.character = character;
}

void chooseFace(std::uint16_t face) noexcept
{
    std::scoped_lock lock(mutex);
    pending.face = face;
}

void onFrameStart() noexcept
{
    if (!supported()) return;

    Pending wanted;
    {
        std::scoped_lock lock(mutex);
        wanted = std::exchange(pending, {});
    }

    auto* const vars = at<std::uint8_t*>(game_code::image(), VarsRva);
    State now;
    if (vars && inMissionOrFreeRoam(at<std::uint16_t>(vars, MissionCode)))
    {
        apply(vars, wanted);
        now = read(vars);
    }

    std::scoped_lock lock(mutex);
    published = now;
}
}
