#include "ui_redirect.hpp"

#include "d3d11_helpers.hpp"
#include "features/hdr/hdr.hpp"
#include "game_constants.hpp"
#include "log.hpp"
#include "render_scale.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>

namespace phantom_fx::ui_redirect
{
namespace
{
using Microsoft::WRL::ComPtr;

// The scene target is B8G8R8A8_TYPELESS with UNORM views to draw and an UNORM_SRGB view that
// GammaControl reads (docs/DEVELOPMENT.md, "Resources"). With HDR it is float, with float views
// holding the same encoded values (hdr.hpp).
DXGI_FORMAT drawFormat()
{
    return hdr::active() ? hdr::sceneFormat() : DXGI_FORMAT_B8G8R8A8_UNORM;
}

DXGI_FORMAT gammaFormat()
{
    return hdr::active() ? hdr::sceneFormat() : DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}

// The output-size textures, by Surface.
struct Texture
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11ShaderResourceView> view;
    unsigned width{}, height{};
};

struct Targets
{
    unsigned width{}, height{};
    Texture surfaces[4]; // Scene, Copy, Blur, BlurTwin
    ComPtr<ID3D11ShaderResourceView> srgbView;
    // The 2D pass masks with stencil; it does not test depth.
    ComPtr<ID3D11DepthStencilView> depth;
    // cPSSystem (slot b0) for the output size. The 2D pass's pixel shaders read only m_renderInfo (c1)
    // and m_renderBuffer (c2) from it, to place screen-space effects.
    ComPtr<ID3D11Buffer> systemConstants;
    // cPSSystem for a grading draw: the game's, copied before each, with c1 and c2 from
    // `systemConstants`. The grading samples the scene by pixel position over m_renderBuffer.zw.
    ComPtr<ID3D11Buffer> postSystemConstants;
    // cPSScene (slot b2): the game's, copied before each draw, with m_viewportSize.xy (c22) set to
    // the output size from `viewportSize`. The camera filters measure the screen with it, and the
    // sandstorm's vignette (Draw2D_TppNoiseFilter) is placed by pixel position over it.
    ComPtr<ID3D11Buffer> sceneConstants, viewportSize;

