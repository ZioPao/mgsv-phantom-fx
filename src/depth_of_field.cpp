#include "depth_of_field.hpp"

#include "depth_of_field_shaders.hpp"
#include "hdr.hpp"
#include "log.hpp"
#include "photo_mode.hpp"
#include "shader_bytecode.hpp"

#include <Windows.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace phantom_fx::depth_of_field
{
namespace
{
using Microsoft::WRL::ComPtr;

// The game's pixel shaders the depth of field works with (docs/DEVELOPMENT.md, "Depth of field").
enum class Role : std::uint8_t
{
    Bake,      // DOF_ScatterBakeFirst: the first pass, reading the scene, the depth and the blur's constants
    Scatter,   // The passes between, which the gather or a lens shot replaces
    Composite, // DOF_ScatterCompositeFinal: the blurred scene into the scene target
    Resolve,   // The deferred resolves with TppTonemap, whose curve the blur undoes
};

struct GameShader
{
    Role role;
    const char* name;
    const char* checksum;
};

constexpr GameShader Shaders[]{
    {Role::Bake, "DOF_ScatterBakeFirst", "2ef2adeb0c715ce006f6d9fab08b3737"},
    {Role::Scatter, "DOF_ScatterBakeSecond", "e49207e64e7b50ba02bcc2b0f45907e6"},
    {Role::Scatter, "DOF_ScatterSprite", "e82da0dcbd06930cbd640c34e2440bae"},
    {Role::Scatter, "DOF_ScatterGather1", "f6046af3b79c5429212eeb93cbfc2ec3"},
    {Role::Scatter, "DOF_ScatterCompositeNear", "28b84529983257033389c1b1e6854915"},
    {Role::Scatter, "DOF_ScatterCompositeFar", "052b3dcbe0ef2e6160ac6580f868e057"},
    {Role::Composite, "DOF_ScatterCompositeFinal", "d1462a415f8e79dba14db617c576407d"},
    {Role::Resolve, "DR_VolFog_TppTonemap", "00a609370c2e8b47a645263f91f6d3e8"},
    {Role::Resolve, "DR_VolFog_TppTonemap_MD", "79c001e295254d8a749e41d1d7e49de7"},
    {Role::Resolve, "DR_TonemapRainFilter", "9a4abc815afc5f121572d695f07af5af"},
    {Role::Resolve, "DR_TonemapRainFilter_NoIR", "47f52852eaccaa83c25745ce790a212c"},
};

// DOF_ScatterBakeFirst's inScene and inDepth, cPSScene and cPSObject; DOF_ScatterCompositeFinal's
// copy of the scene and its depth; the resolves' cPSMaterial.
constexpr UINT BakeSceneSlot = 0, BakeDepthSlot = 1, SceneConstantsSlot = 2, ObjectConstantsSlot = 5;
constexpr UINT CompositeSceneSlot = 1, CompositeDepthSlot = 2, MaterialSlot = 4;
constexpr UINT SceneConstantsSize = 480, ObjectConstantsSize = 208;
// The largest blur radius, as a share of the render height.
constexpr float MaxRadiusShare = 0.05f;
// The game's blur radius is in pixels of a 1920-wide image.
constexpr float GameReferenceWidth = 1920;
// Mip levels of the half-size colour the gather reads.
constexpr UINT HalfColorMips = 4;
// A lens shot leaves out the frames already on their way as it starts, whose camera had not stopped yet.
constexpr std::uint32_t ShotWarmUp = 3;

// The shaders' constant buffer (Parameters in depth_of_field_shaders.hpp).
struct Parameters
{
    std::uint32_t fullSize[2], halfSize[2];
    float halfTexel[2];
    float maxRadius;
    std::uint32_t blades, useLens;
    float aperture, focusDistance, gameUnit, sampleScale;
    std::uint32_t lightInScene;
    float padding[2];
};
static_assert(sizeof(Parameters) == 64);

struct Target
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    ComPtr<ID3D11UnorderedAccessView> access;
    UINT width{}, height{};
};

struct Buffer
{
    ComPtr<ID3D11Buffer> buffer;
    ComPtr<ID3D11ShaderResourceView> view;
    ComPtr<ID3D11UnorderedAccessView> access;
    UINT count{};
};

// A lens shot as the frames being recorded add to it; guarded by resourceMutex.
struct Shot
{
    std::uint32_t id{};
    std::uint32_t warmUp{}, frames{}, total{};
    UINT width{}, height{};
};

// What the overlay asked for, and what the frame being recorded uses.
std::mutex optionsMutex;
Options wanted, current;
// The add-on looks at the game's depth of field passes: the gather is on, or photo mode is.
std::atomic<bool> armed{}, pipelineFailed{};
std::atomic<std::uint32_t> shotFrames{}, shotTotal{};

// Identified game shaders, by the object the game draws with. Holding them keeps their addresses
// from being reused.
std::shared_mutex shaderMutex;
std::unordered_map<IUnknown*, Role> roles;
std::vector<ComPtr<IUnknown>> shaderOwners;

// Set by a context's DOF_ScatterBakeFirst when the add-on replaced it: the game's passes up to its
// DOF_ScatterCompositeFinal are left out, and that one draws the add-on's result. A context records
// on one thread at a time.
thread_local ID3D11DeviceContext* replacingOn{};
thread_local Parameters replacingWith{};
thread_local bool showingShot{};

// ---- Direct3D, guarded by resourceMutex ----
std::mutex resourceMutex;
ComPtr<ID3D11Device> device;
ComPtr<ID3D11ComputeShader> setupShader, dilateShader, gatherShader, accumulateShader, captureShader;
ComPtr<ID3D11PixelShader> compositeShader, showShotShader;
ComPtr<ID3D11SamplerState> linearSampler;
ComPtr<ID3D11Buffer> parameterBuffer;
Buffer lens;    // The blur's scale, focus and limits, and the depth's projection, from the setup pass
Buffer tonemap; // The resolve's TppTonemap curve
Buffer sum;     // A lens shot's frames added up, at the render size
Target halfColor, tileBlur, tileRadius, background, foreground;
Shot shot;
std::uint64_t lastRun{}; // GetTickCount64 of the gather's last frame
std::map<std::string, bool, std::less<>> loggedOnce;

void logOnce(const std::string& message)
{
    if (loggedOnce.emplace(message, true).second) log(message);
}

void check(HRESULT result, const char* what)
{
    if (FAILED(result)) throw std::runtime_error(std::string(what) + " failed (" + hex(static_cast<std::uint32_t>(result)) + ").");
}

ComPtr<ID3DBlob> compile(const char* source, const char* name, const char* profile)
{
    ComPtr<ID3DBlob> code, errors;
    const auto result = D3DCompile(source, std::strlen(source), name, nullptr, nullptr, "main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        code.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(result))
        throw std::runtime_error(std::string("Shader ") + name + " did not compile: "
            + (errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize())
                      : hex(static_cast<std::uint32_t>(result))));

    return code;
}

ComPtr<ID3D11ComputeShader> computeShader(const char* source, const char* name)
{
    const auto code = compile(source, name, "cs_5_0");
    ComPtr<ID3D11ComputeShader> shader;
    check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, shader.GetAddressOf()), name);

    return shader;
}

