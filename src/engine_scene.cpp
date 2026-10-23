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
#include <shared_mutex>
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

// The manager's record chunks: 24-byte entries (chunk, handle tag with 3 in the top nibble, element
// size); a record in use holds the manager plus RecordInUse, and its owner.
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
// translucent, never drawn into them either. Alpha bit 7: decals, unless shadow bit 1 makes the mesh
// a shadow caster the main view does not draw (a pylon's lattice), or alpha 128 comes with shadow 64:
// a cut-out caster (a trench's grating, all 16 draws of 7 such meshes measured went into the shadow
// maps through the alpha-tested shadow shader). Alpha 160 with shadow 64 is mixed: the material's
// technique decides (fox3DDF_Blin_NC never cast), so those stay out until the game's own per-mesh
// pass mask is read. Alpha bit 5 (32): cut out, the shadow pass testing its alpha (m_param.w 1,
// measured per draw), as it does a shadow-only decal's; alpha 16 (glass) is drawn into the shadow
// maps without the test (m_param.w 0): solid.
constexpr std::uint8_t CastsNoShadow = 0x01, ShadowOnly = 0x02, CutOutShadow = 0x40;
constexpr std::uint8_t CutOutAlpha = 0x20, TranslucentAlpha = 0x40, DecalAlpha = 0x80;
// The StrCode64s of a material's texture parameters "Base_Tex_SRGB" (the colour, and a cut-out mesh's
// alpha) and "Mask_Tex_LIN" (a shadow-only mesh's mask, its only texture): Fox Engine's string hash,
// CityHash64 1.0.3 of the name and its terminating zero with seeds 0x9AE16A3B2F90404F and (first
// character << 16) + length, 48 bits kept.
constexpr std::uint64_t BaseTextureParameter = 0x41EA7BE81B61, MaskTextureParameter = 0x3ED9B75CD480;

using VisibilityTest = void (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t,
    std::uintptr_t, std::uintptr_t, std::uintptr_t);
std::array<VisibilityTest, 2> originals{};
// The object brush's hooks are in.
bool brushHooked{};
std::uintptr_t base{};
std::atomic<bool> active{};

// Taking a snapshot: one job thread a frame.
std::mutex snapshotMutex;
std::atomic<std::uint64_t> lastSnapshotFrame{};
std::vector<std::uintptr_t> managers;
// Models by path hash: true when rigid (and its file queued or read), false when left out; and the
// snapshots each unreadable file has been tried in.
std::unordered_map<std::uint64_t, bool> knownModels;
std::unordered_map<std::uint64_t, std::uint32_t> unreadableTries;
std::vector<std::uint8_t> recordBuffer(RecordSize * RecordsPerChunk);
std::vector<float> matrixBuffer;

struct PendingFile
{
    std::uint64_t hash{};
    std::vector<std::uint8_t> bytes;
    bool vegetation{}; // See parseModel
};

// What the snapshots publish for the render thread and the draw hooks.
std::mutex publishMutex;
std::vector<gi_renderer::ModelInstance> latestInstances;
bool latestFresh{};
std::vector<PendingFile> pendingFiles;
// The last snapshot's placements (placementKey) and the hashes of the models placed there: several
// can share one (a pylon is a model and an instance group's model at one transform).
using Placements = std::unordered_map<std::uint64_t, std::vector<std::uint64_t>>;
std::atomic<std::shared_ptr<const Placements>> placements;
// The terrain inputs last noted, and the game frame they were noted in; whether the scene makes the terrain.
std::mutex terrainMutex;
std::optional<TerrainInputs> terrainInputs;
std::atomic<std::uint64_t> terrainFrame{};
std::atomic<bool> engineTerrain{};

// Diagnostics.
std::atomic<std::uint64_t> snapshots{}, filesRead{}, animatedFiles{}, unreadableFiles{}, modelsParsed{}, parseFailures{}, decalMeshes{}, shadowlessMeshes{};
std::atomic<std::uint32_t> lastInstances{}, lastOwners{}, lastOtherOwners{}, lastLeftOut{}, lastHidden{};

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