    const Texture& operator[](Surface surface) const { return surfaces[static_cast<std::size_t>(surface)]; }
};

// Whether each recording context has a 2D pass vertex shader bound, by the context ReShade reports
// with each binding.
struct BoundShader
{
    std::atomic<ID3D11DeviceContext*> context{};
    std::atomic<bool> draw2D{};
};

std::atomic<unsigned> outputWidth{}, outputHeight{};

// The game has 24 deferred contexts and the immediate one.
std::array<BoundShader, 64> boundShaders;
std::atomic<bool> loggedDraw{};

std::mutex mutex;
ComPtr<ID3D11Device> device;
std::shared_ptr<const Targets> targets;
// The render-size scene target the 2D pass last meant, and a view to stretch it from.
ComPtr<ID3D11Resource> scene;
ComPtr<ID3D11ShaderResourceView> sceneView;
bool creationFailed{};

bool belowOutput(render_scale::Size render)
{
    return render.width && render.width < outputWidth && render.height < outputHeight;
}

// The engine renders below the output size, and `texture` has its render size.
bool renderSized(const D3D11_TEXTURE2D_DESC& texture)
{
    const auto render = render_scale::current();
    return texture.Width == render.width && texture.Height == render.height && belowOutput(render);
}

bool sceneTarget(const D3D11_TEXTURE2D_DESC& texture)
{
    return texture.Format == hdr::sceneFormat() && renderSized(texture);
}

bool makeSurface(Texture& surface, const D3D11_TEXTURE2D_DESC& desc, DXGI_FORMAT viewFormat)
{
    const D3D11_RENDER_TARGET_VIEW_DESC targetDesc{viewFormat, D3D11_RTV_DIMENSION_TEXTURE2D};
    D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{viewFormat, D3D11_SRV_DIMENSION_TEXTURE2D};
    viewDesc.Texture2D.MipLevels = 1;
    surface.width = desc.Width;
    surface.height = desc.Height;

    return SUCCEEDED(device->CreateTexture2D(&desc, nullptr, surface.texture.GetAddressOf()))
        && SUCCEEDED(device->CreateRenderTargetView(surface.texture.Get(), &targetDesc, surface.target.GetAddressOf()))
        && SUCCEEDED(device->CreateShaderResourceView(surface.texture.Get(), &viewDesc, surface.view.GetAddressOf()));
}

std::shared_ptr<const Targets> createTargets(unsigned width, unsigned height)
{
    auto created = std::make_shared<Targets>();
    created->width = width;
    created->height = height;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = hdr::sceneFormat();
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    // The motion blur's pair as the game makes its own: half the size of the scene it blends over, and
    // 8-bit, so moving HDR highlights clip as they do without upscaling.
    auto blurDesc = desc;
    blurDesc.Width = std::max(width / 2, 1u);
    blurDesc.Height = std::max(height / 2, 1u);
    blurDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;

    auto& [outputScene, outputCopy, blur, blurTwin] = created->surfaces;
    if (!makeSurface(outputScene, desc, drawFormat()) || !makeSurface(outputCopy, desc, drawFormat()) || !makeSurface(blur, blurDesc, blurDesc.Format)
        || !makeSurface(blurTwin, blurDesc, blurDesc.Format))
        return nullptr;

    const float system[16]{0, 0, 0, 0, static_cast<float>(width), static_cast<float>(height), 0, 0, static_cast<float>(width),
        static_cast<float>(height), 1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height)};
    const D3D11_BUFFER_DESC systemDesc{sizeof(system), D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER};
    const D3D11_SUBRESOURCE_DATA systemData{system};
    const D3D11_BUFFER_DESC postSystemDesc{sizeof(system), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
    if (FAILED(device->CreateBuffer(&systemDesc, &systemData, created->systemConstants.GetAddressOf()))
        || FAILED(device->CreateBuffer(&postSystemDesc, nullptr, created->postSystemConstants.GetAddressOf())))
        return nullptr;

    const D3D11_BUFFER_DESC sceneDesc{sizeof(game_constants::SceneConstants), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
    const float viewport[4]{static_cast<float>(width), static_cast<float>(height)};
    const D3D11_BUFFER_DESC viewportDesc{sizeof(viewport), D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER};
    const D3D11_SUBRESOURCE_DATA viewportData{viewport};
    if (FAILED(device->CreateBuffer(&sceneDesc, nullptr, created->sceneConstants.GetAddressOf()))
        || FAILED(device->CreateBuffer(&viewportDesc, &viewportData, created->viewportSize.GetAddressOf())))
        return nullptr;

    D3D11_SHADER_RESOURCE_VIEW_DESC srgbDesc{gammaFormat(), D3D11_SRV_DIMENSION_TEXTURE2D};
    srgbDesc.Texture2D.MipLevels = 1;
    desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depth;
    if (FAILED(device->CreateShaderResourceView(outputScene.texture.Get(), &srgbDesc, created->srgbView.GetAddressOf()))
        || FAILED(device->CreateTexture2D(&desc, nullptr, depth.GetAddressOf()))
        || FAILED(device->CreateDepthStencilView(depth.Get(), nullptr, created->depth.GetAddressOf())))
        return nullptr;

    return created;
}

// The output-size targets, made when first needed and again when the output size changes.
std::shared_ptr<const Targets> currentTargets()
{
    const unsigned width = outputWidth, height = outputHeight;
    std::lock_guard lock(mutex);
    if (targets && targets->width == width && targets->height == height) return targets;
    if (!device || !width || !height || creationFailed) return nullptr;

    targets = createTargets(width, height);
    creationFailed = !targets;
    log(targets ? "Created the UI target at " + std::to_string(width) + "x" + std::to_string(height) + "."
                : std::string("Could not create the UI target; the UI renders at the render size."));

    return targets;
}

void rememberScene(ID3D11View* view)
{
    ComPtr<ID3D11Resource> resource;
    view->GetResource(resource.GetAddressOf());
    std::lock_guard lock(mutex);
    if (resource == scene) return;

    scene = resource;
    sceneView.Reset();
    D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{drawFormat(), D3D11_SRV_DIMENSION_TEXTURE2D};
    viewDesc.Texture2D.MipLevels = 1;
    if (device) device->CreateShaderResourceView(scene.Get(), &viewDesc, sceneView.GetAddressOf());
}

void issue(ID3D11DeviceContext* context, const DrawCall& call)
{
    if (call.indexed) context->DrawIndexedInstanced(call.count, call.instances, call.first, call.vertexOffset, call.firstInstance);
    else context->DrawInstanced(call.count, call.instances, call.first, call.firstInstance);
}

LONG scaled(LONG value, double scale)
{
    return static_cast<LONG>(std::lround(value * scale));
}

// The game's viewports and scissors, from the render size to the output size.
void scale(const D3D11_VIEWPORT* viewports, D3D11_VIEWPORT* scaledViewports, UINT count, double scaleX, double scaleY)
{
    for (UINT i = 0; i < count; ++i)
    {
        scaledViewports[i] = viewports[i];
        scaledViewports[i].TopLeftX = static_cast<float>(viewports[i].TopLeftX * scaleX);
        scaledViewports[i].TopLeftY = static_cast<float>(viewports[i].TopLeftY * scaleY);
        scaledViewports[i].Width = static_cast<float>(viewports[i].Width * scaleX);
        scaledViewports[i].Height = static_cast<float>(viewports[i].Height * scaleY);
    }
}

void scale(const D3D11_RECT* rects, D3D11_RECT* scaledRects, UINT count, double scaleX, double scaleY)
{
    for (UINT i = 0; i < count; ++i)
        scaledRects[i] = {scaled(rects[i].left, scaleX), scaled(rects[i].top, scaleY), scaled(rects[i].right, scaleX), scaled(rects[i].bottom, scaleY)};
}

// Fills the output's grading cPSSystem from the game's on `context`: the game's values, with
// m_renderInfo (c1) and m_renderBuffer (c2) at the output size.
bool copySystem(ID3D11DeviceContext* context, const Targets& output, ID3D11Buffer* gameSystem)
{
    D3D11_BUFFER_DESC desc{};
    if (gameSystem) gameSystem->GetDesc(&desc);
    using game_constants::SystemConstants;
    if (!gameSystem || desc.ByteWidth != sizeof(SystemConstants)) return false;

    constexpr UINT renderInfoOffset = offsetof(SystemConstants, renderInfo), sizesEnd = offsetof(SystemConstants, dominantLightDir);
    const D3D11_BOX sizes{renderInfoOffset, 0, 0, sizesEnd, 1, 1};
    context->CopyResource(output.postSystemConstants.Get(), gameSystem);
    context->CopySubresourceRegion(output.postSystemConstants.Get(), 0, renderInfoOffset, 0, 0, output.systemConstants.Get(), 0, &sizes);

    return true;
}

// Fills the output's cPSScene from the game's on `context`. The game's m_viewportSize.xy is the
// render size, the size of the scene target the draw was meant for.
bool copyScene(ID3D11DeviceContext* context, const Targets& output, ID3D11Buffer* gameScene)
{
    D3D11_BUFFER_DESC desc{};
    if (gameScene) gameScene->GetDesc(&desc);
    if (!gameScene || desc.ByteWidth != sizeof(game_constants::SceneConstants)) return false;

    constexpr UINT viewportSizeOffset = offsetof(game_constants::SceneConstants, width);
    const D3D11_BOX viewportSize{0, 0, 0, 2 * sizeof(float), 1, 1};
    context->CopyResource(output.sceneConstants.Get(), gameScene);
    context->CopySubresourceRegion(output.sceneConstants.Get(), 0, viewportSizeOffset, 0, 0, output.viewportSize.Get(), 0, &viewportSize);

    return true;
}

BoundShader* slotOf(ID3D11DeviceContext* context, bool add)
{
    for (auto& slot : boundShaders)
    {
        auto* owner = slot.context.load(std::memory_order_acquire);
        if (owner == context) return &slot;
        if (owner) continue;
        if (!add) return nullptr;

        ID3D11DeviceContext* empty{};
        if (slot.context.compare_exchange_strong(empty, context) || empty == context) return &slot;
    }

    return nullptr;
}
}

void vertexShaderBound(ID3D11DeviceContext* context, bool isDraw2D) noexcept
{
    if (auto* slot = slotOf(context, isDraw2D)) slot->draw2D.store(isDraw2D, std::memory_order_relaxed);
}

void contextFinished(ID3D11DeviceContext* context) noexcept
{
    if (auto* slot = slotOf(context, false)) slot->draw2D.store(false, std::memory_order_relaxed);
}

bool drawing2D(ID3D11DeviceContext* context) noexcept
{
    const auto* slot = slotOf(context, false);
    return slot && slot->draw2D.load(std::memory_order_relaxed);
}

bool drawingIntoScene(ID3D11DeviceContext* context) noexcept
{
    ComPtr<ID3D11RenderTargetView> view;
    context->OMGetRenderTargets(1, view.GetAddressOf(), nullptr);
    const auto texture = textureDesc(view.Get());
    const auto render = render_scale::current();

    return texture.Format == hdr::sceneFormat() && texture.Width == render.width && texture.Height == render.height;
}

void attach(ID3D11Device* gameDevice) noexcept
{
    std::lock_guard lock(mutex);
    device = gameDevice;
    creationFailed = false;
}

void detach() noexcept
{
    {
        std::lock_guard lock(mutex);
        targets.reset();
        sceneView.Reset();
        scene.Reset();
        device.Reset();
    }
}

void setOutputSize(unsigned width, unsigned height) noexcept
{
    outputWidth = width;
    outputHeight = height;
}

bool draw2D(ID3D11DeviceContext* context, const DrawCall& call) noexcept
{
    try
    {
        std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> bound{};
        ComPtr<ID3D11DepthStencilView> depth;
        context->OMGetRenderTargets(static_cast<UINT>(bound.size()), bound.data(), depth.GetAddressOf());
        std::array<ComPtr<ID3D11RenderTargetView>, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> views;
        for (std::size_t i = 0; i < bound.size(); ++i) views[i].Attach(bound[i]);

        const auto target = textureDesc(views[0].Get());
        if (!sceneTarget(target)) return false;

        for (std::size_t i = 1; i < views.size(); ++i)
            if (views[i]) return false;

        const auto depthDesc = textureDesc(depth.Get());
        if (depth && (depthDesc.Width != target.Width || depthDesc.Height != target.Height)) return false;

        const auto output = currentTargets();
        if (!output) return false;

        // The 2D pass draws in clip space, so scaling its viewports and scissors is all it takes.
        const double scaleX = static_cast<double>(output->width) / target.Width, scaleY = static_cast<double>(output->height) / target.Height;
        D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{}, scaledViewports[std::size(viewports)]{};
        D3D11_RECT rects[std::size(viewports)]{}, scaledRects[std::size(viewports)]{};
        UINT viewportCount = static_cast<UINT>(std::size(viewports)), rectCount = viewportCount;
        context->RSGetViewports(&viewportCount, viewports);
        context->RSGetScissorRects(&rectCount, rects);
        scale(viewports, scaledViewports, viewportCount, scaleX, scaleY);
        scale(rects, scaledRects, rectCount, scaleX, scaleY);

        ComPtr<ID3D11Buffer> system, gameScene;
        context->PSGetConstantBuffers(0, 1, system.GetAddressOf());
        context->PSGetConstantBuffers(2, 1, gameScene.GetAddressOf());
        const bool sceneCopied = copyScene(context, *output, gameScene.Get());
        context->OMSetRenderTargetsAndUnorderedAccessViews(1, (*output)[Surface::Scene].target.GetAddressOf(), depth ? output->depth.Get() : nullptr, 0,
            D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
        context->RSSetViewports(viewportCount, scaledViewports);
        context->RSSetScissorRects(rectCount, scaledRects);
        context->PSSetConstantBuffers(0, 1, output->systemConstants.GetAddressOf());
        if (sceneCopied) context->PSSetConstantBuffers(2, 1, output->sceneConstants.GetAddressOf());
        issue(context, call);

        context->OMSetRenderTargetsAndUnorderedAccessViews(1, bound.data(), depth.Get(), 0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
        context->RSSetViewports(viewportCount, viewports);
        context->RSSetScissorRects(rectCount, rects);
        context->PSSetConstantBuffers(0, 1, system.GetAddressOf());
        if (sceneCopied) context->PSSetConstantBuffers(2, 1, gameScene.GetAddressOf());
        rememberScene(views[0].Get());

        if (!loggedDraw.exchange(true))
            log("The 2D pass draws at " + std::to_string(output->width) + "x" + std::to_string(output->height) + " while the engine renders at "
                + std::to_string(target.Width) + "x" + std::to_string(target.Height) + ".");

        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool clearTarget(ID3D11DeviceContext* context, ID3D11RenderTargetView* view, const float color[4]) noexcept
{
    try
    {
        if (!drawing2D(context) || !sceneTarget(textureDesc(view))) return false;

        const auto output = currentTargets();
        if (!output) return false;

        context->ClearRenderTargetView((*output)[Surface::Scene].target.Get(), color);
        rememberScene(view);

        return true;
    }
    catch (...)
    {
        return false;
    }
}

void depthStencilCleared(ID3D11DeviceContext* context, ID3D11DepthStencilView* view, UINT flags, float depth, UINT8 stencil) noexcept
{
    try
    {
        if (!renderSized(textureDesc(view))) return;

        if (const auto output = currentTargets()) context->ClearDepthStencilView(output->depth.Get(), flags, depth, stencil);
    }
    catch (...)
    {
    }
}

bool drawGamma(ID3D11DeviceContext* context, const DrawCall& call) noexcept
{
    try
    {
        ComPtr<ID3D11ShaderResourceView> input;
        context->PSGetShaderResources(0, 1, input.GetAddressOf());
        if (!sceneTarget(textureDesc(input.Get()))) return false;

        const auto output = currentTargets();
        if (!output) return false;

        context->PSSetShaderResources(0, 1, output->srgbView.GetAddressOf());
        issue(context, call);
        context->PSSetShaderResources(0, 1, input.GetAddressOf());
        rememberScene(input.Get());

        return true;
    }
    catch (...)
    {
        return false;
    }
}

OutputSizeDraw::OutputSizeDraw(ID3D11DeviceContext* drawing, Surface target, std::initializer_list<Input> inputs) noexcept
{
    try
    {
        const auto output = currentTargets();
        if (!output || inputs.size() > MaxInputs) return;

        ID3D11RenderTargetView* bound[std::size(targets)]{};
        drawing->OMGetRenderTargets(static_cast<UINT>(std::size(bound)), bound, depth.GetAddressOf());
        for (std::size_t i = 0; i < std::size(bound); ++i) targets[i].Attach(bound[i]);

        const auto gameTarget = textureDesc(targets[0].Get());
        if (!gameTarget.Width || !gameTarget.Height) return;

        viewportCount = rectCount = static_cast<UINT>(std::size(viewports));
        drawing->RSGetViewports(&viewportCount, viewports);
        drawing->RSGetScissorRects(&rectCount, rects);
        drawing->PSGetConstantBuffers(0, 1, system.GetAddressOf());
        for (const auto& input : inputs)
        {
            inputSlots[inputCount] = input.slot;
            drawing->PSGetShaderResources(input.slot, 1, inputViews[inputCount].GetAddressOf());
            ++inputCount;
        }

        const auto& surface = (*output)[target];
        const double scaleX = static_cast<double>(surface.width) / gameTarget.Width, scaleY = static_cast<double>(surface.height) / gameTarget.Height;
        D3D11_VIEWPORT scaledViewports[std::size(viewports)]{};
        D3D11_RECT scaledRects[std::size(rects)]{};
        scale(viewports, scaledViewports, viewportCount, scaleX, scaleY);
        scale(rects, scaledRects, rectCount, scaleX, scaleY);
        if (target == Surface::Scene) rememberScene(targets[0].Get());

        context = drawing;
        const bool isBlur = target == Surface::Blur || target == Surface::BlurTwin;
        if (!isBlur && copySystem(drawing, *output, system.Get())) drawing->PSSetConstantBuffers(0, 1, output->postSystemConstants.GetAddressOf());
        for (const auto& input : inputs) drawing->PSSetShaderResources(input.slot, 1, (*output)[input.surface].view.GetAddressOf());
        drawing->OMSetRenderTargetsAndUnorderedAccessViews(1, (*output)[target].target.GetAddressOf(), nullptr, 0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS,
            nullptr, nullptr);
        drawing->RSSetViewports(viewportCount, scaledViewports);
        drawing->RSSetScissorRects(rectCount, scaledRects);
    }
    catch (...)
    {
    }
}

OutputSizeDraw::~OutputSizeDraw()
{
    if (!context) return;

    ID3D11RenderTargetView* bound[std::size(targets)]{};
    for (std::size_t i = 0; i < std::size(targets); ++i) bound[i] = targets[i].Get();
    context->OMSetRenderTargetsAndUnorderedAccessViews(static_cast<UINT>(std::size(bound)), bound, depth.Get(), 0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS,
        nullptr, nullptr);
    context->RSSetViewports(viewportCount, viewports);
    context->RSSetScissorRects(rectCount, rects);
    context->PSSetConstantBuffers(0, 1, system.GetAddressOf());
    for (std::size_t i = 0; i < inputCount; ++i) context->PSSetShaderResources(inputSlots[i], 1, inputViews[i].GetAddressOf());
}

void copySurface(ID3D11DeviceContext* context, Surface to, Surface from) noexcept
{
    if (const auto output = currentTargets()) context->CopyResource((*output)[to].texture.Get(), (*output)[from].texture.Get());
}

SurfaceViews surfaceViews(Surface surface) noexcept
{
    const auto output = currentTargets();
    if (!output) return {};

    const auto& texture = (*output)[surface];

    return {texture.target, texture.view, texture.width, texture.height};
}

bool isUpscaling() noexcept
{
    return belowOutput(render_scale::current());
}

bool renderSized(ID3D11View* view) noexcept
{
    try
    {
        return renderSized(textureDesc(view));
    }
    catch (...)
    {
        return false;
    }
}

Output output() noexcept
{
    const auto current = currentTargets();
    std::lock_guard lock(mutex);
    if (!current) return {};

    const auto& target = (*current)[Surface::Scene];

    return {target.target, target.view, sceneView, current->width, current->height};
}
}