ComPtr<ID3D11PixelShader> pixelShader(const char* source, const char* name)
{
    const auto code = compile(source, name, "ps_5_0");
    ComPtr<ID3D11PixelShader> shader;
    check(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, shader.GetAddressOf()), name);

    return shader;
}

// A structured buffer the shaders write and read, zeroed.
Buffer structuredBuffer(UINT stride, UINT count, const char* what)
{
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = stride * count;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    desc.StructureByteStride = stride;
    const std::vector<std::uint8_t> zeros(desc.ByteWidth);
    const D3D11_SUBRESOURCE_DATA data{zeros.data(), 0, 0};
    Buffer made{.count = count};
    check(device->CreateBuffer(&desc, &data, made.buffer.GetAddressOf()), what);
    check(device->CreateShaderResourceView(made.buffer.Get(), nullptr, made.view.GetAddressOf()), what);
    check(device->CreateUnorderedAccessView(made.buffer.Get(), nullptr, made.access.GetAddressOf()), what);

    return made;
}

// Compiles the shaders and makes the fixed resources once; call with resourceMutex held. Returns
// false when the depth of field is unavailable.
bool ensurePipeline()
{
    if (setupShader) return true;
    if (!device || pipelineFailed) return false;

    try
    {
        dilateShader = computeShader(shaders::Dilate, "dof-dilate");
        gatherShader = computeShader(shaders::Gather, "dof-gather");
        accumulateShader = computeShader(shaders::Accumulate, "dof-accumulate");
        captureShader = computeShader(shaders::CaptureTonemap, "dof-capture-tonemap");
        compositeShader = pixelShader(shaders::Composite, "dof-composite");
        showShotShader = pixelShader(shaders::ShowShot, "dof-show-shot");

        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        check(device->CreateSamplerState(&sampler, linearSampler.ReleaseAndGetAddressOf()), "Creating the depth of field sampler");

        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = sizeof(Parameters);
        buffer.Usage = D3D11_USAGE_DYNAMIC;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        check(device->CreateBuffer(&buffer, nullptr, parameterBuffer.ReleaseAndGetAddressOf()), "Creating the depth of field constants");

        lens = structuredBuffer(16, 2, "Creating the depth of field lens");
        tonemap = structuredBuffer(16, 1, "Creating the tonemap curve");

        // Last, so it marks a complete pipeline.
        setupShader = computeShader(shaders::Setup, "dof-setup");
        log("Depth of field ready.");

        return true;
    }
    catch (const std::exception& error)
    {
        pipelineFailed = true;
        log(std::string("Depth of field unavailable: ") + error.what());

        return false;
    }
}