// FNV-1a over a placement's translation and first rotation element, as the game stores them and
// copies them into the draws' m_world.
std::uint64_t placementKey(float x, float y, float z, float r00) noexcept
{
    const float values[]{x, y, z, r00};
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(values);
    std::uint64_t hash = 14695981039346656037ull;
    for (std::size_t i = 0; i < sizeof(values); ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;

    return hash;
}

// A stored world matrix (4x4, column-major, translation in the last column) as the scene's rows.
std::optional<gi_renderer::Transform> transformOf(const float* m) noexcept
{
    if (m[3] != 0 || m[7] != 0 || m[11] != 0 || m[15] != 1) return std::nullopt;

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

// A rigid model's meshes that the game draws into its shadow maps: each one's vertices with their
// texture coordinates, and the indices of its most detailed triangles, as the game keeps them. The
// object brush's bushes and grass (`vegetation`) cast no shadow in the game (alpha 0xA0, a cut-out
// decal; shadow bit 0): their meshes are taken as cut out all the same, for the rays' shadows.
std::shared_ptr<const gi_renderer::Model> parseModel(std::uint64_t hash, std::span<const std::uint8_t> bytes, bool vegetation)
{
    const FileReader file{bytes};
    const auto layout = layoutOf(file);
    if (!layout || layout->hasBones) return nullptr;

    const auto& b = layout->blocks;
    constexpr int Meshes = 3, Assignments = 9, MeshFormats = 10, VertexFormats = 11, BufferOffsets = 14, FaceInfos = 17;
    // Vertex format usages and data types.
    constexpr std::uint8_t Position = 0, Uv0 = 8, ThreeFloats = 1, TwoHalves = 7;
    if (!b[BufferOffsets].count) return nullptr;

    std::uint32_t facesOffset{};
    const auto faceBuffer = std::min<std::uint32_t>(b[BufferOffsets].count - 1, 2);
    if (!file.at(b[BufferOffsets].offset + 16ull * faceBuffer + 8, facesOffset)) return nullptr;

    auto model = std::make_shared<gi_renderer::Model>();
    model->hash = hash;
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

        const bool decalCaster = (shadow & ShadowOnly) || (alpha == DecalAlpha && (shadow & CutOutShadow));
        if ((alpha & DecalAlpha) && !decalCaster && !vegetation)
        {
            ++decalMeshes;
            continue;
        }

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

        std::optional<Stream> positions, uvs;
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
            }
        }

        if (!positions || positions->dataType != ThreeFloats || firstFaceInfo >= b[FaceInfos].count) return nullptr;

        std::uint32_t lodFirst{}, lodCount{};
        if (!file.at(b[FaceInfos].offset + 8 * firstFaceInfo, lodFirst) || !file.at(b[FaceInfos].offset + 8 * firstFaceInfo + 4, lodCount)) return nullptr;

        // A mesh without two-half texture coordinates has neither alpha nor albedo of its own: a cut-out
        // one is left to the shadow map, any one takes its mean albedo from the screen.
        auto& part = model->parts.emplace_back();
        part.mesh = mesh;
        part.alphaTested = (alpha & CutOutAlpha) || ((alpha & DecalAlpha) && decalCaster);
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
        part.indices.resize(lodCount / 3 * 3);
        for (std::uint32_t i = 0; i < part.indices.size(); ++i)
            if (!file.at(indices + 2ull * i, part.indices[i]) || part.indices[i] >= vertexCount) return nullptr;

        if (part.indices.empty()) model->parts.pop_back();
    }

    return model;
}

