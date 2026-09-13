#include "rive_texture_renderer.hpp"

#include <future>
#include <stdexcept>
#include <windows.h>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_sampler_state.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <rive/artboard.hpp>
#include <rive/renderer/rive_renderer.hpp>
#include <rive/renderer/vulkan/render_context_vulkan_impl.hpp>
#include <rive/renderer/vulkan/render_target_vulkan.hpp>

using namespace godot;
using namespace rive::gpu;

namespace {
void execute_work(uint64_t address) {
    (*reinterpret_cast<std::packaged_task<void()>*>(address))();
}
void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(operation) + ": " + std::to_string(result));
}
constexpr vkutil::ImageAccess sampled = {
    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
    VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
};
}

void RiveTextureRenderer::on_render_thread(const std::function<void()>& work) {
    std::packaged_task<void()> task(work);
    auto done = task.get_future();
    RenderingServer::get_singleton()->call_on_render_thread(
        callable_mp_static(execute_work).bind(uint64_t(&task)));
    done.get();
}

struct RiveTextureRenderer::Impl {
    RenderingDevice* rd = nullptr;
    HMODULE loader = nullptr;
    std::unique_ptr<RenderContext> context;
    std::unique_ptr<rive::RiveRenderer> renderer;
    rive::rcp<RenderTargetVulkanImpl> target;
    rive::rcp<vkutil::Texture2D> image;
    Ref<Texture2DRD> texture;
    RID texture_rid;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    uint64_t frame = 0;
    int width = 0, height = 0;
    bool attempted = false;

    VulkanContext* vk() const {
        return context->static_impl_cast<RenderContextVulkanImpl>()->vulkanContext();
    }