// Makes `target` at the size, unless it has it; call with resourceMutex held. Command lists that
// still use the previous texture hold their own references.
void ensureTarget(Target& target, UINT width, UINT height, DXGI_FORMAT format, UINT mips, const char* what)
{
    if (target.texture && target.width == width && target.height == height) return;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = mips;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | (mips > 1 ? D3D11_BIND_RENDER_TARGET : 0);
    desc.MiscFlags = mips > 1 ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
    Target made{.width = width, .height = height};
    check(device->CreateTexture2D(&desc, nullptr, made.texture.GetAddressOf()), what);
    check(device->CreateShaderResourceView(made.texture.Get(), nullptr, made.view.GetAddressOf()), what);
    D3D11_UNORDERED_ACCESS_VIEW_DESC access{};
    access.Format = format;
    access.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    check(device->CreateUnorderedAccessView(made.texture.Get(), &access, made.access.GetAddressOf()), what);
    target = std::move(made);
}

UINT groups(UINT pixels)
{
    return (pixels + 7) / 8;
}

// The gather's textures for the render size; call with resourceMutex held.
void ensureTargets(const Parameters& parameters)
{
    const UINT halfWidth = parameters.halfSize[0], halfHeight = parameters.halfSize[1];
    const bool resized = halfColor.width != halfWidth || halfColor.height != halfHeight;
    ensureTarget(halfColor, halfWidth, halfHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, HalfColorMips, "Creating the depth of field colour");
    ensureTarget(tileBlur, groups(halfWidth), groups(halfHeight), DXGI_FORMAT_R16_FLOAT, 1, "Creating the depth of field tiles");
    ensureTarget(tileRadius, groups(halfWidth), groups(halfHeight), DXGI_FORMAT_R16_FLOAT, 1, "Creating the depth of field tiles");
    ensureTarget(background, halfWidth, halfHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, "Creating the depth of field background");
    ensureTarget(foreground, halfWidth, halfHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, "Creating the depth of field foreground");
    if (resized) log("Depth of field at " + std::to_string(parameters.fullSize[0]) + "x" + std::to_string(parameters.fullSize[1]) + ".");
}

