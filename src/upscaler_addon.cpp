// ReShade add-on (MGSVDLSS.addon64): NVIDIA DLSS in place of the game's FXAA (upscaler.hpp), as DLAA or
// upscaling from a lower render size (render_scale.hpp, ui_redirect.hpp). It lets the game start
// under ReShade (device_check.hpp), trades the game's motion blur for its velocity passes
// (motion_blur.hpp), keeps the FXAA pass that DLSS replaces with post processing down (fxaa.hpp),
// can lift its 60 fps cap (frame_rate.hpp) and has an optional pipeline recorder for development
// (frame_log.hpp).
// NGX receives ReShade's wrapped context, so other add-ons that follow DLSS evaluations see it like
// a game's own DLSS.
#include "device_check.hpp"
#include "frame_log.hpp"
#include "frame_rate.hpp"
#include "fxaa.hpp"
#include "log.hpp"
#include "motion_blur.hpp"
#include "render_scale.hpp"
#include "upscaler.hpp"

#include <Windows.h>
#include <Psapi.h>
#include <MinHook.h>
#include <d3d11.h>
// Before reshade.hpp, so register_addon also connects the ImGui calls of the overlay to ReShade's.
#include <imgui.h>
#include <reshade.hpp>
#include <array>
#include <filesystem>
#include <string>

extern "C" __declspec(dllexport) const char* NAME = "MGSV DLSS";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "NVIDIA DLSS (DLAA and upscaling) in place of the FXAA of Metal Gear Solid V: The Phantom Pain (Steam version 1.0.15.4).";

namespace
{
using namespace mgsv_dlss;
using namespace reshade::api;

// ReShade's proxies return their native object for this interface (IID_UnwrappedObject).
constexpr GUID UnwrappedObject{0x7f2c9a11, 0x3b4e, 0x4d6a, {0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42}};

HMODULE self{};
command_list* immediateList{};

ID3D11DeviceContext* native(command_list* list)
{
    return reinterpret_cast<ID3D11DeviceContext*>(list->get_native());
}

// The settings, log and frame records live beside the add-on, as ReShade keeps its own.
std::filesystem::path dataDirectory()
{
    std::wstring path(MAX_PATH, L'\0');
    for (;;)
    {
        const auto length = GetModuleFileNameW(self, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) return std::filesystem::current_path();
        if (length < path.size()) return std::filesystem::path(path.substr(0, length)).parent_path();

        path.resize(path.size() * 2);
    }
}

void logGameBuild()
{
    const auto* base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || nt->Signature != IMAGE_NT_SIGNATURE)
    {
        log("The executable has no PE header.");
        return;
    }

    log("Game executable: TimeDateStamp " + hex(nt->FileHeader.TimeDateStamp) + ", SizeOfImage " + hex(nt->OptionalHeader.SizeOfImage)
        + (supportedBuild() ? " (supported)." : " (not supported)."));
}

// The command queue ReShade reports is a base inside its D3D11DeviceContext proxy; find the proxy
// itself and prove it by asking for the native context it wraps.
ID3D11DeviceContext* wrapperOf(command_queue* queue, ID3D11DeviceContext* context)
{
    const auto address = reinterpret_cast<std::uintptr_t>(queue);
    HMODULE reshade{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            *reinterpret_cast<LPCWSTR*>(address), &reshade))
        return nullptr;

    MODULEINFO info{};
    if (!GetModuleInformation(GetCurrentProcess(), reshade, &info, sizeof(info))) return nullptr;

    const auto begin = reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll), end = begin + info.SizeOfImage;
    for (std::uintptr_t offset = 8; offset <= 32; offset += 8)
    {
        const auto candidate = address - offset;
        const auto vtable = *reinterpret_cast<const std::uintptr_t*>(candidate);
        if (vtable < begin || vtable >= end) continue;

        IUnknown* unwrapped{};
        if (FAILED(reinterpret_cast<IUnknown*>(candidate)->QueryInterface(UnwrappedObject, reinterpret_cast<void**>(&unwrapped)))) continue;

        const bool match = unwrapped == context;
        unwrapped->Release();
        if (match) return reinterpret_cast<ID3D11DeviceContext*>(candidate);
    }

    return nullptr;
}