// A model resource's path hash when its model is rigid; its file is queued the first time it is seen
// (`vegetation`: see parseModel). On the job thread taking the snapshot.
std::optional<std::uint64_t> rigidResource(std::uintptr_t resource, bool vegetation = false)
{
    if (!isA(resource, ResourceVtable)) return std::nullopt;

    const auto hash = read<std::uint64_t>(resource + ResourceHash);
    if (!hash) return std::nullopt;
    if (const auto found = knownModels.find(*hash); found != knownModels.end()) return found->second ? hash : std::nullopt;

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
        knownModels[*hash] = false;
        ++unreadableFiles;
        return std::nullopt;
    }

    unreadableTries.erase(*hash);
    knownModels[*hash] = !layout->hasBones;
    if (layout->hasBones)
    {
        ++animatedFiles;
        return std::nullopt;
    }

    ++filesRead;
    std::lock_guard lock(publishMutex);
    pendingFiles.push_back(std::move(file));
    return hash;
}

std::optional<std::uint64_t> rigidModel(std::uintptr_t model)
{
    if (!isA(model, ModelVtable)) return std::nullopt;

    const auto resource = read<std::uintptr_t>(model + ModelResource);
    return resource ? rigidResource(*resource) : std::nullopt;
}

// Every record in use of the managers seen, their owners' instances, published; with snapshotMutex.
void snapshot()
{
    std::vector<std::uintptr_t> owners;
    for (const auto manager : managers)
    {
        const auto table = read<std::uintptr_t>(manager + ChunkTable);
        if (!table) continue;

        for (std::uint32_t chunk = 0; chunk < MaxChunks; ++chunk)
        {
            const auto records = read<std::uintptr_t>(*table + 24 * chunk);
            const auto tag = read<std::uint64_t>(*table + 24 * chunk + 8);
            const auto size = read<std::uint16_t>(*table + 24 * chunk + 16);
            if (!records || !*records || !tag || (*tag >> 28) != 3 || !size || *size != RecordSize) break;
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
    Placements placed;
    std::uint32_t others{}, leftOut{}, hidden{};
    const auto place = [&](std::uint64_t hash, const float* matrix) {
        const auto transform = transformOf(matrix);
        if (!transform)
        {
            ++leftOut;
            return;
        }

        instances.push_back({hash, *transform});
        auto& models = placed[placementKey(matrix[12], matrix[13], matrix[14], matrix[0])];
        if (std::find(models.begin(), models.end(), hash) == models.end()) models.push_back(hash);
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
        if (const auto hash = rigidResource(resource, true))
            for (const auto& transform : transforms) instances.push_back({*hash, transform});
    });

    ++snapshots;
    lastInstances = static_cast<std::uint32_t>(instances.size());
    lastOwners = static_cast<std::uint32_t>(owners.size());
    lastOtherOwners = others;
    lastLeftOut = leftOut;
    lastHidden = hidden;
    placements.store(std::make_shared<const Placements>(std::move(placed)));
    std::lock_guard lock(publishMutex);
    latestInstances = std::move(instances);
    latestFresh = true;
}

// Before the game's visibility test: the first one a game frame takes the snapshot.
void beforeVisibilityTest(std::uintptr_t manager) noexcept
{
    if (!active.load(std::memory_order_relaxed)) return;

    // Without the game loop's count, about once a 60 Hz frame.
    const std::uint64_t frame = game_loop::currentFrame() ? game_loop::currentFrame() : GetTickCount64() / 16;
    if (lastSnapshotFrame.load(std::memory_order_relaxed) == frame) return;

    std::unique_lock lock(snapshotMutex, std::try_to_lock);
    if (!lock.owns_lock() || lastSnapshotFrame.load(std::memory_order_relaxed) == frame || !isA(manager, ManagerVtable)) return;

    try
    {
        if (std::find(managers.begin(), managers.end(), manager) == managers.end() && managers.size() < MaxManagers) managers.push_back(manager);
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
    if (!supportedBuild()) return "Unknown game build; the ray-traced scene captures every mesh as it is drawn.";

    base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    for (const auto rva : VisibilityTestRvas)
        if (std::memcmp(reinterpret_cast<const void*>(base + rva), VisibilityTestStart, sizeof(VisibilityTestStart)))
            return "The visibility tests have unexpected code; the ray-traced scene captures every mesh as it is drawn.";

    for (std::size_t i = 0; i < std::size(VisibilityTestRvas); ++i)
    {
        auto* target = reinterpret_cast<void*>(base + VisibilityTestRvas[i]);
        auto status = MH_CreateHook(target, reinterpret_cast<void*>(Detours[i]), reinterpret_cast<void**>(&originals[i]));
        if (status == MH_OK) status = MH_EnableHook(target);
        if (status != MH_OK)
        {
            for (std::size_t j = 0; j <= i; ++j) MH_RemoveHook(reinterpret_cast<void*>(base + VisibilityTestRvas[j]));
            return std::string("Hooking the visibility tests failed (") + MH_StatusToString(status) + "); the ray-traced scene captures every mesh as it is drawn.";
        }
    }

    const auto brush = engine_brush::hook(base);
    brushHooked = engine_brush::hooked();
    return "The visibility tests are hooked: the ray-traced scene takes the game's loaded models from its visibility manager." + brush;
}

void setActive(bool on) noexcept
{
    active = on;
    if (on) return;

    // Off, the draws are captured again, and the renderer lets its scene go: the model files are read
    // again when next on.
    placements.store(nullptr);
    engineTerrain = false;

    try
    {
        std::scoped_lock lock(snapshotMutex, publishMutex);
        knownModels.clear();
        unreadableTries.clear();
        pendingFiles.clear();
        latestInstances.clear();
        latestFresh = false;
        lastSnapshotFrame = 0;
    }
    catch (...)
    {
    }
}

void take(Snapshot& snapshot)
{
    std::vector<PendingFile> files;
    {
        std::lock_guard lock(publishMutex);
        snapshot.instancesUpdated = latestFresh;
        if (latestFresh) snapshot.instances = std::move(latestInstances);
        latestFresh = false;

        std::uint64_t bytes{};
        std::size_t taken{};
        while (taken < pendingFiles.size() && bytes < ParseBytesPerTake) bytes += pendingFiles[taken++].bytes.size();
        files.assign(std::make_move_iterator(pendingFiles.begin()), std::make_move_iterator(pendingFiles.begin() + taken));
        pendingFiles.erase(pendingFiles.begin(), pendingFiles.begin() + taken);
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
        snapshot.newModels.push_back(std::move(parsed));
    }
}

void forget(const std::vector<std::uint64_t>& hashes)
{
    if (hashes.empty()) return;

    std::lock_guard lock(snapshotMutex);
    for (const auto hash : hashes) knownModels.erase(hash);
}

bool clonesFromEngine() noexcept
{
    return brushHooked && active.load(std::memory_order_relaxed);
}

bool covers(const float* worldRows) noexcept
{
    const auto placed = placements.load();
    return placed && placed->contains(placementKey(worldRows[3], worldRows[7], worldRows[11], worldRows[0]));
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

bool terrainFromEngine() noexcept
{
    return engineTerrain.load(std::memory_order_relaxed);
}

void setTerrainFromEngine(bool on) noexcept
{
    engineTerrain = on;
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

std::string status()
{
    if (!snapshots.load()) return "the game's scene: no snapshot yet";

    return "the game's scene: " + std::to_string(lastInstances.load()) + " instances from " + std::to_string(lastOwners.load()) + " owners ("
        + std::to_string(lastOtherOwners.load()) + " of other kinds, " + std::to_string(lastLeftOut.load()) + " left out, "
        + std::to_string(lastHidden.load()) + " models the game hides), " + std::to_string(snapshots.load()) + " snapshots; model files: " + std::to_string(filesRead.load()) + " read, "
        + std::to_string(animatedFiles.load()) + " animated, " + std::to_string(unreadableFiles.load()) + " unreadable, "
        + std::to_string(modelsParsed.load()) + " parsed, "
        + std::to_string(parseFailures.load()) + " failed; " + std::to_string(decalMeshes.load()) + " decal meshes and "
        + std::to_string(shadowlessMeshes.load()) + " meshes that cast no shadow left out; " + engine_brush::status();
}
}