// The size of the 2D texture behind `view`; zero when there is none.
std::pair<UINT, UINT> sizeOf(ID3D11View* view)
{
    ComPtr<ID3D11Resource> resource;
    ComPtr<ID3D11Texture2D> texture;
    D3D11_TEXTURE2D_DESC desc{};
    if (view) view->GetResource(resource.GetAddressOf());
    if (resource && SUCCEEDED(resource.As(&texture))) texture->GetDesc(&desc);

    return {desc.Width, desc.Height};
}

UINT byteWidth(ID3D11Buffer* buffer)
{
    D3D11_BUFFER_DESC desc{};
    if (buffer) buffer->GetDesc(&desc);

    return desc.ByteWidth;
}

std::optional<Role> roleOf(IUnknown* shader)
{
    if (!shader) return std::nullopt;

    std::shared_lock lock(shaderMutex);
    const auto found = roles.find(shader);

    return found != roles.end() ? std::optional(found->second) : std::nullopt;
}

// The game's compute bindings on a deferred context, put back when this goes out of scope.
class SavedComputeState
{
public:
    explicit SavedComputeState(ID3D11DeviceContext* context) : context(context)
    {
        context->CSGetShader(shader.GetAddressOf(), nullptr, nullptr);
        context->CSGetConstantBuffers(0, Buffers, buffers);
        context->CSGetShaderResources(0, Views, views);
        context->CSGetUnorderedAccessViews(0, Accesses, accesses);
        context->CSGetSamplers(0, 1, &sampler);
    }

    ~SavedComputeState()
    {
        context->CSSetShader(shader.Get(), nullptr, 0);
        context->CSSetConstantBuffers(0, Buffers, buffers);
        context->CSSetShaderResources(0, Views, views);
        context->CSSetUnorderedAccessViews(0, Accesses, accesses, nullptr);
        context->CSSetSamplers(0, 1, &sampler);
        for (auto* buffer : buffers)
            if (buffer) buffer->Release();
        for (auto* view : views)
            if (view) view->Release();
        for (auto* access : accesses)
            if (access) access->Release();
        if (sampler) sampler->Release();
    }

    SavedComputeState(const SavedComputeState&) = delete;
    SavedComputeState& operator=(const SavedComputeState&) = delete;

private:
    static constexpr UINT Buffers = 3, Views = 8, Accesses = 3;

    ID3D11DeviceContext* context;
    ComPtr<ID3D11ComputeShader> shader;
    ID3D11Buffer* buffers[Buffers]{};
    ID3D11ShaderResourceView* views[Views]{};
    ID3D11UnorderedAccessView* accesses[Accesses]{};
    ID3D11SamplerState* sampler{};
};

// The game's pixel shader bindings the composite replaces, put back when this goes out of scope.
class SavedPixelState
{
public:
    explicit SavedPixelState(ID3D11DeviceContext* context) : context(context)
    {
        context->PSGetShader(shader.GetAddressOf(), nullptr, nullptr);
        context->PSGetConstantBuffers(0, 1, &buffer);
        context->PSGetShaderResources(0, Views, views);
        context->PSGetSamplers(0, 1, &sampler);
    }

    ~SavedPixelState()
    {
        context->PSSetShader(shader.Get(), nullptr, 0);
        context->PSSetConstantBuffers(0, 1, &buffer);
        context->PSSetShaderResources(0, Views, views);
        context->PSSetSamplers(0, 1, &sampler);
        if (buffer) buffer->Release();
        for (auto* view : views)
            if (view) view->Release();
        if (sampler) sampler->Release();
    }