void onInitCommandQueue(command_queue* queue)
{
    auto* context = reinterpret_cast<ID3D11DeviceContext*>(queue->get_native());
    if (queue->get_device()->get_api() != device_api::d3d11 || !context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE || immediateList)
        return;

    immediateList = queue->get_immediate_command_list();
    auto* wrapper = wrapperOf(queue, context);
    upscaler::attach(reinterpret_cast<ID3D11Device*>(queue->get_device()->get_native()), context, wrapper);
    log("Immediate context " + hex(reinterpret_cast<std::uintptr_t>(context)) + ", ReShade wrapper "
        + hex(reinterpret_cast<std::uintptr_t>(wrapper)) + ".");
}

void onDestroyCommandQueue(command_queue* queue)
{
    if (queue->get_immediate_command_list() != immediateList) return;

    upscaler::detach();
    immediateList = nullptr;
}

void onInitPipeline(device* device, pipeline_layout, uint32_t count, const pipeline_subobject* subobjects, pipeline pipeline)
{
    if (device->get_api() != device_api::d3d11) return;

    for (uint32_t i = 0; i < count; ++i)
    {
        const auto* shader = static_cast<const shader_desc*>(subobjects[i].data);
        if (!shader) continue;

        if (subobjects[i].type == pipeline_subobject_type::pixel_shader)
            upscaler::shaderCreated(shader->code, shader->code_size, reinterpret_cast<IUnknown*>(pipeline.handle));
        else if (subobjects[i].type == pipeline_subobject_type::vertex_shader)
            ui_redirect::vertexShaderCreated(shader->code, shader->code_size, reinterpret_cast<IUnknown*>(pipeline.handle));
    }
}

bool onDraw(command_list* list, uint32_t vertices, uint32_t instances, uint32_t first, uint32_t firstInstance)
{
    return upscaler::draw(native(list), {vertices, instances, first, firstInstance, 0, false});
}

bool onDrawIndexed(command_list* list, uint32_t indices, uint32_t instances, uint32_t first, int32_t offset, uint32_t firstInstance)
{
    return upscaler::draw(native(list), {indices, instances, first, firstInstance, offset, true});
}

bool onClearRenderTarget(command_list* list, resource_view view, const float color[4], uint32_t rects, const rect*)
{
    return !rects && upscaler::clearTarget(native(list), reinterpret_cast<ID3D11RenderTargetView*>(view.handle), color);
}

bool onClearDepthStencil(command_list* list, resource_view view, const float* depth, const uint8_t* stencil, uint32_t rects, const rect*)
{
    const UINT flags = (depth ? D3D11_CLEAR_DEPTH : 0) | (stencil ? D3D11_CLEAR_STENCIL : 0);
    if (!rects)
        upscaler::depthStencilCleared(native(list), reinterpret_cast<ID3D11DepthStencilView*>(view.handle), flags, depth ? *depth : 0,
            stencil ? *stencil : 0);

    return false;
}

void onBindPipeline(command_list* list, pipeline_stage stages, pipeline pipeline)
{
    if ((stages & pipeline_stage::vertex_shader) == pipeline_stage::vertex_shader)
        ui_redirect::vertexShaderBound(native(list), reinterpret_cast<IUnknown*>(pipeline.handle));
}

// ReShade reports bound samplers after binding them; rebind biased copies on the native context.
void onPushDescriptors(command_list* list, shader_stage stages, pipeline_layout, uint32_t, const descriptor_table_update& update)
{
    if (update.type != descriptor_type::sampler || (stages & shader_stage::pixel) != shader_stage::pixel
        || update.count > D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT)
        return;

    std::array<ID3D11SamplerState*, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT> biased{};
    const auto* samplers = static_cast<ID3D11SamplerState* const*>(update.descriptors);
    if (upscaler::biasSamplers(native(list), update.count, samplers, biased.data()))
        native(list)->PSSetSamplers(update.binding, update.count, biased.data());
}