    // Establish Godot's tracked layout before external rendering starts. Merely
    // importing a VkImage leaves the graph at UNDEFINED (discarding its contents
    // on first use). A one-pixel compute read followed by a 16-byte buffer read
    // submits that transition. No animation pixels are read back to the CPU.
    void prime_texture() {
        Ref<RDShaderSource> source;
        source.instantiate();
        source->set_stage_source(RenderingDevice::SHADER_STAGE_COMPUTE,
            "#version 450\nlayout(local_size_x=1) in;\n"
            "layout(set=0,binding=0) uniform sampler2D source_texture;\n"
            "layout(set=0,binding=1,std430) buffer Result { vec4 value; } result;\n"
            "void main(){ result.value=texelFetch(source_texture,ivec2(0),0); }\n");
        auto spirv = rd->shader_compile_spirv_from_source(source);
        if (!spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_COMPUTE).is_empty())
            throw std::runtime_error("Could not compile texture initialization shader");
        RID shader = rd->shader_create_from_spirv(spirv);
        RID pipeline = rd->compute_pipeline_create(shader);
        Ref<RDSamplerState> state;
        state.instantiate();
        RID sampler = rd->sampler_create(state);
        RID buffer = rd->storage_buffer_create(16);
        Ref<RDUniform> input, output;
        input.instantiate();
        input->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
        input->set_binding(0);
        input->add_id(sampler);
        input->add_id(texture_rid);
        output.instantiate();
        output->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
        output->set_binding(1);
        output->add_id(buffer);
        TypedArray<RDUniform> uniforms;
        uniforms.push_back(input);
        uniforms.push_back(output);
        RID set = rd->uniform_set_create(uniforms, shader, 0);
        if (!shader.is_valid() || !pipeline.is_valid() || !set.is_valid())
            throw std::runtime_error("Could not initialize shared Rive texture");
        auto list = rd->compute_list_begin();
        rd->compute_list_bind_compute_pipeline(list, pipeline);
        rd->compute_list_bind_uniform_set(list, set, 0);
        rd->compute_list_dispatch(list, 1, 1, 1);
        rd->compute_list_end();
        rd->buffer_get_data(buffer);
        rd->free_rid(set);
        rd->free_rid(pipeline);
        rd->free_rid(shader);
        rd->free_rid(sampler);
        rd->free_rid(buffer);
        target->updateLastAccess(sampled);
    }

    void release_target() {
        if (texture.is_valid()) texture->set_texture_rd_rid(RID());
        if (texture_rid.is_valid()) rd->free_rid(texture_rid);
        texture_rid = RID();
        target.reset();
        image.reset();
        width = height = 0;
    }

    void resize(int w, int h) {
        if (w == width && h == height) return;
        release_target();
        const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        VkImageCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.extent = {uint32_t(w), uint32_t(h), 1};
        info.mipLevels = info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        image = vk()->makeTexture2D(info, "Godot Rive output");
        if (!image) throw std::runtime_error("Could not allocate Rive output texture");
        target = context->static_impl_cast<RenderContextVulkanImpl>()->makeRenderTarget(w, h, info.format, usage);
        target->setTargetImageView(image->vkImageView(), image->vkImage(), {});
        texture_rid = rd->texture_create_from_extension(RenderingDevice::TEXTURE_TYPE_2D,
            RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM, RenderingDevice::TEXTURE_SAMPLES_1,
            RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT, uint64_t(image->vkImage()), w, h, 1, 1);
        if (!texture_rid.is_valid()) throw std::runtime_error("Could not import Rive output texture into Godot");
        prime_texture();
        width = w;
        height = h;
        if (texture.is_null()) texture.instantiate();
        texture->set_texture_rd_rid(texture_rid);
    }

    void initialize() {
        rd = RenderingServer::get_singleton()->get_rendering_device();
        if (!rd || RenderingServer::get_singleton()->get_current_rendering_driver_name() != "vulkan")
            throw std::runtime_error("Rive requires Godot's Vulkan driver (Forward+ or Mobile)");
        loader = LoadLibraryW(L"vulkan-1.dll");
        if (!loader) throw std::runtime_error("Vulkan loader not found");
        auto get_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
        if (!get_proc) throw std::runtime_error("Vulkan entry point not found");
        auto instance = reinterpret_cast<VkInstance>(rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TOPMOST_OBJECT, RID(), 0));
        auto device = reinterpret_cast<VkDevice>(rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_LOGICAL_DEVICE, RID(), 0));
        auto physical = reinterpret_cast<VkPhysicalDevice>(rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_PHYSICAL_DEVICE, RID(), 0));
        queue = reinterpret_cast<VkQueue>(rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_COMMAND_QUEUE, RID(), 0));
        auto family = uint32_t(rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_QUEUE_FAMILY, RID(), 0));
        auto get_features = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(get_proc(instance, "vkGetPhysicalDeviceFeatures"));
        VkPhysicalDeviceFeatures supported = {};
        get_features(physical, &supported);
        // Godot 4.5 enables these core features when supported. Do not assume
        // any optional extension (interlock, raster ordering) was enabled.
        VulkanFeatures features;
        features.independentBlend = supported.independentBlend;
        features.fragmentStoresAndAtomics = supported.fragmentStoresAndAtomics;
        features.shaderClipDistance = supported.shaderClipDistance;
        features.fillModeNonSolid = supported.fillModeNonSolid;
        context = RenderContextVulkanImpl::MakeContext(instance, physical, device, features, get_proc);
        if (!context) throw std::runtime_error("Could not create Rive Vulkan context");
        VkCommandPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = family;
        check(vk()->CreateCommandPool(device, &pool_info, nullptr, &pool), "CreateCommandPool");
        VkCommandBufferAllocateInfo allocation = {};
        allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocation.commandPool = pool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        check(vk()->AllocateCommandBuffers(device, &allocation, &command), "AllocateCommandBuffers");
        renderer = std::make_unique<rive::RiveRenderer>(context.get());
    }

    void draw(rive::Artboard* artboard, const rive::Mat2D& transform, int w, int h) {
        // ponytail: serialize the shared queue; replace with frame fences and
        // buffered targets when profiling justifies overlapping GPU work.
        check(vk()->QueueWaitIdle(queue), "QueueWaitIdle");
        resize(w, h);
        check(vk()->ResetCommandBuffer(command, 0), "ResetCommandBuffer");
        VkCommandBufferBeginInfo begin = {};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vk()->BeginCommandBuffer(command, &begin), "BeginCommandBuffer");
        RenderContext::FrameDescriptor frame_desc;
        frame_desc.renderTargetWidth = w;
        frame_desc.renderTargetHeight = h;
        frame_desc.clearColor = 0;
        context->beginFrame(frame_desc);
        renderer->save();
        renderer->transform(transform);
        artboard->draw(renderer.get());
        renderer->restore();
        RenderContext::FlushResources resources;
        resources.renderTarget = target.get();
        resources.externalCommandBuffer = command;
        resources.safeFrameNumber = frame;
        resources.currentFrameNumber = ++frame;
        context->flush(resources);
        target->accessTargetImage(command, sampled);
        check(vk()->EndCommandBuffer(command), "EndCommandBuffer");
        VkSubmitInfo submit = {};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        check(vk()->QueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "QueueSubmit");
        check(vk()->QueueWaitIdle(queue), "QueueWaitIdle");
    }

    void shutdown() {
        if (context) {
            vk()->QueueWaitIdle(queue);
            release_target();
            renderer.reset();
            if (pool) vk()->DestroyCommandPool(vk()->device, pool, nullptr);
            context.reset();
        }
        if (loader) FreeLibrary(loader);
        loader = nullptr;
    }
};

RiveTextureRenderer::RiveTextureRenderer() : impl(std::make_unique<Impl>()) {}
RiveTextureRenderer::~RiveTextureRenderer() { on_render_thread([this] { impl->shutdown(); }); }
bool RiveTextureRenderer::initialize() {
    if (impl->attempted) return impl->renderer != nullptr;
    impl->attempted = true;
    try { on_render_thread([this] { impl->initialize(); }); }
    catch (const std::exception& error) { UtilityFunctions::push_error(error.what()); }
    return impl->renderer != nullptr;
}
rive::Factory* RiveTextureRenderer::factory() const { return impl->context.get(); }
Ref<Texture2DRD> RiveTextureRenderer::texture() const { return impl->texture; }
bool RiveTextureRenderer::draw(rive::Artboard* artboard, const rive::Mat2D& transform, int width, int height) {
    if (!artboard || !initialize()) return false;
    try { on_render_thread([&] { impl->draw(artboard, transform, width, height); }); }
    catch (const std::exception& error) { UtilityFunctions::push_error(error.what()); return false; }
    return true;
}
