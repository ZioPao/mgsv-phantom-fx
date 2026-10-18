#include "ui_redirect.hpp"

#include "log.hpp"
#include "render_scale.hpp"

#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace mgsv_dlss::ui_redirect
{
namespace
{
using Microsoft::WRL::ComPtr;

// The scene target is B8G8R8A8_TYPELESS with UNORM views to draw and an UNORM_SRGB view that
// GammaControl reads (docs/DEVELOPMENT.md, "Resources").
constexpr DXGI_FORMAT SceneFormat = DXGI_FORMAT_B8G8R8A8_TYPELESS;

struct Targets
{
    unsigned width{}, height{};
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11ShaderResourceView> srgbView;
    // The 2D pass masks with stencil; it does not test depth.
    ComPtr<ID3D11DepthStencilView> depth;
    // cPSSystem (slot b0) for the output size. The 2D pass's pixel shaders read only m_renderInfo (c1)
    // and m_renderBuffer (c2) from it, to place screen-space effects.
    ComPtr<ID3D11Buffer> systemConstants;
};

// Whether each recording context has a 2D pass vertex shader bound, by the context ReShade reports
// with each binding.
struct BoundShader
{
    std::atomic<ID3D11DeviceContext*> context{};
    std::atomic<bool> draw2D{};
};

std::atomic<unsigned> outputWidth{}, outputHeight{};

// The 2D pass's vertex shaders; holding them keeps their addresses from being reused.
std::shared_mutex shaderMutex;
std::unordered_map<IUnknown*, ComPtr<IUnknown>> draw2DShaders;
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

D3D11_TEXTURE2D_DESC textureDesc(ID3D11View* view)
{
    D3D11_TEXTURE2D_DESC desc{};
    if (!view) return desc;

    ComPtr<ID3D11Resource> resource;
    view->GetResource(resource.GetAddressOf());
    ComPtr<ID3D11Texture2D> texture;
    if (SUCCEEDED(resource.As(&texture))) texture->GetDesc(&desc);

    return desc;
}

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
    return texture.Format == SceneFormat && renderSized(texture);
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
    desc.Format = SceneFormat;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> color, depth;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, color.GetAddressOf()))) return nullptr;

    const float system[16]{0, 0, 0, 0, static_cast<float>(width), static_cast<float>(height), 0, 0, static_cast<float>(width),
        static_cast<float>(height), 1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height)};
    const D3D11_BUFFER_DESC systemDesc{sizeof(system), D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER};
    const D3D11_SUBRESOURCE_DATA systemData{system};
    if (FAILED(device->CreateBuffer(&systemDesc, &systemData, created->systemConstants.GetAddressOf()))) return nullptr;

    D3D11_RENDER_TARGET_VIEW_DESC targetDesc{DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_RTV_DIMENSION_TEXTURE2D};
    D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, D3D11_SRV_DIMENSION_TEXTURE2D};
    viewDesc.Texture2D.MipLevels = 1;
    desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(device->CreateRenderTargetView(color.Get(), &targetDesc, created->target.GetAddressOf()))
        || FAILED(device->CreateShaderResourceView(color.Get(), &viewDesc, created->srgbView.GetAddressOf()))
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
    D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_SRV_DIMENSION_TEXTURE2D};
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

// The 2D pass's vertex shaders are generated as ...\Draw2D*_vs.hlsl, and the game's shaders keep that
// path in their debug data. Other shaders mention Draw2D only through shared headers.
bool draw2DSource(std::string_view code)
{
    constexpr std::string_view Prefix = "\\Draw2D", Suffix = "_vs.hlsl";
    for (auto at = code.find(Prefix); at != code.npos; at = code.find(Prefix, at + 1))
    {
        auto end = at + Prefix.size();
        while (end < code.size() && (std::isalnum(static_cast<unsigned char>(code[end])) || code[end] == '_')) ++end;
        // The name ends in _vs, followed by .hlsl.
        if (code.substr(end - 3, Suffix.size()) == Suffix) return true;
    }

    return false;
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

void vertexShaderCreated(const void* code, std::size_t size, IUnknown* shader) noexcept
{
    if (!code || !shader || !draw2DSource(std::string_view(static_cast<const char*>(code), size))) return;

    try
    {
        std::unique_lock lock(shaderMutex);
        draw2DShaders.try_emplace(shader, shader);
    }
    catch (...)
    {
    }
}

void vertexShaderBound(ID3D11DeviceContext* context, IUnknown* shader) noexcept
{
    bool draw2D{};
    if (shader)
    {
        std::shared_lock lock(shaderMutex);
        draw2D = draw2DShaders.contains(shader);
    }

    if (auto* slot = slotOf(context, draw2D)) slot->draw2D.store(draw2D, std::memory_order_relaxed);
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

    std::unique_lock lock(shaderMutex);
    draw2DShaders.clear();
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
        for (UINT i = 0; i < viewportCount; ++i)
        {
            scaledViewports[i] = viewports[i];
            scaledViewports[i].TopLeftX = static_cast<float>(viewports[i].TopLeftX * scaleX);
            scaledViewports[i].TopLeftY = static_cast<float>(viewports[i].TopLeftY * scaleY);
            scaledViewports[i].Width = static_cast<float>(viewports[i].Width * scaleX);
            scaledViewports[i].Height = static_cast<float>(viewports[i].Height * scaleY);
        }

        for (UINT i = 0; i < rectCount; ++i)
            scaledRects[i] = {scaled(rects[i].left, scaleX), scaled(rects[i].top, scaleY), scaled(rects[i].right, scaleX),
                scaled(rects[i].bottom, scaleY)};

        ComPtr<ID3D11Buffer> system;
        context->PSGetConstantBuffers(0, 1, system.GetAddressOf());
        context->OMSetRenderTargetsAndUnorderedAccessViews(1, output->target.GetAddressOf(), depth ? output->depth.Get() : nullptr, 0,
            D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
        context->RSSetViewports(viewportCount, scaledViewports);
        context->RSSetScissorRects(rectCount, scaledRects);
        context->PSSetConstantBuffers(0, 1, output->systemConstants.GetAddressOf());
        issue(context, call);

        context->OMSetRenderTargetsAndUnorderedAccessViews(1, bound.data(), depth.Get(), 0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
        context->RSSetViewports(viewportCount, viewports);
        context->RSSetScissorRects(rectCount, rects);
        context->PSSetConstantBuffers(0, 1, system.GetAddressOf());
        rememberScene(views[0].Get());

        if (!loggedDraw.exchange(true))
        {
            std::shared_lock lock(shaderMutex);
            log("The 2D pass draws at " + std::to_string(output->width) + "x" + std::to_string(output->height) + " while the engine renders at "
                + std::to_string(target.Width) + "x" + std::to_string(target.Height) + "; " + std::to_string(draw2DShaders.size())
                + " of its vertex shaders are known.");
        }

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

        context->ClearRenderTargetView(output->target.Get(), color);
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

Output output() noexcept
{
    const auto current = currentTargets();
    std::lock_guard lock(mutex);
    if (!current) return {};

    return {current->target, sceneView, current->width, current->height};
}
}