    SavedPixelState(const SavedPixelState&) = delete;
    SavedPixelState& operator=(const SavedPixelState&) = delete;

private:
    static constexpr UINT Views = 8;

    ID3D11DeviceContext* context;
    ComPtr<ID3D11PixelShader> shader;
    ID3D11Buffer* buffer{};
    ID3D11ShaderResourceView* views[Views]{};
    ID3D11SamplerState* sampler{};
};

void writeParameters(ID3D11DeviceContext* context, const Parameters& parameters)
{
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(context->Map(parameterBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "Writing the depth of field constants");
    std::memcpy(mapped.pData, &parameters, sizeof(parameters));
    context->Unmap(parameterBuffer.Get(), 0);
}

Parameters parametersFor(UINT width, UINT height, const Options& options, const photo_mode::Lens& photo)
{
    const UINT halfWidth = (width + 1) / 2, halfHeight = (height + 1) / 2;
    return {.fullSize = {width, height},
        .halfSize = {halfWidth, halfHeight},
        .halfTexel = {1.0f / static_cast<float>(halfWidth), 1.0f / static_cast<float>(halfHeight)},
        .maxRadius = MaxRadiusShare * static_cast<float>(height),
        .blades = static_cast<std::uint32_t>(options.blades),
        .useLens = photo.active ? 1u : 0u,
        .aperture = photo.aperture,
        .focusDistance = std::max(photo.focusDistance, 0.1f),
        .gameUnit = static_cast<float>(width) / GameReferenceWidth,
        .sampleScale = 0,
        // With HDR the scene holds light, not TppTonemap's image (hdr.hpp).
        .lightInScene = hdr::active() ? 1u : 0u,
        .padding = {}};
}

// Records the setup, dilate and gather passes on `context`; call with resourceMutex held.
void recordGather(ID3D11DeviceContext* context, const Parameters& parameters, ID3D11ShaderResourceView* scene, ID3D11ShaderResourceView* depth,
    ID3D11Buffer* sceneConstants, ID3D11Buffer* objectConstants)
{
    ensureTargets(parameters);
    writeParameters(context, parameters);

    SavedComputeState saved(context);
    ID3D11ShaderResourceView* const none[8]{};
    ID3D11UnorderedAccessView* const noAccess[3]{};
    ID3D11Buffer* buffers[]{parameterBuffer.Get(), objectConstants, sceneConstants};
    context->CSSetConstantBuffers(0, 3, buffers);
    context->CSSetShaderResources(0, 8, none);
    context->CSSetShaderResources(5, 1, tonemap.view.GetAddressOf());
    context->CSSetSamplers(0, 1, linearSampler.GetAddressOf());

    ID3D11ShaderResourceView* setupInputs[]{scene, depth};
    ID3D11UnorderedAccessView* setupOutputs[]{halfColor.access.Get(), tileBlur.access.Get(), lens.access.Get()};
    context->CSSetShaderResources(0, 2, setupInputs);
    context->CSSetUnorderedAccessViews(0, 3, setupOutputs, nullptr);
    context->CSSetShader(setupShader.Get(), nullptr, 0);
    context->Dispatch(groups(halfColor.width), groups(halfColor.height), 1);
    context->CSSetUnorderedAccessViews(0, 3, noAccess, nullptr);
    context->CSSetShaderResources(0, 2, none);
    context->GenerateMips(halfColor.view.Get());

    context->CSSetShaderResources(0, 1, tileBlur.view.GetAddressOf());
    context->CSSetUnorderedAccessViews(0, 1, tileRadius.access.GetAddressOf(), nullptr);
    context->CSSetShader(dilateShader.Get(), nullptr, 0);
    context->Dispatch(groups(tileRadius.width), groups(tileRadius.height), 1);
    context->CSSetUnorderedAccessViews(0, 1, noAccess, nullptr);

    ID3D11ShaderResourceView* gatherInputs[]{halfColor.view.Get(), tileRadius.view.Get()};
    ID3D11UnorderedAccessView* gatherOutputs[]{background.access.Get(), foreground.access.Get()};
    context->CSSetShaderResources(0, 2, gatherInputs);
    context->CSSetUnorderedAccessViews(0, 2, gatherOutputs, nullptr);
    context->CSSetShader(gatherShader.Get(), nullptr, 0);
    context->Dispatch(groups(halfColor.width), groups(halfColor.height), 1);
    context->CSSetUnorderedAccessViews(0, 2, noAccess, nullptr);
}

// Counts this frame into the lens shot; call with resourceMutex held. Returns whether the frame
// adds to the sum.
bool countShotFrame(const photo_mode::Lens& photo, UINT width, UINT height)
{
    // A new shot, or the render size changed under this one: start adding up again.
    if (photo.shot != shot.id || width != shot.width || height != shot.height)
        shot = {.id = photo.shot, .warmUp = ShotWarmUp, .frames = 0, .total = photo.shotFrames, .width = width, .height = height};

    bool adds{};
    if (shot.warmUp) --shot.warmUp;
    else if (shot.frames < shot.total)
    {
        adds = true;
        if (++shot.frames == shot.total)
        {
            photo_mode::shotFinished(shot.id);
            log("Lens shot " + std::to_string(shot.id) + " has its " + std::to_string(shot.total) + " frames.");
        }
    }

    shotFrames = shot.frames;
    shotTotal = shot.total;

    return adds;
}

// Adds the scene to the lens shot's sum; call with resourceMutex held.
void recordShotFrame(ID3D11DeviceContext* context, const Parameters& parameters, ID3D11ShaderResourceView* scene)
{
    const UINT pixels = parameters.fullSize[0] * parameters.fullSize[1];
    if (sum.count != pixels)
    {
        sum = structuredBuffer(12, pixels, "Creating the lens shot's sum");
        log("Lens shot sum at " + std::to_string(parameters.fullSize[0]) + "x" + std::to_string(parameters.fullSize[1]) + ".");
    }

    writeParameters(context, parameters);
    SavedComputeState saved(context);
    if (shot.frames == 1)
    {
        constexpr UINT zeros[4]{};
        context->ClearUnorderedAccessViewUint(sum.access.Get(), zeros);
    }

    ID3D11UnorderedAccessView* const noAccess[1]{};
    context->CSSetConstantBuffers(0, 1, parameterBuffer.GetAddressOf());
    context->CSSetShaderResources(0, 1, &scene);
    context->CSSetShaderResources(5, 1, tonemap.view.GetAddressOf());
    context->CSSetUnorderedAccessViews(0, 1, sum.access.GetAddressOf(), nullptr);
    context->CSSetShader(accumulateShader.Get(), nullptr, 0);
    context->Dispatch(groups(parameters.fullSize[0]), groups(parameters.fullSize[1]), 1);
    context->CSSetUnorderedAccessViews(0, 1, noAccess, nullptr);
}

// DOF_ScatterBakeFirst: the gather or the lens shot from its inputs. False leaves the depth of field
// to the game this frame.
bool bake(ID3D11DeviceContext* context)
{
    replacingOn = nullptr;

    ComPtr<ID3D11ShaderResourceView> scene, depth;
    ComPtr<ID3D11Buffer> sceneConstants, objectConstants;
    context->PSGetShaderResources(BakeSceneSlot, 1, scene.GetAddressOf());
    context->PSGetShaderResources(BakeDepthSlot, 1, depth.GetAddressOf());
    context->PSGetConstantBuffers(SceneConstantsSlot, 1, sceneConstants.GetAddressOf());
    context->PSGetConstantBuffers(ObjectConstantsSlot, 1, objectConstants.GetAddressOf());
    const auto size = sizeOf(scene.Get());
    if (!size.first || sizeOf(depth.Get()) != size || byteWidth(sceneConstants.Get()) != SceneConstantsSize
        || byteWidth(objectConstants.Get()) != ObjectConstantsSize)
    {
        logOnce("Depth of field: DOF_ScatterBakeFirst has unexpected inputs; the game's depth of field stays.");
        return false;
    }

    Options options;
    {
        std::lock_guard lock(optionsMutex);
        options = current;
    }

    const auto photo = photo_mode::lens();
    auto parameters = parametersFor(size.first, size.second, options, photo);

    std::lock_guard lock(resourceMutex);
    if (!ensurePipeline()) return false;

    if (photo.shooting)
    {
        if (countShotFrame(photo, size.first, size.second)) recordShotFrame(context, parameters, scene.Get());
        if (shot.frames)
        {
            parameters.sampleScale = 1.0f / static_cast<float>(shot.frames);
            replacingOn = context;
            replacingWith = parameters;
            showingShot = true;

            return true;
        }
    }

    if (!options.gather) return false;

    recordGather(context, parameters, scene.Get(), depth.Get(), sceneConstants.Get(), objectConstants.Get());
    replacingOn = context;
    replacingWith = parameters;
    showingShot = false;
    lastRun = GetTickCount64();

    return true;
}

// DOF_ScatterCompositeFinal: the add-on's result into the scene target.
bool composite(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call)
{
    const auto parameters = replacingWith;
    const bool shotResult = showingShot;
    replacingOn = nullptr;

    ComPtr<ID3D11ShaderResourceView> scene, depth;
    context->PSGetShaderResources(CompositeSceneSlot, 1, scene.GetAddressOf());
    context->PSGetShaderResources(CompositeDepthSlot, 1, depth.GetAddressOf());
    const std::pair size{parameters.fullSize[0], parameters.fullSize[1]};
    if (sizeOf(scene.Get()) != size || sizeOf(depth.Get()) != size)
    {
        logOnce("Depth of field: DOF_ScatterCompositeFinal has unexpected inputs; the frame stays sharp.");
        return false;
    }

    std::lock_guard lock(resourceMutex);
    writeParameters(context, parameters);
    SavedPixelState saved(context);
    ID3D11ShaderResourceView* const none[8]{};
    context->PSSetShaderResources(0, 8, none);
    context->PSSetConstantBuffers(0, 1, parameterBuffer.GetAddressOf());
    context->PSSetShaderResources(0, 1, scene.GetAddressOf());
    context->PSSetShaderResources(5, 1, tonemap.view.GetAddressOf());
    if (shotResult)
    {
        context->PSSetShaderResources(7, 1, sum.view.GetAddressOf());
        context->PSSetShader(showShotShader.Get(), nullptr, 0);
    }
    else
    {
        ID3D11ShaderResourceView* inputs[]{depth.Get(), background.view.Get(), foreground.view.Get(), halfColor.view.Get()};
        context->PSSetShaderResources(1, 4, inputs);
        context->PSSetShaderResources(6, 1, lens.view.GetAddressOf());
        context->PSSetSamplers(0, 1, linearSampler.GetAddressOf());
        context->PSSetShader(compositeShader.Get(), nullptr, 0);
    }

    context->DrawInstanced(call.count, call.instances, call.first, call.firstInstance);

    return true;
}

// At a deferred resolve: its TppTonemap curve, for the blur to undo.
void captureTonemap(ID3D11DeviceContext* context)
{
    ComPtr<ID3D11Buffer> material;
    context->PSGetConstantBuffers(MaterialSlot, 1, material.GetAddressOf());
    if (byteWidth(material.Get()) < 16) return;

    std::lock_guard lock(resourceMutex);
    if (!ensurePipeline()) return;

    SavedComputeState saved(context);
    ID3D11UnorderedAccessView* const noAccess[1]{};
    context->CSSetConstantBuffers(1, 1, material.GetAddressOf());
    context->CSSetUnorderedAccessViews(0, 1, tonemap.access.GetAddressOf(), nullptr);
    context->CSSetShader(captureShader.Get(), nullptr, 0);
    context->Dispatch(1, 1, 1);
    context->CSSetUnorderedAccessViews(0, 1, noAccess, nullptr);
}
}