void onInitSwapchain(swapchain* chain, bool)
{
    const auto desc = chain->get_device()->get_resource_desc(chain->get_current_back_buffer());
    upscaler::setOutputSize(desc.texture.width, desc.texture.height);
}

void onMapBuffer(device*, resource buffer, uint64_t, uint64_t, map_access access, void** data)
{
    if (access != map_access::read_only && data) upscaler::bufferMapped(reinterpret_cast<ID3D11Resource*>(buffer.handle), *data);
}

void onUnmapBuffer(device*, resource buffer)
{
    upscaler::bufferUnmapped(reinterpret_cast<ID3D11Resource*>(buffer.handle));
}

// Called before the immediate context executes a command list, and after FinishCommandList with the
// new command list and the deferred context it came from.
void onExecuteSecondary(command_list* list, command_list* secondary)
{
    if (list == immediateList)
    {
        upscaler::executing(native(list), secondary->get_native());
        return;
    }

    upscaler::recorded(list->get_native(), secondary->get_native());
    ui_redirect::contextFinished(native(secondary));
}

void onDestroyCommandList(command_list* list)
{
    upscaler::forget(list->get_native());
}

void onPresent(command_queue* queue, swapchain*, const rect*, const rect*, uint32_t, const rect*)
{
    if (queue->get_immediate_command_list() == immediateList) upscaler::present();
}

void onOverlay(effect_runtime*)
{
    upscaler::drawMenu();
}
}

extern "C" __declspec(dllexport) bool AddonInit(HMODULE addon, HMODULE reshadeModule)
{
    if (!reshade::register_addon(addon, reshadeModule)) return false;

    self = addon;
    const auto directory = dataDirectory();
    openLog(directory / "MGSVDLSS.log");
    log("MGSV DLSS " MGSV_DLSS_VERSION ".");
    logGameBuild();
    log(passDeviceCheck());

    // The engine hooks share one MinHook instance; a hook that cannot be made logs MinHook's reason.
    MH_Initialize();
    log(motion_blur::hook());
    log(fxaa::hook());
    log(frame_rate::hook());
    log(render_scale::hook());

    frame_log::install(directory);
    upscaler::initialize(directory, supportedBuild());

    reshade::register_event<reshade::addon_event::init_command_queue>(onInitCommandQueue);
    reshade::register_event<reshade::addon_event::destroy_command_queue>(onDestroyCommandQueue);
    reshade::register_event<reshade::addon_event::init_pipeline>(onInitPipeline);
    reshade::register_event<reshade::addon_event::draw>(onDraw);
    reshade::register_event<reshade::addon_event::draw_indexed>(onDrawIndexed);
    reshade::register_event<reshade::addon_event::clear_render_target_view>(onClearRenderTarget);
    reshade::register_event<reshade::addon_event::clear_depth_stencil_view>(onClearDepthStencil);
    reshade::register_event<reshade::addon_event::push_descriptors>(onPushDescriptors);
    reshade::register_event<reshade::addon_event::bind_pipeline>(onBindPipeline);
    reshade::register_event<reshade::addon_event::init_swapchain>(onInitSwapchain);
    reshade::register_event<reshade::addon_event::map_buffer_region>(onMapBuffer);
    reshade::register_event<reshade::addon_event::unmap_buffer_region>(onUnmapBuffer);
    reshade::register_event<reshade::addon_event::execute_secondary_command_list>(onExecuteSecondary);
    reshade::register_event<reshade::addon_event::destroy_command_list>(onDestroyCommandList);
    reshade::register_event<reshade::addon_event::present>(onPresent);
    reshade::register_overlay("MGSV DLSS", onOverlay);

    return true;
}

extern "C" __declspec(dllexport) void AddonUninit(HMODULE addon, HMODULE reshadeModule)
{
    // ReShade unloads add-ons when its last device goes; the next load reports the game's device again.
    upscaler::detach();
    // Removes every engine hook, so the game keeps running without the add-on.
    MH_Uninitialize();
    immediateList = nullptr;
    reshade::unregister_overlay("MGSV DLSS", onOverlay);
    frame_log::uninstall();
    reshade::unregister_addon(addon, reshadeModule);
}
