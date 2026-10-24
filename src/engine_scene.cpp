#include "engine_scene.hpp"

#include "device_check.hpp"
#include "engine_brush.hpp"
#include "game_memory.hpp"
#include "game_loop.hpp"

#include <Windows.h>
#include <MinHook.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>

namespace phantom_fx::engine_scene
{
namespace
{
using game_memory::read;
using game_memory::readRaw;

// The model visibility tests, called from the job threads with the visibility manager in rcx, once a
// view (main camera, shadow cascades) a frame; docs/DEVELOPMENT.md, "Scene objects".
constexpr std::uintptr_t VisibilityTestRvas[]{0x22C6F0, 0x22C8B0};
// mov [rsp+18h], rbx; push rbp, rsi, rdi, r12, r13, r14, r15; sub rsp, 0C40h
constexpr unsigned char VisibilityTestStart[]{0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41,
    0x57, 0x48, 0x81, 0xEC, 0x40, 0x0C, 0x00, 0x00};
constexpr std::uintptr_t ManagerVtable = 0x21065C8, GroupVtable = 0x2106720, ModelVtable = 0x20F4D90, ResourceVtable = 0x21027C0;
// The texture manager (a global, its vtable) keeps an array of 0x78-byte texture records, as many as
// its capacity (+0x28); a record's handle (+0x40, its vtable) has the texture's path hash at +0x20, its
// record's index at +0x34 and its resource at +0x10, whose first word points to an entry holding the
// ID3D11Texture2D at +0x10. The array's start is found from its streaming list (+0x10 to +0x18,
// 24-byte nodes whose first word points 0x10 into a record): a listed record less its index's records.
// The list leaves a few loaded textures out, so the array is walked, not the list.
constexpr std::uintptr_t TextureManagerGlobal = 0x2B79B18, TextureManagerVtable = 0x21046E8, TextureVtable = 0x2104710;
constexpr std::size_t TextureNodes = 0x10, TextureNodeSize = 24, NodeToRecord = 0x10, TextureCapacity = 0x28, TextureRecordSize = 0x78, RecordTexture = 0x40,
                      TexturePath = 0x20, TextureIndex = 0x34, TextureResource = 0x10, EntryTexture = 0x10;
constexpr std::uint32_t MaxTextureRecords = 1 << 16;

// The manager's record chunks: 24-byte entries (chunk, handle tag, element size); the tag's low 32 bits
// hold 3 in the top nibble and the chunk's number in bits 16 to 23, its high 32 bits other data, set
// from chunk 11 of the largest manager on (read as 64 bits, the walk stopped there and left 60% of
// Africa's placements out). A record in use holds the manager plus RecordInUse, and its owner.
constexpr std::size_t ChunkTable = 0x88, RecordInUse = 0x100;
constexpr std::size_t RecordSize = 0x30, RecordsPerChunk = 256, RecordUse = 0x20, RecordOwner = 0x28;
// An instance group: its model, and the holder of its world matrices (begin, end).
constexpr std::size_t GroupModel = 0x40, GroupMatrices = 0x60, MatricesBegin = 0x20, MatricesEnd = 0x28;
// A model: its world matrix (when an instance itself) and resource, which has the file's path hash,
// the file in memory and its size.
constexpr std::size_t ModelMatrix = 0x40, ModelResource = 0x118, ResourceHash = 0x30, ResourceFile = 0x60, ResourceFileSize = 0x68;
// A model's passes it is hidden from, a bit a pass: the draw lists leave it out of a pass whose bit is
// set (0x25BE00). The game's model handles show and hide it (0x2165F0: 0 shown, all set hidden).
// Levels keep hidden copies of rocks and cliffs, in the visibility manager too.
constexpr std::size_t ModelHiddenPasses = 0x1A4;
constexpr std::uint32_t HiddenFromEveryPass = 0xFFFFFFFF;
// A model's layer flags, which a few draw-list builders test against a pass's (0x25BE00). Bit 0x20 marks
// the silhouette of a soldier the player marked (a copy of its skeleton with glass-flagged meshes, its
// root bone's matrix not a pose): the game draws it in the main view only, through walls (measured
// draw by draw, 2026-10-23: the only two such models, never in a shadow pass). Posed as a character,
// its root-weighted vertices streaked from the soldier to the player.
constexpr std::size_t ModelLayer = 0x1AC;
constexpr std::uint32_t MarkerSilhouetteLayer = 0x20;
// Every model, characters too, is on the object registry's model list (docs/DEVELOPMENT.md, "Every
// model"): the global pointing to the registry, its table of list headers by type number (a header:
// the last object, then the first), the global holding the models' type number, and the link from
// one object to the next.
constexpr std::uintptr_t RegistryGlobal = 0x2BA04A8, RegistryVtable = 0x20F08C8, ModelTypeGlobal = 0x29E51B4;
constexpr std::size_t RegistryLists = 0x1E8, ListFirst = 8, ObjectNext = 0x10;
constexpr std::uint32_t MaxTypes = 256, MaxListedModels = 1 << 16;
// A skinned model (docs/DEVELOPMENT.md, "Characters"): its bone count (16 bits: a flag above it is set
// on animals' and some characters' models, the dog's 0x1004E for 78 bones), the object whose +0x78 holds
// its skinning palette, and its mesh groups' count and state, a byte each: drawn while none of
// HiddenGroupBits is set (measured against a soldier's draws: 13 groups drawn, 33 not).
constexpr std::size_t ModelBoneCount = 0xF8, ModelSkin = 0x188, SkinPalette = 0x78, ModelGroupCount = 0x1E8, ModelGroupStates = 0x178;
constexpr std::uint8_t HiddenGroupBits = 0x33;
constexpr std::uint32_t MaxBones = 1024;
constexpr std::uint32_t MaxChunks = 64, MaxManagers = 8;
constexpr std::uint64_t MaxGroupInstances = 4096, MaxFileBytes = 64ull << 20;
// A model file that does not read as one (still streaming in, or not an FMDL) is looked at again for
// this many snapshots before it is given up.
constexpr std::uint32_t UnreadableRetries = 600;
// New model files read a take at most; the rest wait for the next takes.
constexpr std::uint64_t ParseBytesPerTake = 8ull << 20;

// The scene takes the meshes the game draws into its shadow maps, as its draws showed them mesh by
// mesh (docs/DEVELOPMENT.md, "Scene objects"), by an FMDL mesh's alpha (first byte) and shadow
// (second byte) bits. Shadow bit 0: never drawn into the shadow maps (sky domes kilometres wide
// around the camera have it: they blocked the sky for the global illumination). Alpha bit 6:
// translucent, never drawn into them either. Alpha bit 7: decals, which the game casts or not by the
// material's technique: some do (a pylon's lattice with shadow bit 1, a trench's grating and a tree's
// twigs with shadow 64), others never (fox3DDF_Blin_NC). Until the game's own per-mesh pass mask is
// read, every decal is taken as a cut-out caster (the user's call, 2026-10-23: see what breaks). Alpha
// bit 5 (32): cut out, the shadow pass testing its alpha (m_param.w 1, measured per draw), as it does
// a shadow-only decal's; alpha 16 (glass) is drawn into the shadow maps without the test (m_param.w 0):
// solid.
constexpr std::uint8_t CastsNoShadow = 0x01;
constexpr std::uint8_t CutOutAlpha = 0x20, TranslucentAlpha = 0x40, DecalAlpha = 0x80;
// The StrCode64s of a material's texture parameters "Base_Tex_SRGB" (the colour, and a cut-out mesh's
// alpha) and "Mask_Tex_LIN" (a shadow-only mesh's mask, its only texture): Fox Engine's string hash,
// CityHash64 1.0.3 of the name and its terminating zero with seeds 0x9AE16A3B2F90404F and (first
// character << 16) + length, 48 bits kept.
constexpr std::uint64_t BaseTextureParameter = 0x41EA7BE81B61, MaskTextureParameter = 0x3ED9B75CD480;

using VisibilityTest = void (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t,
    std::uintptr_t, std::uintptr_t, std::uintptr_t);
std::array<VisibilityTest, 2> originals{};
std::uintptr_t base{};
std::atomic<bool> active{};

// Taking a snapshot: one job thread a frame.
std::mutex snapshotMutex;
std::atomic<std::uint64_t> lastSnapshotFrame{};
// The visibility managers the tests ran with (noteManager), a slot each.
std::array<std::atomic<std::uintptr_t>, MaxManagers> seenManagers{};
// Models by path hash: rigid or skinned (its file queued or read), or left out; and the snapshots each
// unreadable file has been tried in.
enum class FileKind : std::uint8_t
{
    LeftOut,
    Rigid,
    Skinned
};
std::unordered_map<std::uint64_t, FileKind> knownModels;
std::unordered_map<std::uint64_t, std::uint32_t> unreadableTries;
std::vector<std::uint8_t> recordBuffer(RecordSize * RecordsPerChunk);
std::vector<float> matrixBuffer;

struct PendingFile
{
    std::uint64_t hash{};
    std::vector<std::uint8_t> bytes;
    bool vegetation{}; // See parseModel
};

// What the snapshots publish for the render thread.
std::mutex publishMutex;
std::vector<gi_renderer::ModelInstance> latestInstances;
std::vector<gi_renderer::CharacterInstance> latestCharacters;
bool latestFresh{};
std::uint64_t latestFrame{}; // The game loop's frame the latest snapshot was taken in

// The skinned models parsed, for their meshes' index ranges and bone groups (take); render thread.
std::unordered_map<std::uint64_t, std::shared_ptr<const gi_renderer::Model>> skinnedModels;

// Diagnostics: the bones as the snapshot took them (setBonesFromSnapshot); and where they came from.
std::atomic<bool> bonesFromSnapshot{};
std::atomic<std::uint64_t> posesCompared{}, posesChanged{}, drawsNoted{}, drawsPlaced{}, meshesPosed{};
std::vector<PendingFile> pendingFiles;
// The terrain inputs last noted, and the game frame they were noted in.
std::mutex terrainMutex;
std::optional<TerrainInputs> terrainInputs;
std::atomic<std::uint64_t> terrainFrame{};

// Diagnostics.
std::atomic<std::uint64_t> snapshots{}, filesRead{}, skinnedFiles{}, unreadableFiles{}, modelsParsed{}, parseFailures{}, shadowlessMeshes{};
std::atomic<std::uint32_t> lastInstances{}, lastOwners{}, lastOtherOwners{}, lastLeftOut{}, lastHidden{}, lastCharacters{}, lastListedModels{};
// The model list was found in the last snapshot: the characters come from it.
std::atomic<bool> modelListFound{};

// Takes a reference to a COM object the game holds; false when it is gone.
bool hold(IUnknown* object) noexcept
{
    __try
    {
        object->AddRef();
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool isA(std::uintptr_t object, std::uintptr_t vtableRva) noexcept
{
    return game_memory::isA(object, base + vtableRva);
}

// A stored world matrix (4x4, column-major, translation in the last column) as the scene's rows. The
// bottom row is (0 0 0 1) give or take rounding: a weapon held in a hand, its matrix made from the
// hand's bone, had 0.99999988 for the 1.
std::optional<gi_renderer::Transform> transformOf(const float* m) noexcept
{
    constexpr float Rounding = 1e-3f;
    if (std::abs(m[3]) > Rounding || std::abs(m[7]) > Rounding || std::abs(m[11]) > Rounding || std::abs(m[15] - 1) > Rounding) return std::nullopt;

    gi_renderer::Transform rows{};
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 4; ++column)
        {
            const float value = m[column * 4 + row];
            if (!std::isfinite(value) || std::abs(value) > 1e6f) return std::nullopt;

            rows[row * 4 + column] = value;
        }

    return rows;
}

// Little-endian fields of a model file, bounds-checked.
struct FileReader
{
    std::span<const std::uint8_t> bytes;

    template<class T> bool at(std::uint64_t offset, T& out) const noexcept
    {
        if (offset > bytes.size() || bytes.size() - offset < sizeof(T)) return false;

        std::memcpy(&out, bytes.data() + offset, sizeof(T));
        return true;
    }
};

// Section 0's blocks (FMDL 2.0x, as the community's FmdlTool reads them): where each starts and how
// many entries it has.
struct Block
{
    std::uint64_t offset{};
    std::uint32_t count{};
};

struct FileLayout
{
    std::array<Block, 32> blocks{};
    std::uint64_t bufferData{}; // Section 1's vertex and index data
    bool hasBones{};
};

std::optional<FileLayout> layoutOf(const FileReader& file) noexcept
{
    std::uint32_t magic{}, count0{}, count1{}, offset0{}, offset1{};
    std::uint64_t sectionInfo{};
    if (!file.at(0, magic) || magic != 0x4C444D46 || !file.at(8, sectionInfo) || !file.at(32, count0) || !file.at(36, count1) || !file.at(40, offset0)
        || !file.at(48, offset1) || count0 > 64 || count1 > 64)
        return std::nullopt;

    FileLayout layout;
    for (std::uint32_t i = 0; i < count0; ++i)
    {
        std::uint16_t type{}, count{};
        std::uint32_t offset{};
        if (!file.at(sectionInfo + 8 * i, type) || !file.at(sectionInfo + 8 * i + 2, count) || !file.at(sectionInfo + 8 * i + 4, offset)) return std::nullopt;

        if (type == 0) layout.hasBones = true;
        if (type < layout.blocks.size()) layout.blocks[type] = {std::uint64_t{offset0} + offset, count};
    }

    for (std::uint32_t i = 0; i < count1; ++i)
    {
        std::uint32_t type{}, offset{};
        const auto at = sectionInfo + 8ull * count0 + 12ull * i;
        if (!file.at(at, type) || !file.at(at + 4, offset)) return std::nullopt;

        if (type == 2) layout.bufferData = std::uint64_t{offset1} + offset;
    }

    if (!layout.bufferData) return std::nullopt;

    return layout;
}

// A vertex stream of a mesh: where its first vertex's element starts, the stride, and the element's type.
struct Stream
{
    std::uint64_t at{};
    std::uint32_t stride{};
    std::uint8_t dataType{};
};

// Each mesh's group (block 2, 32 bytes: the group at +4, the count of its meshes at +6, the first at +8).
std::optional<std::vector<std::uint32_t>> meshGroupsOf(const FileReader& file, const FileLayout& layout)
{
    constexpr int GroupMeshes = 2, Meshes = 3;
    const auto& b = layout.blocks;
    std::vector<std::uint32_t> groups(b[Meshes].count);
    for (std::uint32_t i = 0; i < b[GroupMeshes].count; ++i)
    {
        const auto at = b[GroupMeshes].offset + 32ull * i;
        std::uint16_t group{}, count{}, first{};
        if (!file.at(at + 4, group) || !file.at(at + 6, count) || !file.at(at + 8, first)) return std::nullopt;

        for (std::uint32_t mesh = first; mesh < std::uint32_t{first} + count && mesh < groups.size(); ++mesh) groups[mesh] = group;
    }

    return groups;
}

// The path hash of a mesh's texture for a parameter: its material instance's (block 4: texture count at
// +6, first texture parameter at +8) texture parameter of that name (block 7: the name's StrCode64
// index into block 22, the texture's into block 6), whose texture (block 6: its path's index into
// block 21) has that path hash. 0 when the mesh has none.
std::uint64_t textureOf(const FileReader& file, const FileLayout& layout, std::uint16_t materialInstance, std::uint64_t parameter)
{
    const auto& b = layout.blocks;
    constexpr int MaterialInstances = 4, Textures = 6, Parameters = 7, PathCodes = 21, StrCodes = 22;
    std::uint8_t count{};
    std::uint16_t first{};
    if (materialInstance >= b[MaterialInstances].count || !file.at(b[MaterialInstances].offset + 16ull * materialInstance + 6, count)
        || !file.at(b[MaterialInstances].offset + 16ull * materialInstance + 8, first))
        return 0;

    for (std::uint32_t i = first; i < std::uint32_t{first} + count && i < b[Parameters].count; ++i)
    {
        std::uint16_t name{}, texture{}, path{};
        std::uint64_t nameHash{}, pathHash{};
        if (!file.at(b[Parameters].offset + 4ull * i, name) || !file.at(b[Parameters].offset + 4ull * i + 2, texture) || name >= b[StrCodes].count
            || !file.at(b[StrCodes].offset + 8ull * name, nameHash) || nameHash != parameter)
            continue;

        if (texture < b[Textures].count && file.at(b[Textures].offset + 4ull * texture + 2, path) && path < b[PathCodes].count
            && file.at(b[PathCodes].offset + 8ull * path, pathHash))
            return pathHash;
    }

    return 0;
}

// A skinned mesh's vertices' bones and weights (ModelPart::bones, weights): its four bone numbers a
// vertex index its bone group (block 5, 68 bytes: the count at +2, then 32 of the model's bone numbers;
// the mesh's group at +6), as the game's palette for the mesh is the model's through the group.
// Without numbers a vertex follows the group's first bone; without weights, its first number alone.
bool readSkin(const FileReader& file, const Block& boneGroups, std::uint64_t meshAt, std::uint32_t bones, std::optional<Stream> numbers,
    std::optional<Stream> weights, gi_renderer::ModelPart& part)
{
    std::uint16_t group{}, count{};
    if (!file.at(meshAt + 6, group) || group >= boneGroups.count || !file.at(boneGroups.offset + 68ull * group + 2, count)) return false;

    std::array<std::uint16_t, 32> members{};
    for (std::uint32_t i = 0; i < std::min<std::uint32_t>(count, 32); ++i)
        if (!file.at(boneGroups.offset + 68ull * group + 4 + 2ull * i, members[i])) return false;

    part.boneGroup.assign(members.begin(), members.begin() + std::min<std::uint32_t>(count, 32));

    const auto vertexCount = part.vertices.size();
    part.bones.resize(vertexCount);
    part.weights.resize(vertexCount);
    for (std::size_t v = 0; v < vertexCount; ++v)
    {
        std::array<std::uint8_t, 4> local{};
        std::uint32_t weight = 0xFF;
        if ((numbers && !file.at(numbers->at + v * numbers->stride, local)) || (weights && !file.at(weights->at + v * weights->stride, weight))) return false;

        std::uint64_t packed{};
        for (int k = 0; k < 4; ++k)
        {
            const std::uint64_t bone = local[k] < std::min<std::uint32_t>(count, 32) && members[local[k]] < bones ? members[local[k]] : 0;
            packed |= bone << (16 * k);
        }

        part.bones[v] = packed;
        part.weights[v] = weight;
    }

    return true;
}

// A model's meshes that the game draws into its shadow maps: each one's vertices with their texture
// coordinates, its mesh group, and the indices of its most detailed triangles, as the game keeps them;
// a skinned model's vertices also with their bones (readSkin). The object brush's bushes and grass
// (`vegetation`) cast no shadow in the game (alpha 0xA0, a cut-out decal; shadow bit 0): their meshes
// are taken as cut out all the same, for the rays' shadows.
std::shared_ptr<const gi_renderer::Model> parseModel(std::uint64_t hash, std::span<const std::uint8_t> bytes, bool vegetation)
{
    const FileReader file{bytes};
    const auto layout = layoutOf(file);
    if (!layout) return nullptr;

    const auto& b = layout->blocks;
    constexpr int Bones = 0, Meshes = 3, BoneGroups = 5, Assignments = 9, MeshFormats = 10, VertexFormats = 11, BufferOffsets = 14, FaceInfos = 17;
    // Vertex format usages and data types.
    constexpr std::uint8_t Position = 0, BoneWeights = 1, BoneNumbers = 7, Uv0 = 8, ThreeFloats = 1, TwoHalves = 7, FourUnorms = 8, FourBytes = 9;
    if (!b[BufferOffsets].count) return nullptr;

    std::uint32_t facesOffset{};
    const auto faceBuffer = std::min<std::uint32_t>(b[BufferOffsets].count - 1, 2);
    if (!file.at(b[BufferOffsets].offset + 16ull * faceBuffer + 8, facesOffset)) return nullptr;

    auto model = std::make_shared<gi_renderer::Model>();
    model->hash = hash;
    model->bones = layout->hasBones ? std::min(b[Bones].count, MaxBones) : 0;
    const auto meshGroups = meshGroupsOf(file, *layout);
    if (!meshGroups) return nullptr;

    for (std::uint32_t mesh = 0; mesh < b[Meshes].count; ++mesh)
    {
        const auto meshAt = b[Meshes].offset + 48ull * mesh;
        std::uint8_t alpha{}, shadow{};
        std::uint16_t materialInstance{}, vertexCount{};
        std::uint32_t firstFace{};
        std::uint64_t firstFaceInfo{};
        if (!file.at(meshAt, alpha) || !file.at(meshAt + 1, shadow) || !file.at(meshAt + 4, materialInstance) || !file.at(meshAt + 10, vertexCount)
            || !file.at(meshAt + 16, firstFace) || !file.at(meshAt + 24, firstFaceInfo))
            return nullptr;

        if (((shadow & CastsNoShadow) && !vegetation) || (alpha & TranslucentAlpha))
        {
            ++shadowlessMeshes;
            continue;
        }

        // The mesh's streams: its mesh formats in order, each followed by as many of its vertex
        // formats as it says.
        const auto assignmentAt = b[Assignments].offset + 8ull * mesh;
        std::uint8_t meshFormatCount{};
        std::uint16_t firstMeshFormat{}, firstVertexFormat{};
        if (mesh >= b[Assignments].count || !file.at(assignmentAt, meshFormatCount) || !file.at(assignmentAt + 4, firstMeshFormat)
            || !file.at(assignmentAt + 6, firstVertexFormat))
            return nullptr;

        std::optional<Stream> positions, uvs, weights, numbers;
        std::uint32_t vertexFormat = firstVertexFormat;
        for (std::uint32_t i = firstMeshFormat; i < std::uint32_t{firstMeshFormat} + meshFormatCount && i < b[MeshFormats].count; ++i)
        {
            const auto at = b[MeshFormats].offset + 8ull * i;
            std::uint8_t bufferOffset{}, formats{}, stride{};
            std::uint32_t offset{}, buffer{};
            if (!file.at(at, bufferOffset) || !file.at(at + 1, formats) || !file.at(at + 2, stride) || !file.at(at + 4, offset)) return nullptr;
            if (bufferOffset >= b[BufferOffsets].count || !file.at(b[BufferOffsets].offset + 16ull * bufferOffset + 8, buffer)) return nullptr;

            for (std::uint32_t k = 0; k < formats && vertexFormat < b[VertexFormats].count; ++k, ++vertexFormat)
            {
                const auto formatAt = b[VertexFormats].offset + 4ull * vertexFormat;
                std::uint8_t usage{}, dataType{};
                std::uint16_t element{};
                if (!file.at(formatAt, usage) || !file.at(formatAt + 1, dataType) || !file.at(formatAt + 2, element)) return nullptr;

                const Stream stream{layout->bufferData + buffer + offset + element, stride, dataType};
                if (usage == Position && !positions) positions = stream;
                if (usage == Uv0 && !uvs) uvs = stream;
                if (usage == BoneWeights && !weights) weights = stream;
                if (usage == BoneNumbers && !numbers) numbers = stream;
            }
        }

        if (!positions || positions->dataType != ThreeFloats || firstFaceInfo >= b[FaceInfos].count) return nullptr;

        std::uint32_t lodFirst{}, lodCount{};
        if (!file.at(b[FaceInfos].offset + 8 * firstFaceInfo, lodFirst) || !file.at(b[FaceInfos].offset + 8 * firstFaceInfo + 4, lodCount)) return nullptr;

        // A mesh without two-half texture coordinates has neither alpha nor albedo of its own: a cut-out
        // one is left to the shadow map, any one takes its mean albedo from the screen.
        auto& part = model->parts.emplace_back();
        part.mesh = mesh;
        part.group = (*meshGroups)[mesh];
        part.alphaTested = (alpha & (CutOutAlpha | DecalAlpha)) != 0;
        part.baseTexture = textureOf(file, *layout, materialInstance, BaseTextureParameter);
        if (!part.baseTexture)
        {
            part.baseTexture = textureOf(file, *layout, materialInstance, MaskTextureParameter);
            part.maskInRed = part.baseTexture != 0;
        }

        const bool textured = uvs && uvs->dataType == TwoHalves;
        part.vertices.resize(vertexCount);
        if (textured) part.uvs.resize(vertexCount);
        for (std::uint32_t v = 0; v < vertexCount; ++v)
            if (!file.at(positions->at + std::uint64_t{v} * positions->stride, part.vertices[v])
                || (textured && !file.at(uvs->at + std::uint64_t{v} * uvs->stride, part.uvs[v])))
                return nullptr;

        const auto indices = layout->bufferData + facesOffset + 2ull * (std::uint64_t{firstFace} + lodFirst);
        part.firstIndex = firstFace + lodFirst;
        part.indexCount = lodCount;
        part.indices.resize(lodCount / 3 * 3);
        for (std::uint32_t i = 0; i < part.indices.size(); ++i)
            if (!file.at(indices + 2ull * i, part.indices[i]) || part.indices[i] >= vertexCount) return nullptr;

        if (model->bones && !readSkin(file, b[BoneGroups], meshAt, model->bones, numbers && numbers->dataType == FourBytes ? numbers : std::nullopt,
                                weights && weights->dataType == FourUnorms ? weights : std::nullopt, part))
            return nullptr;

        if (part.indices.empty()) model->parts.pop_back();
    }

    return model;
}

// A model resource's path hash when its model is of the kind `wanted`; its file is queued the first
// time it is seen (`vegetation`: see parseModel). On the job thread taking the snapshot.
std::optional<std::uint64_t> modelResource(std::uintptr_t resource, FileKind wanted, bool vegetation = false)
{
    if (!isA(resource, ResourceVtable)) return std::nullopt;

    const auto hash = read<std::uint64_t>(resource + ResourceHash);
    if (!hash) return std::nullopt;
    if (const auto found = knownModels.find(*hash); found != knownModels.end()) return found->second == wanted ? hash : std::nullopt;

    // A file not loaded yet is looked at again next frame.
    const auto fileAt = read<std::uintptr_t>(resource + ResourceFile);
    const auto size = read<std::uint32_t>(resource + ResourceFileSize);
    if (!fileAt || !*fileAt || !size || *size < 0x40) return std::nullopt;

    PendingFile file{*hash, {}, vegetation};
    if (*size <= MaxFileBytes)
    {
        file.bytes.resize(*size);
        if (!readRaw(file.bytes.data(), *fileAt, file.bytes.size())) return std::nullopt;
    }

    const auto layout = file.bytes.empty() ? std::nullopt : layoutOf(FileReader{file.bytes});
    if (!layout)
    {
        // Possibly still streaming in: given up only after many snapshots.
        if (++unreadableTries[*hash] < UnreadableRetries) return std::nullopt;

        unreadableTries.erase(*hash);
        knownModels[*hash] = FileKind::LeftOut;
        ++unreadableFiles;
        return std::nullopt;
    }

    unreadableTries.erase(*hash);
    const auto kind = layout->hasBones ? FileKind::Skinned : FileKind::Rigid;
    knownModels[*hash] = kind;
    ++(kind == FileKind::Skinned ? skinnedFiles : filesRead);
    {
        std::lock_guard lock(publishMutex);
        pendingFiles.push_back(std::move(file));
    }

    return kind == wanted ? hash : std::nullopt;
}

std::optional<std::uint64_t> rigidModel(std::uintptr_t model)
{
    if (!isA(model, ModelVtable)) return std::nullopt;

    const auto resource = read<std::uintptr_t>(model + ModelResource);
    return resource ? modelResource(*resource, FileKind::Rigid) : std::nullopt;
}

// The game's characters: every skinned model on the object registry's model list the game shows,
// in its pose this frame (docs/DEVELOPMENT.md, "Every model", "Characters"); false without the list.
bool collectCharacters(std::vector<gi_renderer::CharacterInstance>& characters, std::uint32_t& listed)
{
    const auto registry = read<std::uintptr_t>(base + RegistryGlobal);
    const auto type = read<std::uint32_t>(base + ModelTypeGlobal);
    if (!registry || !type || *type >= MaxTypes || !isA(*registry, RegistryVtable)) return false;

    const auto list = read<std::uintptr_t>(*registry + RegistryLists + 8ull * *type);
    auto node = list && *list ? read<std::uintptr_t>(*list + ListFirst) : std::nullopt;
    if (!node) return false;

    for (listed = 0; *node && listed < MaxListedModels; ++listed)
    {
        const auto model = *node;
        if (!isA(model, ModelVtable)) break;

        node = read<std::uintptr_t>(model + ObjectNext);
        if (!node) break;
        if (read<std::uint32_t>(model + ModelHiddenPasses) == HiddenFromEveryPass) continue;
        if (read<std::uint32_t>(model + ModelLayer).value_or(0) & MarkerSilhouetteLayer) continue;

        // The file is queued last: a model that is no character (most of the list's are rigid) has its
        // file read for nothing, again each time the renderer lets go of what it never placed.
        const auto bones = read<std::uint16_t>(model + ModelBoneCount);
        const auto skin = read<std::uintptr_t>(model + ModelSkin);
        const auto palette = skin && *skin ? read<std::uintptr_t>(*skin + SkinPalette) : std::nullopt;
        const auto groups = read<std::uint16_t>(model + ModelGroupCount);
        const auto states = read<std::uintptr_t>(model + ModelGroupStates);
        float matrix[16]{};
        if (!bones || !*bones || *bones > MaxBones || !palette || !*palette || !groups || !states || !readRaw(matrix, model + ModelMatrix, sizeof(matrix)))
            continue;

        const auto transform = transformOf(matrix);
        const auto resource = transform ? read<std::uintptr_t>(model + ModelResource) : std::nullopt;
        const auto hash = resource ? modelResource(*resource, FileKind::Skinned) : std::nullopt;
        if (!hash) continue;

        gi_renderer::CharacterInstance character{model, *hash, *transform, std::vector<float>(std::size_t{*bones} * 12), {}};
        std::vector<std::uint8_t> groupStates(*groups);
        if (!readRaw(character.palette.data(), *palette, character.palette.size() * sizeof(float))
            || (*groups && (!*states || !readRaw(groupStates.data(), *states, groupStates.size()))))
            continue;

        character.shownGroups.resize(*groups);
        for (std::size_t g = 0; g < groupStates.size(); ++g) character.shownGroups[g] = (groupStates[g] & HiddenGroupBits) == 0;
        characters.push_back(std::move(character));
    }

    return true;
}

// Every record in use of the managers seen, their owners' instances, published; with snapshotMutex.
void snapshot()
{
    std::vector<std::uintptr_t> owners;
    for (auto& slot : seenManagers)
    {
        const auto manager = slot.load(std::memory_order_relaxed);
        if (!manager) continue;

        // A manager gone frees its slot for the one that replaces it.
        if (!isA(manager, ManagerVtable))
        {
            auto expected = manager;
            slot.compare_exchange_strong(expected, 0);
            continue;
        }

        const auto table = read<std::uintptr_t>(manager + ChunkTable);
        if (!table) continue;

        for (std::uint32_t chunk = 0; chunk < MaxChunks; ++chunk)
        {
            const auto records = read<std::uintptr_t>(*table + 24 * chunk);
            const auto tag = read<std::uint32_t>(*table + 24 * chunk + 8);
            const auto size = read<std::uint16_t>(*table + 24 * chunk + 16);
            if (!records || !*records || !tag || (*tag >> 28) != 3 || ((*tag >> 16) & 0xFF) != chunk || !size || *size != RecordSize) break;
            if (!readRaw(recordBuffer.data(), *records, recordBuffer.size())) continue;

            for (std::size_t i = 0; i < RecordsPerChunk; ++i)
            {
                std::uintptr_t use{}, owner{};
                std::memcpy(&use, recordBuffer.data() + i * RecordSize + RecordUse, sizeof(use));
                std::memcpy(&owner, recordBuffer.data() + i * RecordSize + RecordOwner, sizeof(owner));
                if (use == manager + RecordInUse && owner) owners.push_back(owner);
            }
        }
    }

    std::sort(owners.begin(), owners.end());
    owners.erase(std::unique(owners.begin(), owners.end()), owners.end());

    std::vector<gi_renderer::ModelInstance> instances;
    std::uint32_t others{}, leftOut{}, hidden{};
    const auto place = [&](std::uint64_t hash, const float* matrix) {
        const auto transform = transformOf(matrix);
        if (!transform)
        {
            ++leftOut;
            return;
        }

        instances.push_back({hash, *transform});
    };

    for (const auto owner : owners)
    {
        const auto vtable = read<std::uintptr_t>(owner);
        if (!vtable) continue;

        if (*vtable == base + GroupVtable)
        {
            const auto model = read<std::uintptr_t>(owner + GroupModel);
            const auto hash = model ? rigidModel(*model) : std::nullopt;
            const auto holder = read<std::uintptr_t>(owner + GroupMatrices);
            const auto begin = holder ? read<std::uintptr_t>(*holder + MatricesBegin) : std::nullopt;
            const auto end = holder ? read<std::uintptr_t>(*holder + MatricesEnd) : std::nullopt;
            if (!hash || !begin || !end || *end < *begin || (*end - *begin) % 64 || (*end - *begin) / 64 > MaxGroupInstances)
            {
                ++leftOut;
                continue;
            }

            matrixBuffer.resize((*end - *begin) / 4);
            if (!readRaw(matrixBuffer.data(), *begin, *end - *begin)) continue;

            for (std::size_t i = 0; i < matrixBuffer.size(); i += 16) place(*hash, matrixBuffer.data() + i);
        }
        else if (*vtable == base + ModelVtable)
        {
            if (read<std::uint32_t>(owner + ModelHiddenPasses) == HiddenFromEveryPass)
            {
                ++hidden;
                continue;
            }

            float matrix[16]{};
            const auto hash = rigidModel(owner);
            if (hash && readRaw(matrix, owner + ModelMatrix, sizeof(matrix))) place(*hash, matrix);
            else ++leftOut;
        }
        else
            ++others;
    }

    // The object brush's vegetation, a plugin's placements in a block at a time.
    engine_brush::collect([&](std::uintptr_t resource, std::span<const gi_renderer::Transform> transforms) {
        if (const auto hash = modelResource(resource, FileKind::Rigid, true))
            for (const auto& transform : transforms) instances.push_back({*hash, transform});
    });

    std::vector<gi_renderer::CharacterInstance> characters;
    std::uint32_t listed{};
    modelListFound = collectCharacters(characters, listed);

    ++snapshots;
    lastInstances = static_cast<std::uint32_t>(instances.size());
    lastOwners = static_cast<std::uint32_t>(owners.size());
    lastOtherOwners = others;
    lastLeftOut = leftOut;
    lastHidden = hidden;
    lastCharacters = static_cast<std::uint32_t>(characters.size());
    lastListedModels = listed;
    std::lock_guard lock(publishMutex);
    latestInstances = std::move(instances);
    latestCharacters = std::move(characters);
    latestFresh = true;
    latestFrame = lastSnapshotFrame.load(std::memory_order_relaxed);
}

// Registers a manager in the first free slot unless it has one; any thread.
void noteManager(std::uintptr_t manager) noexcept
{
    for (auto& slot : seenManagers)
    {
        auto current = slot.load(std::memory_order_relaxed);
        if (current == manager) return;
        if (!current && (slot.compare_exchange_strong(current, manager) || current == manager)) return;
    }
}

// Before the game's visibility test: every manager it runs with is noted, and the first test a game
// frame takes the snapshot.
void beforeVisibilityTest(std::uintptr_t manager) noexcept
{
    if (!active.load(std::memory_order_relaxed)) return;

    // Noted on every test: a manager whose tests never come first in a frame was missed before.
    if (isA(manager, ManagerVtable)) noteManager(manager);

    // Without the game loop's count, about once a 60 Hz frame.
    const std::uint64_t frame = game_loop::currentFrame() ? game_loop::currentFrame() : GetTickCount64() / 16;
    if (lastSnapshotFrame.load(std::memory_order_relaxed) == frame) return;

    std::unique_lock lock(snapshotMutex, std::try_to_lock);
    if (!lock.owns_lock() || lastSnapshotFrame.load(std::memory_order_relaxed) == frame) return;

    try
    {
        lastSnapshotFrame = frame;
        snapshot();
    }
    catch (...)
    {
    }
}

template<std::size_t Index>
void visibilityTest(std::uintptr_t a1, std::uintptr_t a2, std::uintptr_t a3, std::uintptr_t a4, std::uintptr_t a5, std::uintptr_t a6, std::uintptr_t a7,
    std::uintptr_t a8, std::uintptr_t a9, std::uintptr_t a10)
{
    beforeVisibilityTest(a1);
    originals[Index](a1, a2, a3, a4, a5, a6, a7, a8, a9, a10);
}

constexpr VisibilityTest Detours[]{&visibilityTest<0>, &visibilityTest<1>};
}

std::string hook() noexcept
{
    if (!supportedBuild()) return "Unknown game build: the ray-traced scene has none of the game's models.";

    base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    for (const auto rva : VisibilityTestRvas)
        if (std::memcmp(reinterpret_cast<const void*>(base + rva), VisibilityTestStart, sizeof(VisibilityTestStart)))
            return "The visibility tests have unexpected code: the ray-traced scene has none of the game's models.";

    for (std::size_t i = 0; i < std::size(VisibilityTestRvas); ++i)
    {
        auto* target = reinterpret_cast<void*>(base + VisibilityTestRvas[i]);
        auto status = MH_CreateHook(target, reinterpret_cast<void*>(Detours[i]), reinterpret_cast<void**>(&originals[i]));
        if (status == MH_OK) status = MH_EnableHook(target);
        if (status != MH_OK)
        {
            for (std::size_t j = 0; j <= i; ++j) MH_RemoveHook(reinterpret_cast<void*>(base + VisibilityTestRvas[j]));
            return std::string("Hooking the visibility tests failed (") + MH_StatusToString(status) + "): the ray-traced scene has none of the game's models.";
        }
    }

    return "The visibility tests are hooked: the ray-traced scene takes the game's loaded models from its visibility manager." + engine_brush::hook(base);
}

void setActive(bool on) noexcept
{
    active = on;
    if (on) return;

    // Off, the renderer lets its scene go: the model files are read again when next on.
    modelListFound = false;

    try
    {
        std::scoped_lock lock(snapshotMutex, publishMutex);
        knownModels.clear();
        unreadableTries.clear();
        pendingFiles.clear();
        latestInstances.clear();
        latestCharacters.clear();
        latestFresh = false;
        lastSnapshotFrame = 0;
    }
    catch (...)
    {
    }
}

void take(Snapshot& snapshot, const std::vector<gi_inputs::DrawnBones>& drawn)
{
    std::vector<PendingFile> files;
    std::uint64_t snapshotFrame{};
    {
        std::lock_guard lock(publishMutex);
        snapshot.instancesUpdated = latestFresh;
        if (latestFresh)
        {
            snapshot.instances = std::move(latestInstances);
            snapshot.characters = std::move(latestCharacters);
            snapshotFrame = latestFrame;
        }

        latestFresh = false;

        std::uint64_t bytes{};
        std::size_t taken{};
        while (taken < pendingFiles.size() && bytes < ParseBytesPerTake) bytes += pendingFiles[taken++].bytes.size();
        files.assign(std::make_move_iterator(pendingFiles.begin()), std::make_move_iterator(pendingFiles.begin() + taken));
        pendingFiles.erase(pendingFiles.begin(), pendingFiles.begin() + taken);
    }

    // The snapshot reads the bones at the frame's first visibility test, before the game animates them
    // for the frame it draws (measured 2026-10-24: in the helicopter, a lamp's shadow of Snake trailed his
    // moving arms and face). The bones the frame's draws had replace them: each draw's mesh found by its
    // model's place and its index range, its g_vsBone slots written through its bone group. The bones no
    // draw had are read again now, at the kick, which races the game's next frame.
    if (!bonesFromSnapshot.load(std::memory_order_relaxed) && !snapshot.characters.empty())
    {
        std::vector<float> now;
        for (auto& character : snapshot.characters)
        {
            const auto skin = read<std::uintptr_t>(character.id + ModelSkin);
            const auto palette = skin && *skin ? read<std::uintptr_t>(*skin + SkinPalette) : std::nullopt;
            now.resize(character.palette.size());
            if (!palette || !*palette || !readRaw(now.data(), *palette, now.size() * sizeof(float))) continue;

            ++posesCompared;
            if (std::memcmp(now.data(), character.palette.data(), now.size() * sizeof(float)) == 0) continue;

            ++posesChanged;
            character.palette = now;
        }

        // Within 2 cm: the place is a product of the game's float matrices.
        constexpr float SamePlace = 0.02f;
        drawsNoted += drawn.size();
        for (const auto& mesh : drawn)
            for (auto& character : snapshot.characters)
            {
                const auto& t = character.transform;
                if (std::abs(t[3] - mesh.position[0]) > SamePlace || std::abs(t[7] - mesh.position[1]) > SamePlace
                    || std::abs(t[11] - mesh.position[2]) > SamePlace)
                    continue;

                const auto model = skinnedModels.find(character.hash);
                if (model == skinnedModels.end()) continue;

                const auto& parts = model->second->parts;
                const auto part = std::find_if(parts.begin(), parts.end(),
                    [&](const auto& candidate) { return candidate.firstIndex == mesh.firstIndex && candidate.indexCount == mesh.indexCount; });
                if (part == parts.end()) continue;

                ++drawsPlaced;
                for (std::size_t slot = 0; slot < part->boneGroup.size(); ++slot)
                {
                    const std::size_t bone = part->boneGroup[slot];
                    if ((bone + 1) * 12 > character.palette.size()) continue;

                    std::copy_n(mesh.bones.begin() + slot * 12, 12, character.palette.begin() + bone * 12);
                }

                ++meshesPosed;
                break;
            }
    }

    for (const auto& file : files)
    {
        auto parsed = parseModel(file.hash, file.bytes, file.vegetation);
        if (!parsed)
        {
            ++parseFailures;
            continue;
        }

        ++modelsParsed;
        if (parsed->bones) skinnedModels[parsed->hash] = parsed;
        snapshot.newModels.push_back(std::move(parsed));
    }
}

void forget(const std::vector<std::uint64_t>& hashes)
{
    if (hashes.empty()) return;

    std::lock_guard lock(snapshotMutex);
    for (const auto hash : hashes)
    {
        knownModels.erase(hash);
        skinnedModels.erase(hash);
    }
}

void noteTerrain(ID3D11DeviceContext* context, const float* objectConstants, const float* materialConstants) noexcept
{
    const std::uint64_t frame = game_loop::currentFrame() ? game_loop::currentFrame() : GetTickCount64() / 16;
    if (terrainFrame.exchange(frame, std::memory_order_relaxed) == frame) return;

    try
    {
        TerrainInputs inputs;
        ID3D11ShaderResourceView* views[2]{};
        context->VSGetShaderResources(0, 2, views);
        inputs.pages.Attach(views[0]);
        inputs.heights.Attach(views[1]);
        std::memcpy(inputs.converter.data(), materialConstants, sizeof(inputs.converter));
        std::memcpy(inputs.scaleOffset.data(), objectConstants + 40, sizeof(inputs.scaleOffset));
        if (!inputs.pages || !inputs.heights) return;

        std::lock_guard lock(terrainMutex);
        terrainInputs = std::move(inputs);
    }
    catch (...)
    {
    }
}

std::optional<TerrainInputs> takeTerrain()
{
    std::lock_guard lock(terrainMutex);
    return std::exchange(terrainInputs, std::nullopt);
}

std::vector<LoadedTexture> loadedTextures(const std::vector<std::uint64_t>& paths)
{
    std::vector<LoadedTexture> found;
    if (!base || paths.empty()) return found;

    try
    {
        const auto manager = read<std::uintptr_t>(base + TextureManagerGlobal);
        if (!manager || !isA(*manager, TextureManagerVtable)) return found;

        const auto capacity = read<std::uint32_t>(*manager + TextureCapacity);
        const auto begin = read<std::uintptr_t>(*manager + TextureNodes), end = read<std::uintptr_t>(*manager + TextureNodes + 8);
        if (!capacity || *capacity > MaxTextureRecords || !begin || !end || *end < *begin || (*end - *begin) / TextureNodeSize > *capacity) return found;

        // The record array's start, from the first listed record whose handle gives its index.
        std::optional<std::uintptr_t> records;
        for (auto node = *begin; node < *end && !records; node += TextureNodeSize)
        {
            const auto record = read<std::uintptr_t>(node);
            if (!record || !*record || (*record >= *begin && *record < *begin + std::uintptr_t{*capacity} * TextureNodeSize)) continue;

            const auto handle = read<std::uintptr_t>(*record - NodeToRecord + RecordTexture);
            const auto index = handle && isA(*handle, TextureVtable) ? read<std::uint32_t>(*handle + TextureIndex) : std::nullopt;
            if (index && *index < *capacity) records = *record - NodeToRecord - std::uintptr_t{*index} * TextureRecordSize;
        }

        std::vector<std::uint8_t> array(std::size_t{*capacity} * TextureRecordSize);
        if (!records || !readRaw(array.data(), *records, array.size())) return found;

        const std::unordered_set<std::uint64_t> wanted(paths.begin(), paths.end());
        for (std::size_t at = 0; at + TextureRecordSize <= array.size(); at += TextureRecordSize)
        {
            std::uintptr_t handle{};
            std::memcpy(&handle, array.data() + at + RecordTexture, sizeof(handle));
            if (!handle || !isA(handle, TextureVtable)) continue;

            const auto path = read<std::uint64_t>(handle + TexturePath);
            if (!path || !wanted.contains(*path)) continue;

            // Held, then looked up again: the game swaps a texture for a fuller one as its mips stream in,
            // and one swapped while it was taken is left for the next call.
            const auto texture = [&]() -> std::optional<std::uintptr_t> {
                const auto resource = read<std::uintptr_t>(handle + TextureResource);
                const auto entry = resource ? read<std::uintptr_t>(*resource) : std::nullopt;
                return entry ? read<std::uintptr_t>(*entry + EntryTexture) : std::nullopt;
            };
            const auto object = texture();
            if (!object || !*object || !hold(reinterpret_cast<IUnknown*>(*object))) continue;

            Microsoft::WRL::ComPtr<IUnknown> held;
            held.Attach(reinterpret_cast<IUnknown*>(*object));
            LoadedTexture loaded{*path};
            if (texture() == object && SUCCEEDED(held.As(&loaded.texture))) found.push_back(std::move(loaded));
        }
    }
    catch (...)
    {
    }

    return found;
}

void setBonesFromSnapshot(bool on) noexcept
{
    bonesFromSnapshot = on;
}

std::string poseTiming()
{
    if (bonesFromSnapshot.load()) return "characters' bones as the snapshot took them (diagnostics)";

    const auto compared = posesCompared.exchange(0), changed = posesChanged.exchange(0);
    const auto noted = drawsNoted.exchange(0), placed = drawsPlaced.exchange(0), posed = meshesPosed.exchange(0);
    return "characters' bones: " + std::to_string(changed) + " of " + std::to_string(compared) + " palettes moved on since the snapshot; "
        + std::to_string(noted) + " skinned draws, " + std::to_string(placed) + " found their character and mesh, " + std::to_string(posed)
        + " meshes posed as drawn";
}

std::string status()
{
    if (!snapshots.load()) return "the game's scene: no snapshot yet";

    return "the game's scene: " + std::to_string(lastInstances.load()) + " instances from " + std::to_string(lastOwners.load()) + " owners ("
        + std::to_string(lastOtherOwners.load()) + " of other kinds, " + std::to_string(lastLeftOut.load()) + " left out, "
        + std::to_string(lastHidden.load()) + " models the game hides), " + std::to_string(lastCharacters.load()) + " characters of "
        + std::to_string(lastListedModels.load()) + " models listed" + (modelListFound.load() ? "" : " (no model list)") + ", "
        + std::to_string(snapshots.load()) + " snapshots; model files: " + std::to_string(filesRead.load()) + " rigid, "
        + std::to_string(skinnedFiles.load()) + " skinned, " + std::to_string(unreadableFiles.load()) + " unreadable, "
        + std::to_string(modelsParsed.load()) + " parsed, "
        + std::to_string(parseFailures.load()) + " failed; " + std::to_string(shadowlessMeshes.load())
        + " meshes that cast no shadow left out; " + engine_brush::status();
}
}