void attach(ID3D11Device* gameDevice) noexcept
{
    bool enable{};
    {
        std::lock_guard lock(optionsMutex);
        enable = wanted.gather;
    }

    std::lock_guard lock(resourceMutex);
    device = gameDevice;
    if (enable) ensurePipeline();
}

void detach() noexcept
{
    armed = false;
    {
        std::lock_guard lock(resourceMutex);
        setupShader.Reset();
        dilateShader.Reset();
        gatherShader.Reset();
        accumulateShader.Reset();
        captureShader.Reset();
        compositeShader.Reset();
        showShotShader.Reset();
        linearSampler.Reset();
        parameterBuffer.Reset();
        lens = {};
        tonemap = {};
        sum = {};
        halfColor = {};
        tileBlur = {};
        tileRadius = {};
        background = {};
        foreground = {};
        shot = {};
        device.Reset();
        pipelineFailed = false;
    }

    std::unique_lock lock(shaderMutex);
    roles.clear();
    shaderOwners.clear();
}

void configure(const Options& options) noexcept
{
    {
        std::lock_guard lock(optionsMutex);
        wanted = options;
    }

    if (!options.gather) return;

    std::lock_guard lock(resourceMutex);
    ensurePipeline();
}

std::string problem()
{
    {
        std::lock_guard lock(optionsMutex);
        if (!wanted.gather) return {};
    }

    std::lock_guard lock(resourceMutex);
    if (pipelineFailed) return "The depth of field is unavailable (see PhantomFX.log).";
    if (lastRun && GetTickCount64() - lastRun < 1000) return {};

    return "The depth of field runs in the game's own: it needs Depth of Field on in the game's graphics settings.";
}

ShotProgress shotProgress() noexcept
{
    return {shotFrames, shotTotal};
}

void shaderCreated(const void* code, std::size_t size, IUnknown* shader) noexcept
{
    const auto checksum = dxbcHashOf(code, size);
    if (!checksum || !shader) return;

    const auto text = toHex(*checksum);
    for (const auto& known : Shaders)
    {
        if (text != known.checksum) continue;

        try
        {
            std::unique_lock lock(shaderMutex);
            roles[shader] = known.role;
            shaderOwners.emplace_back(shader);
        }
        catch (...)
        {
            return;
        }

        log(std::string("Identified ") + known.name + ".");
        return;
    }
}

bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call) noexcept
{
    if (!armed.load(std::memory_order_relaxed) || call.instances != 1) return false;

    // The sprites are the only indexed draws, and only matter while the add-on replaces the passes.
    const bool replacing = replacingOn == context;
    if (call.indexed ? !replacing : call.count != 3) return false;

    try
    {
        ComPtr<ID3D11PixelShader> shader;
        context->PSGetShader(shader.GetAddressOf(), nullptr, nullptr);
        const auto role = roleOf(shader.Get());
        if (!role) return false;

        switch (*role)
        {
        case Role::Resolve: captureTonemap(context); return false;
        case Role::Bake: return bake(context);
        case Role::Scatter: return replacing;
        case Role::Composite: return replacing && composite(context, call);
        }
    }
    catch (const std::exception& error)
    {
        replacingOn = nullptr;
        std::lock_guard lock(resourceMutex);
        logOnce(std::string("Depth of field: a pass kept the game's drawing: ") + error.what());
    }
    catch (...)
    {
        replacingOn = nullptr;
    }

    return false;
}

void present() noexcept
{
    const auto photo = photo_mode::lens();
    if (!photo.shooting)
    {
        shotFrames = 0;
        shotTotal = 0;
    }

    std::lock_guard lock(optionsMutex);
    current = wanted;
    armed = (current.gather || photo.active) && !pipelineFailed;
}
}
