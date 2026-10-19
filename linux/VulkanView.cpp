#include "VulkanView.hpp"
#include "Transport.hpp"
#include <atomic>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include "pyrowave.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <iostream>
#include <time.h>
#include "../vendor/stb/stb_easy_font.h"

static void vkcheck(VkResult r, const char *what) {
    if (r != VK_SUCCESS)
        throw std::runtime_error(std::string(what) + " (Vulkan " + std::to_string(r) + ")");
}

struct VulkanView::Impl {
    SDL_Window *window;
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkPhysicalDeviceProperties properties{};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    float priority = 1.f;
    std::vector<const char *> deviceExtensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    PFN_vkGetCalibratedTimestampsEXT calibratedTimestamps = nullptr;
    VkTimeDomainEXT hostTimeDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT;
    pyrowave_device codec = nullptr;
    pyrowave_decoder decoder = nullptr;
    int width = 0, height = 0, chroma = 1;
    bool imagePyro = true;
    VkBuffer videoBuffer = VK_NULL_HANDLE;
    VkDeviceMemory videoMemory = VK_NULL_HANDLE;
    void *videoPixels = nullptr;
    size_t videoCapacity = 0;
    std::array<VkDeviceSize, 3> videoOffsets{};
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapFormat = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D extent{};
    std::vector<VkImage> swapImages;
    std::vector<VkImageView> swapViews;
    std::vector<VkFramebuffer> framebuffers;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    std::vector<VkSemaphore> finished;
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet descriptor = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;

    struct Image {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        uint32_t w = 0, h = 0;
    };

    std::array<Image, 3> images;
    Image cursorImage;
    Image statsImage;
    VkBuffer statsBuffer = VK_NULL_HANDLE;
    VkDeviceMemory statsMemory = VK_NULL_HANDLE;
    void *statsPixels = nullptr;
    bool statsVisible = false, statsUploaded = false, statsDirty = false;
    std::string statsText;
    td::CursorState cursor;
    std::shared_ptr<const td::CursorImage> uploadedCursorImage;
    bool cursorUploaded = false, queryWasDecode = false;
    VkBuffer cursorBuffer = VK_NULL_HANDLE;
    VkDeviceMemory cursorMemory = VK_NULL_HANDLE;
    void *cursorPixels = nullptr;

    struct Constants {
        int32_t srgb = 0, visible = 0;
        float left = 0, top = 0, width = 1, height = 1;
        int32_t stats = 0;
        float statsWidth = 1, statsHeight = 1, statsLeft = 16, statsTop = 16;
    };

    static_assert(sizeof(Constants) == 44, "Display push constant layout mismatch");
    bool imagesUsed = false, dirty = true;
    std::atomic<uint64_t> requestedSize{0};
    std::atomic<bool> resizeRequested{false};
    VkQueryPool queries = VK_NULL_HANDLE;
    bool queryPending = false;
    uint32_t timestampBits = 0;
    VulkanView::Timings timing;
    VulkanView::Completion completed;
    bool completionReady = false;

    explicit Impl(SDL_Window *w) : window(w) {
        int initialW = 0, initialH = 0;
        SDL_GetWindowSizeInPixels(window, &initialW, &initialH);
        requestedSize = (uint64_t(uint32_t(initialW)) << 32) | uint32_t(initialH);

        uint32_t extCount = 0;
        auto exts = SDL_Vulkan_GetInstanceExtensions(&extCount);
        if (!exts)
            throw std::runtime_error(SDL_GetError());
        app.pApplicationName = "ThunderDisplay";
        app.apiVersion = VK_API_VERSION_1_3;
        instanceInfo.pApplicationInfo = &app;
        instanceInfo.enabledExtensionCount = extCount;
        instanceInfo.ppEnabledExtensionNames = exts;
        vkcheck(vkCreateInstance(&instanceInfo, nullptr, &instance), "vkCreateInstance");
        if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface))
            throw std::runtime_error(SDL_GetError());
        uint32_t count = 0;
        vkEnumeratePhysicalDevices(instance, &count, nullptr);
        std::vector<VkPhysicalDevice> gpus(count);
        vkEnumeratePhysicalDevices(instance, &count, gpus.data());
        for (auto gpu : gpus) {
            uint32_t n = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(gpu, &n, nullptr);
            std::vector<VkQueueFamilyProperties> qs(n);
            vkGetPhysicalDeviceQueueFamilyProperties(gpu, &n, qs.data());
            for (uint32_t i = 0; i < n; i++) {
                VkBool32 supported = false;
                vkGetPhysicalDeviceSurfaceSupportKHR(gpu, i, surface, &supported);
                if (supported &&
                    (qs[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
                        (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
                    physical = gpu;
                    family = i;
                    timestampBits = qs[i].timestampValidBits;
                    break;
                }
            }
            if (physical)
                break;
        }
        if (!physical)
            throw std::runtime_error("No GPU with Vulkan compute and display support");
        vkGetPhysicalDeviceProperties(physical, &properties);
        std::cerr << "GPU: " << properties.deviceName << '\n';
        features.pNext = &f11;
        f11.pNext = &f12;
        f12.pNext = &f13;
        vkGetPhysicalDeviceFeatures2(physical, &features);
        if (!f13.subgroupSizeControl || !f12.storageBuffer8BitAccess)
            throw std::runtime_error("GPU lacks PyroWave subgroup/8-bit buffer support");
        uint32_t extensionCount = 0;
        vkEnumerateDeviceExtensionProperties(physical, nullptr, &extensionCount, nullptr);
        std::vector<VkExtensionProperties> supportedExtensions(extensionCount);
        vkEnumerateDeviceExtensionProperties(physical, nullptr, &extensionCount,
                                             supportedExtensions.data());
        bool calibrate = false;
        for (auto &e : supportedExtensions)
            if (!strcmp(e.extensionName, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME))
                calibrate = true;
        auto domainsFunction = reinterpret_cast<PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT>(
            vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT"));
        if (calibrate && domainsFunction) {
            uint32_t count = 0;
            domainsFunction(physical, &count, nullptr);
            std::vector<VkTimeDomainEXT> domains(count);
            domainsFunction(physical, &count, domains.data());
            bool mono = false, raw = false, dev = false;
            for (auto d : domains) {
                mono |= d == VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT;
                raw |= d == VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_EXT;
                dev |= d == VK_TIME_DOMAIN_DEVICE_EXT;
            }
            calibrate = dev && (mono || raw);
            hostTimeDomain =
                mono ? VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT : VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_EXT;
        } else
            calibrate = false;
        if (calibrate)
            deviceExtensions.push_back(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
        queueInfo.queueFamilyIndex = family;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        deviceInfo.pNext = &features;
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = deviceExtensions.size();
        deviceInfo.ppEnabledExtensionNames = deviceExtensions.data();
        vkcheck(vkCreateDevice(physical, &deviceInfo, nullptr, &device), "vkCreateDevice");
        vkGetDeviceQueue(device, family, 0, &queue);
        if (calibrate)
            calibratedTimestamps = reinterpret_cast<PFN_vkGetCalibratedTimestampsEXT>(
                vkGetDeviceProcAddr(device, "vkGetCalibratedTimestampsEXT"));
        std::cerr << "Latency GPU clock: " << (calibratedTimestamps ? "calibrated" : "unavailable")
                  << '\n';
        pyrowave_device_create_queue_info q{queue, family, 0};
        pyrowave_device_create_info ci{};
        ci.GetInstanceProcAddr = vkGetInstanceProcAddr;
        ci.instance = instance;
        ci.physical_device = physical;
        ci.device = device;
        ci.instance_create_info = &instanceInfo;
        ci.device_create_info = &deviceInfo;
        ci.queue_info = &q;
        ci.queue_info_count = 1;
        if (pyrowave_create_device(&ci, &codec) != PYROWAVE_SUCCESS)
            throw std::runtime_error("PyroWave Vulkan initialization failed");
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = family;
        vkcheck(vkCreateCommandPool(device, &pi, nullptr, &pool), "command pool");
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        vkcheck(vkAllocateCommandBuffers(device, &ai, &command), "command buffer");
        if (timestampBits) {
            VkQueryPoolCreateInfo queryInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
            queryInfo.queryCount = 3;
            vkcheck(vkCreateQueryPool(device, &queryInfo, nullptr, &queries), "timestamp queries");
        }
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkcheck(vkCreateFence(device, &fi, nullptr, &fence), "fence");
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkcheck(vkCreateSemaphore(device, &si, nullptr, &acquired), "acquire semaphore");
        VkFormatProperties planeProperties;
        vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R8_UNORM, &planeProperties);
        if (!(planeProperties.optimalTilingFeatures &
              VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
            throw std::runtime_error("GPU cannot linearly sample decoded YUV planes");
        VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW =
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.maxLod = 0;
        vkcheck(vkCreateSampler(device, &samplerInfo, nullptr, &sampler), "YUV sampler");
        std::array<VkDescriptorSetLayoutBinding, 5> bindings{};
        for (unsigned i = 0; i < 5; i++) {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = bindings.size();
        li.pBindings = bindings.data();
        vkcheck(vkCreateDescriptorSetLayout(device, &li, nullptr, &descriptorLayout),
                "descriptor layout");
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.maxSets = 1;
        dpi.poolSizeCount = 1;
        dpi.pPoolSizes = &ps;
        vkcheck(vkCreateDescriptorPool(device, &dpi, nullptr, &descriptorPool), "descriptor pool");
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = descriptorPool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &descriptorLayout;
        vkcheck(vkAllocateDescriptorSets(device, &dai, &descriptor), "descriptor set");
        VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Constants)};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &descriptorLayout;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &range;
        vkcheck(vkCreatePipelineLayout(device, &pli, nullptr, &pipelineLayout), "pipeline layout");
        initCursorBuffer();
        createImage(cursorImage, 1, 1, VK_FORMAT_R8G8B8A8_UNORM);
        updateCursorDescriptor();
        initStatsBuffer();
        createImage(statsImage, 1, 1, VK_FORMAT_R8G8B8A8_UNORM);
        updateStatsDescriptor();
        createSwapchain();
        std::cerr << "Display path: direct YUV sampling to swapchain, linear filtering\n";
    }

    VkShaderModule shader(const char *name) {
        const char *base = SDL_GetBasePath();
        if (!base)
            throw std::runtime_error("Cannot locate application directory");
        std::filesystem::path path = std::filesystem::path(base) / name;
        if (!std::filesystem::exists(path))
            path = std::filesystem::path(base) / "../share/thunder-display" / name;
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file)
            throw std::runtime_error(std::string("Cannot open shader: ") + name);
        size_t bytes = size_t(file.tellg());
        if (!bytes || bytes % 4)
            throw std::runtime_error("Invalid SPIR-V shader");
        std::vector<uint32_t> spirv(bytes / 4);
        file.seekg(0);
        file.read(reinterpret_cast<char *>(spirv.data()), bytes);
        if (!file)
            throw std::runtime_error("Cannot read SPIR-V shader");
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = bytes;
        info.pCode = spirv.data();
        VkShaderModule module;
        vkcheck(vkCreateShaderModule(device, &info, nullptr, &module), "shader module");
        return module;
    }

    void createDisplayPipeline(VkFormat format) {
        if (pipeline && format == swapFormat)
            return;
        if (pipeline)
            vkDestroyPipeline(device, pipeline, nullptr);
        pipeline = VK_NULL_HANDLE;
        if (renderPass)
            vkDestroyRenderPass(device, renderPass, nullptr);
        renderPass = VK_NULL_HANDLE;
        VkAttachmentDescription attachment{};
        attachment.format = format;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color;
        std::array<VkSubpassDependency, 2> dependencies{};
        dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass = 0;
        dependencies[0].srcStageMask = dependencies[0].dstStageMask =
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].srcSubpass = 0;
        dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rp.attachmentCount = 1;
        rp.pAttachments = &attachment;
        rp.subpassCount = 1;
        rp.pSubpasses = &subpass;
        rp.dependencyCount = dependencies.size();
        rp.pDependencies = dependencies.data();
        vkcheck(vkCreateRenderPass(device, &rp, nullptr, &renderPass), "display render pass");
        std::array<VkShaderModule, 2> modules{shader("present.vert.spv"),
                                              shader("present.frag.spv")};
        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        for (unsigned i = 0; i < 2; i++) {
            stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
            stages[i].module = modules[i];
            stages[i].pName = "main";
        }
        VkPipelineVertexInputStateCreateInfo vertex{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo multisample{
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &blendAttachment;
        std::array<VkDynamicState, 2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT,
                                                    VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = dynamicStates.size();
        dynamic.pDynamicStates = dynamicStates.data();
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        info.stageCount = stages.size();
        info.pStages = stages.data();
        info.pVertexInputState = &vertex;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = pipelineLayout;
        info.renderPass = renderPass;
        info.subpass = 0;
        VkResult result =
            vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
        for (auto module : modules)
            vkDestroyShaderModule(device, module, nullptr);
        vkcheck(result, "direct YUV pipeline");
        swapFormat = format;
    }

    void destroySwapchainViews() {
        for (auto framebuffer : framebuffers)
            vkDestroyFramebuffer(device, framebuffer, nullptr);
        framebuffers.clear();
        for (auto view : swapViews)
            vkDestroyImageView(device, view, nullptr);
        swapViews.clear();
        for (auto semaphore : finished)
            vkDestroySemaphore(device, semaphore, nullptr);
        finished.clear();
    }

    void createSwapchain() {
        uint64_t size = requestedSize;
        int w = int(size >> 32), h = int(uint32_t(size));
        if (w <= 0 || h <= 0) {
            dirty = true;
            return;
        }
        vkDeviceWaitIdle(device);
        destroySwapchainViews();
        VkSurfaceCapabilitiesKHR caps;
        vkcheck(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps),
                "surface capabilities");
        if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
            throw std::runtime_error(
                "Surface does not support direct color attachment presentation");
        uint32_t n = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &n, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(n);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &n, formats.data());
        if (formats.empty())
            throw std::runtime_error("Surface offers no image formats");
        auto format = formats.front();
        for (auto f : formats)
            if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
                f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                format = f;
                break;
            }
        createDisplayPipeline(format.format);
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &n, nullptr);
        std::vector<VkPresentModeKHR> modes(n);
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &n, modes.data());
        VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
        for (auto m : modes)
            if (m == VK_PRESENT_MODE_MAILBOX_KHR)
                mode = m;
        std::cerr << "Present mode: " << (mode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" : "FIFO")
                  << "\n";
        extent = caps.currentExtent;
        if (extent.width == UINT32_MAX) {
            extent.width =
                std::clamp(uint32_t(w), caps.minImageExtent.width, caps.maxImageExtent.width);
            extent.height =
                std::clamp(uint32_t(h), caps.minImageExtent.height, caps.maxImageExtent.height);
        }
        uint32_t count = caps.minImageCount + 1;
        if (caps.maxImageCount)
            count = std::min(count, caps.maxImageCount);
        VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        ci.surface = surface;
        ci.minImageCount = count;
        ci.imageFormat = format.format;
        ci.imageColorSpace = format.colorSpace;
        ci.imageExtent = extent;
        ci.imageArrayLayers = 1;
        ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform = caps.currentTransform;
        ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        for (auto a :
             {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
              VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR})
            if (caps.supportedCompositeAlpha & a) {
                ci.compositeAlpha = a;
                break;
            }
        ci.presentMode = mode;
        ci.clipped = VK_TRUE;
        ci.oldSwapchain = swapchain;
        VkSwapchainKHR next;
        vkcheck(vkCreateSwapchainKHR(device, &ci, nullptr, &next), "swapchain");
        if (swapchain)
            vkDestroySwapchainKHR(device, swapchain, nullptr);
        swapchain = next;
        vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr);
        swapImages.resize(count);
        vkGetSwapchainImagesKHR(device, swapchain, &count, swapImages.data());
        finished.resize(count);
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (auto &s : finished)
            vkcheck(vkCreateSemaphore(device, &si, nullptr, &s), "present semaphore");
        std::cerr << "Swapchain pixels: " << extent.width << 'x' << extent.height
                  << " (SDL drawable " << w << 'x' << h << ")\n";
        swapViews.resize(count);
        framebuffers.resize(count);
        for (uint32_t i = 0; i < count; i++) {
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = swapImages[i];
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = swapFormat;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkcheck(vkCreateImageView(device, &vi, nullptr, &swapViews[i]), "swapchain image view");
            VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fb.renderPass = renderPass;
            fb.attachmentCount = 1;
            fb.pAttachments = &swapViews[i];
            fb.width = extent.width;
            fb.height = extent.height;
            fb.layers = 1;
            vkcheck(vkCreateFramebuffer(device, &fb, nullptr, &framebuffers[i]),
                    "swapchain framebuffer");
        }
        dirty = false;
    }

    void destroyImage(Image &i) {
        if (i.view)
            vkDestroyImageView(device, i.view, nullptr);
        if (i.image)
            vkDestroyImage(device, i.image, nullptr);
        if (i.memory)
            vkFreeMemory(device, i.memory, nullptr);
        i = {};
    }

    void createImage(Image &i, uint32_t w, uint32_t h, VkFormat format) {
        i.w = w;
        i.h = h;
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = format;
        ci.extent = {w, h, 1};
        ci.mipLevels = ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                   VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        vkcheck(vkCreateImage(device, &ci, nullptr, &i.image), "image");
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, i.image, &req);
        VkPhysicalDeviceMemoryProperties props;
        vkGetPhysicalDeviceMemoryProperties(physical, &props);
        uint32_t index = UINT32_MAX;
        for (uint32_t k = 0; k < props.memoryTypeCount; k++)
            if ((req.memoryTypeBits & (1 << k)) &&
                (props.memoryTypes[k].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                index = k;
                break;
            }
        if (index == UINT32_MAX)
            throw std::runtime_error("No GPU image memory");
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = index;
        vkcheck(vkAllocateMemory(device, &ai, nullptr, &i.memory), "image memory");
        vkcheck(vkBindImageMemory(device, i.image, i.memory, 0), "bind image");
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = i.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkcheck(vkCreateImageView(device, &vi, nullptr, &i.view), "image view");
    }

    void setupImages(int w, int h, int c, bool pyro = true) {
        vkDeviceWaitIdle(device);
        if (decoder)
            pyrowave_decoder_destroy(decoder);
        decoder = nullptr;
        for (auto &i : images)
            destroyImage(i);
        width = w;
        height = h;
        chroma = c;
        imagePyro = pyro;
        if (pyro) {
            pyrowave_decoder_create_info ci{
                codec, w, h, c ? PYROWAVE_CHROMA_SUBSAMPLING_444 : PYROWAVE_CHROMA_SUBSAMPLING_420,
                false};
            if (pyrowave_decoder_create(&ci, &decoder) != PYROWAVE_SUCCESS)
                throw std::runtime_error("Cannot create PyroWave decoder for this GPU");
        }
        for (int i = 0; i < 3; i++)
            createImage(images[i], (i && c == 0) ? w / 2 : w, (i && c == 0) ? h / 2 : h,
                        VK_FORMAT_R8_UNORM);
        std::array<VkDescriptorImageInfo, 3> infos{};
        std::array<VkWriteDescriptorSet, 3> writes{};
        for (unsigned i = 0; i < 3; i++) {
            infos[i].sampler = sampler;
            infos[i].imageView = images[i].view;
            infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = descriptor;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo = &infos[i];
        }
        vkUpdateDescriptorSets(device, writes.size(), writes.data(), 0, nullptr);
        imagesUsed = false;
    }

    void destroyVideoBuffer() {
        if (videoPixels)
            vkUnmapMemory(device, videoMemory);
        if (videoBuffer)
            vkDestroyBuffer(device, videoBuffer, nullptr);
        if (videoMemory)
            vkFreeMemory(device, videoMemory, nullptr);
        videoPixels = nullptr;
        videoBuffer = VK_NULL_HANDLE;
        videoMemory = VK_NULL_HANDLE;
        videoCapacity = 0;
    }

    void uploadYuv(const YuvFrame &frame) {
        size_t bytes = 0;
        for (int p = 0; p < 3; p++) {
            bytes = (bytes + 3) & ~size_t(3);
            videoOffsets[p] = bytes;
            bytes += frame.planes[p].size();
        }
        if (videoCapacity < bytes) {
            destroyVideoBuffer();
            VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bi.size = bytes;
            bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            vkcheck(vkCreateBuffer(device, &bi, nullptr, &videoBuffer), "H264 upload buffer");
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(device, videoBuffer, &req);
            VkPhysicalDeviceMemoryProperties props;
            vkGetPhysicalDeviceMemoryProperties(physical, &props);
            uint32_t index = UINT32_MAX;
            for (uint32_t i = 0; i < props.memoryTypeCount; i++)
                if ((req.memoryTypeBits & (1u << i)) &&
                    (props.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                        (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                    index = i;
                    break;
                }
            if (index == UINT32_MAX)
                throw std::runtime_error("No H264 upload memory");
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = index;
            vkcheck(vkAllocateMemory(device, &ai, nullptr, &videoMemory), "H264 upload memory");
            vkcheck(vkBindBufferMemory(device, videoBuffer, videoMemory, 0), "H264 bind upload");
            vkcheck(vkMapMemory(device, videoMemory, 0, VK_WHOLE_SIZE, 0, &videoPixels),
                    "H264 map upload");
            videoCapacity = bytes;
        }
        for (int p = 0; p < 3; p++)
            memcpy(static_cast<uint8_t *>(videoPixels) + videoOffsets[p], frame.planes[p].data(),
                   frame.planes[p].size());
    }

    void initCursorBuffer() {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = td::MaxCursorSize * td::MaxCursorSize * 4;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkcheck(vkCreateBuffer(device, &bi, nullptr, &cursorBuffer), "cursor staging buffer");
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, cursorBuffer, &req);
        VkPhysicalDeviceMemoryProperties props;
        vkGetPhysicalDeviceMemoryProperties(physical, &props);
        uint32_t index = UINT32_MAX;
        for (uint32_t i = 0; i < props.memoryTypeCount; i++)
            if ((req.memoryTypeBits & (1u << i)) &&
                (props.memoryTypes[i].propertyFlags &
                 (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                index = i;
                break;
            }
        if (index == UINT32_MAX)
            throw std::runtime_error("No coherent cursor upload memory");
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = index;
        vkcheck(vkAllocateMemory(device, &ai, nullptr, &cursorMemory), "cursor staging memory");
        vkcheck(vkBindBufferMemory(device, cursorBuffer, cursorMemory, 0), "bind cursor staging");
        vkcheck(vkMapMemory(device, cursorMemory, 0, VK_WHOLE_SIZE, 0, &cursorPixels),
                "map cursor pixels");
        memset(cursorPixels, 0, bi.size);
    }

    void updateCursorDescriptor() {
        VkDescriptorImageInfo image{sampler, cursorImage.view,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = descriptor;
        write.dstBinding = 3;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }

    void initStatsBuffer() {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = 1024 * 320 * 4;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        vkcheck(vkCreateBuffer(device, &bi, nullptr, &statsBuffer), "stats staging");
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, statsBuffer, &req);
        VkPhysicalDeviceMemoryProperties props;
        vkGetPhysicalDeviceMemoryProperties(physical, &props);
        uint32_t index = UINT32_MAX;
        for (uint32_t i = 0; i < props.memoryTypeCount; i++)
            if ((req.memoryTypeBits & (1u << i)) &&
                (props.memoryTypes[i].propertyFlags &
                 (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                index = i;
                break;
            }
        if (index == UINT32_MAX)
            throw std::runtime_error("No coherent stats upload memory");
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = index;
        vkcheck(vkAllocateMemory(device, &ai, nullptr, &statsMemory), "stats memory");
        vkcheck(vkBindBufferMemory(device, statsBuffer, statsMemory, 0), "bind stats");
        vkcheck(vkMapMemory(device, statsMemory, 0, VK_WHOLE_SIZE, 0, &statsPixels), "map stats");
        memset(statsPixels, 0, bi.size);
    }

    void updateStatsDescriptor() {
        VkDescriptorImageInfo image{sampler, statsImage.view,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = descriptor;
        write.dstBinding = 4;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }

    void prepareStats() {
        if (!statsDirty || !statsVisible)
            return;
        uint32_t w = std::clamp(stb_easy_font_width(statsText.data()) * 2 + 32, 64, 1024),
                 h = std::clamp(stb_easy_font_height(statsText.data()) * 2 + 32, 32, 320);
        if (w != statsImage.w || h != statsImage.h) {
            destroyImage(statsImage);
            createImage(statsImage, w, h, VK_FORMAT_R8G8B8A8_UNORM);
            statsUploaded = false;
            updateStatsDescriptor();
        }
        auto pixels = static_cast<uint8_t *>(statsPixels);
        for (size_t p = 0; p < size_t(w) * h; p++) {
            pixels[p * 4] = pixels[p * 4 + 1] = pixels[p * 4 + 2] = 0;
            pixels[p * 4 + 3] = 205;
        }

        struct Vertex {
            float x, y, z;
            uint8_t color[4];
        };

        std::vector<Vertex> vertices(statsText.size() * 48);
        int quads = stb_easy_font_print(8, 8, statsText.data(), nullptr, vertices.data(),
                                        vertices.size() * sizeof(Vertex));
        for (int q = 0; q < quads; q++) {
            auto v = vertices.data() + q * 4;
            int x0 = std::max(0, int(std::floor(v[0].x * 2))),
                x1 = std::min(int(w), int(std::ceil(v[2].x * 2))),
                y0 = std::max(0, int(std::floor(v[0].y * 2))),
                y1 = std::min(int(h), int(std::ceil(v[2].y * 2)));
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++) {
                    auto p = pixels + (size_t(y) * w + x) * 4;
                    p[0] = p[1] = p[2] = 245;
                    p[3] = 255;
                }
        }
    }

    void collectCompletion() {
        if (completionReady || !queryPending || vkGetFenceStatus(device, fence) != VK_SUCCESS)
            return;
        uint64_t ticks[3] = {};
        if (vkGetQueryPoolResults(device, queries, 0, 3, sizeof(ticks), ticks, 8,
                                  VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
            return;
        uint64_t mask = timestampBits == 64 ? UINT64_MAX : ((uint64_t(1) << timestampBits) - 1);
        double ms = properties.limits.timestampPeriod / 1e6;
        completed = {};
        completed.decoded = queryWasDecode;
        completed.decodeGPU = ((ticks[1] - ticks[0]) & mask) * ms;
        completed.presentGPU = ((ticks[2] - ticks[1]) & mask) * ms;
        if (calibratedTimestamps && queryWasDecode && statsVisible) {
            VkCalibratedTimestampInfoEXT infos[2] = {
                {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, nullptr,
                 VK_TIME_DOMAIN_DEVICE_EXT},
                {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, nullptr, hostTimeDomain}};
            uint64_t times[2] = {}, deviation = 0;
            if (calibratedTimestamps(device, 2, infos, times, &deviation) == VK_SUCCESS) {
                int64_t adjustment = 0;
                uint64_t hostError = 0;
                if (hostTimeDomain == VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_EXT) {
                    uint64_t before = td::nowNs();
                    timespec raw{};
                    clock_gettime(CLOCK_MONOTONIC_RAW, &raw);
                    uint64_t after = td::nowNs();
                    adjustment = int64_t(before + (after - before) / 2) -
                                 int64_t(uint64_t(raw.tv_sec) * 1000000000 + raw.tv_nsec);
                    hostError = (after - before) / 2;
                }
                double age =
                    double((times[0] - ticks[2]) & mask) * properties.limits.timestampPeriod;
                int64_t host = int64_t(times[1]) + adjustment;
                if (age >= 0 && age < 1000000000 && host > int64_t(age)) {
                    completed.hostNs = uint64_t(host - int64_t(age));
                    completed.errorNs = deviation + hostError +
                                        uint64_t(std::ceil(properties.limits.timestampPeriod));
                }
            }
        }
        queryPending = false;
        completionReady = true;
    }

    bool pollCompletion(VulkanView::Completion &result, bool pollGPU) {
        if (pollGPU)
            collectCompletion();
        if (!completionReady)
            return false;
        result = completed;
        completionReady = false;
        return true;
    }

    void diagnoseColor() {
        if (!std::getenv("THUNDERDISPLAY_COLOR_DIAG") || chroma != 1)
            return;
        if (std::remove("/tmp/thunder-display-color-probe.once") != 0)
            return;
        vkcheck(vkDeviceWaitIdle(device), "color probe GPU wait");
        const size_t pixels = size_t(width) * height;
        std::array<std::vector<uint8_t>, 3> planes;
        pyrowave_cpu_buffer buffer{};
        buffer.width = width;
        buffer.height = height;
        buffer.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV444P;
        for (int i = 0; i < 3; i++) {
            planes[i].resize(pixels);
            buffer.data[i] = planes[i].data();
            buffer.row_stride_in_bytes[i] = width;
            buffer.plane_size_in_bytes[i] = pixels;
        }
        auto result = pyrowave_decoder_decode_cpu_buffer_synchronous(decoder, &buffer);
        if (result != PYROWAVE_SUCCESS) {
            std::cerr << "[ColorProbe] decode result=" << result << '\n';
            return;
        }
        std::array<uint64_t, 256> uh{}, vh{};
        uint64_t neutral = 0;
        double uSum = 0, vSum = 0, greenRed = 0, greenBlue = 0;
        for (size_t i = 0; i < pixels; i++) {
            const int y = planes[0][i], u = planes[1][i], v = planes[2][i];
            uh[u]++;
            vh[v]++;
            if (std::abs(u - 128) > 3 || std::abs(v - 128) > 3 || y < 32 || y > 219)
                continue;
            neutral++;
            uSum += u;
            vSum += v;
            const double yy = (y - 16) * 255.0 / 219, uu = (u - 128) * 255.0 / 224,
                         vv = (v - 128) * 255.0 / 224;
            const double r = std::clamp(yy + 1.5748 * vv, 0.0, 255.0),
                         g = std::clamp(yy - 0.187324 * uu - 0.468124 * vv, 0.0, 255.0),
                         b = std::clamp(yy + 1.8556 * uu, 0.0, 255.0);
            greenRed += g - r;
            greenBlue += g - b;
        }
        std::cerr << "[ColorProbe] pixels=" << pixels << " U127=" << uh[127] << " U128=" << uh[128]
                  << " U129=" << uh[129] << " V127=" << vh[127] << " V128=" << vh[128]
                  << " V129=" << vh[129] << " neutral=" << neutral;
        if (neutral)
            std::cerr << " mean_uv=" << uSum / neutral << ',' << vSum / neutral
                      << " green_minus_red=" << greenRed / neutral
                      << " green_minus_blue=" << greenBlue / neutral;
        std::cerr << '\n';
        uint64_t highFrequency = 0;
        double hfU = 0, hfV = 0, hfGreenRed = 0, hfGreenBlue = 0;
        for (int y = 0; y < height - 1; y++)
            for (int x = 0; x < width - 1; x++) {
                const size_t p = size_t(y) * width + x;
                const int yy = planes[0][p], u = planes[1][p], v = planes[2][p];
                if ((std::abs(yy - int(planes[0][p + 1])) < 24 &&
                     std::abs(yy - int(planes[0][p + width])) < 24) ||
                    std::abs(u - 128) > 4 || std::abs(v - 128) > 4)
                    continue;
                highFrequency++;
                hfU += u;
                hfV += v;
                const double luma = (yy - 16) * 255.0 / 219, cb = (u - 128) * 255.0 / 224,
                             cr = (v - 128) * 255.0 / 224;
                const double r = std::clamp(luma + 1.5748 * cr, 0.0, 255.0),
                             g = std::clamp(luma - 0.187324 * cb - 0.468124 * cr, 0.0, 255.0),
                             b = std::clamp(luma + 1.8556 * cb, 0.0, 255.0);
                hfGreenRed += g - r;
                hfGreenBlue += g - b;
            }
        if (highFrequency)
            std::cerr << "[ColorProbeHF] pixels=" << highFrequency
                      << " mean_uv=" << hfU / highFrequency << ',' << hfV / highFrequency
                      << " green_minus_red=" << hfGreenRed / highFrequency
                      << " green_minus_blue=" << hfGreenBlue / highFrequency << '\n';
        std::ofstream raw("/tmp/thunder-display-color.yuv", std::ios::binary);
        for (auto &p : planes)
            raw.write(reinterpret_cast<const char *>(p.data()), p.size());
        std::vector<uint8_t> rgb(pixels * 3);
        for (size_t i = 0; i < pixels; i++) {
            const double y = (int(planes[0][i]) - 16) * 255.0 / 219,
                         u = (int(planes[1][i]) - 128) * 255.0 / 224,
                         v = (int(planes[2][i]) - 128) * 255.0 / 224;
            rgb[3 * i] = uint8_t(std::clamp(std::lround(y + 1.5748 * v), 0l, 255l));
            rgb[3 * i + 1] =
                uint8_t(std::clamp(std::lround(y - .187324 * u - .468124 * v), 0l, 255l));
            rgb[3 * i + 2] = uint8_t(std::clamp(std::lround(y + 1.8556 * u), 0l, 255l));
        }
        std::ofstream frame("/tmp/thunder-display-color.ppm", std::ios::binary);
        frame << "P6\n" << width << ' ' << height << "\n255\n";
        frame.write(reinterpret_cast<const char *>(rgb.data()), rgb.size());
        std::cerr << "[ColorProbe] saved decoded YUV/RGB " << width << 'x' << height << '\n';
    }

    void barrier(VkImage image, VkImageLayout old, VkImageLayout next, VkAccessFlags src,
                 VkAccessFlags dst, VkPipelineStageFlags source, VkPipelineStageFlags dest) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = old;
        b.newLayout = next;
        b.srcAccessMask = src;
        b.dstAccessMask = dst;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(command, source, dest, 0, 0, nullptr, 0, nullptr, 1, &b);
    }

    bool show(int w, int h, int c, const std::vector<std::vector<uint8_t>> &units,
              bool decode = true, const YuvFrame *upload = nullptr) {
        if (!decode && !imagesUsed)
            return false;
        timing = {};
        uint64_t began = td::nowNs();
        vkcheck(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "wait GPU");
        uint64_t waited = td::nowNs();
        timing.fence = (waited - began) / 1e6;
        collectCompletion();
        prepareStats();
        if (resizeRequested.exchange(false))
            dirty = true;
        if (decode) {
            if (w != width || h != height || c != chroma || imagePyro == bool(upload))
                setupImages(w, h, c, !upload);
            if (upload)
                uploadYuv(*upload);
            else {
                pyrowave_decoder_clear(decoder);
                for (auto &u : units)
                    if (!u.empty() && pyrowave_decoder_push_packet(decoder, u.data(), u.size()) !=
                                          PYROWAVE_SUCCESS)
                        return false;
                if (!pyrowave_decoder_decode_is_ready(decoder, false)) {
                    static int reports = 0;
                    if (reports++ < 8)
                        std::cerr << "Decode waiting: complete frame required\n";
                    return false;
                }
            }
        }
        bool uploadCursor = !cursorUploaded || uploadedCursorImage != cursor.image;
        bool uploadStats = !statsUploaded || statsDirty;
        if (uploadCursor) {
            uint32_t cw = cursor.image ? cursor.image->width : 1,
                     ch = cursor.image ? cursor.image->height : 1;
            if (cursorImage.w != cw || cursorImage.h != ch) {
                destroyImage(cursorImage);
                createImage(cursorImage, cw, ch, VK_FORMAT_R8G8B8A8_UNORM);
                cursorUploaded = false;
                updateCursorDescriptor();
            }
            if (cursor.image)
                memcpy(cursorPixels, cursor.image->rgba.data(), cursor.image->rgba.size());
            else
                memset(cursorPixels, 0, 4);
        }
        uint64_t fed = td::nowNs();
        timing.feed = (fed - waited) / 1e6;
        if (dirty)
            createSwapchain();
        if (dirty)
            return false;
        uint32_t index = 0;
        VkResult r =
            vkAcquireNextImageKHR(device, swapchain, 1000000, acquired, VK_NULL_HANDLE, &index);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) {
            dirty = true;
            return false;
        }
        if (r == VK_TIMEOUT || r == VK_NOT_READY) {
            static unsigned reports = 0;
            if (reports++ < 12)
                std::cerr << "Present skipped: no swapchain image within 1 ms\n";
            return false;
        }
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
            vkcheck(r, "acquire image");
        uint64_t acquiredAt = td::nowNs();
        timing.acquire = (acquiredAt - fed) / 1e6;
        vkcheck(vkResetCommandBuffer(command, 0), "reset command");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkcheck(vkBeginCommandBuffer(command, &begin), "begin command");
        if (queries) {
            vkCmdResetQueryPool(command, queries, 0, 3);
            vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 0);
        }

        if (uploadCursor) {
            barrier(cursorImage.image,
                    cursorUploaded ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                   : VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    cursorUploaded ? VK_ACCESS_SHADER_READ_BIT : 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                    cursorUploaded ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                   : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {cursorImage.w, cursorImage.h, 1};
            vkCmdCopyBufferToImage(command, cursorBuffer, cursorImage.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            barrier(cursorImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        }
        if (uploadStats) {
            barrier(statsImage.image,
                    statsUploaded ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                  : VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    statsUploaded ? VK_ACCESS_SHADER_READ_BIT : 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                    statsUploaded ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                  : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {statsImage.w, statsImage.h, 1};
            vkCmdCopyBufferToImage(command, statsBuffer, statsImage.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            barrier(statsImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        }
        if (decode && upload) {
            for (int i = 0; i < 3; i++) {
                barrier(images[i].image,
                        imagesUsed ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                   : VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        imagesUsed ? VK_ACCESS_SHADER_READ_BIT : 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                        imagesUsed ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                   : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT);
                VkBufferImageCopy copy{};
                copy.bufferOffset = videoOffsets[i];
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.imageExtent = {images[i].w, images[i].h, 1};
                vkCmdCopyBufferToImage(command, videoBuffer, images[i].image,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
                barrier(images[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            }
        }
        if (decode && !upload) {
            for (int i = 0; i < 3; i++)
                barrier(images[i].image,
                        imagesUsed ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                   : VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_GENERAL, imagesUsed ? VK_ACCESS_SHADER_READ_BIT : 0,
                        VK_ACCESS_SHADER_WRITE_BIT,
                        imagesUsed ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                   : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            pyrowave_gpu_buffers buffers{};
            for (int i = 0; i < 3; i++) {
                auto &v = buffers.planes[i];
                v.image = images[i].image;
                v.width = images[i].w;
                v.height = images[i].h;
                v.image_format = v.view_format = VK_FORMAT_R8_UNORM;
                v.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
                v.swizzle = VK_COMPONENT_SWIZZLE_IDENTITY;
                v.layout = VK_IMAGE_LAYOUT_GENERAL;
            }
            pyrowave_device_set_command_buffer(codec, command);
            auto decoded = pyrowave_decoder_decode_gpu_buffer(decoder, nullptr, nullptr, &buffers);
            pyrowave_device_set_command_buffer(codec, VK_NULL_HANDLE);
            if (decoded != PYROWAVE_SUCCESS)
                throw std::runtime_error("PyroWave GPU decode failed");
            for (int i = 0; i < 3; i++)
                barrier(images[i].image, VK_IMAGE_LAYOUT_GENERAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
                        VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        }
        if (queries)
            vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, 1);
        VkClearValue black{};
        black.color.float32[3] = 1;
        VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        pass.renderPass = renderPass;
        pass.framebuffer = framebuffers[index];
        pass.renderArea.extent = extent;
        pass.clearValueCount = 1;
        pass.pClearValues = &black;
        vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
        double fit = std::min(double(extent.width) / w, double(extent.height) / h);
        uint32_t dw = std::max(1u, uint32_t(w * fit)), dh = std::max(1u, uint32_t(h * fit));
        int32_t dx = (int32_t(extent.width) - int32_t(dw)) / 2,
                dy = (int32_t(extent.height) - int32_t(dh)) / 2;
        VkViewport viewport{float(dx), float(dy), float(dw), float(dh), 0, 1};
        VkRect2D scissor{{dx, dy}, {dw, dh}};
        vkCmdSetViewport(command, 0, 1, &viewport);
        vkCmdSetScissor(command, 0, 1, &scissor);
        Constants pc;
        pc.srgb = (swapFormat == VK_FORMAT_B8G8R8A8_SRGB || swapFormat == VK_FORMAT_R8G8B8A8_SRGB)
                      ? 1
                      : 0;
        pc.stats = statsVisible ? 1 : 0;
        pc.statsWidth = std::min(float(statsImage.w), float(dw > 32 ? dw - 32 : 1));
        pc.statsHeight = statsImage.h * pc.statsWidth / std::max(1u, statsImage.w);
        pc.statsLeft = dx + 16;
        pc.statsTop = dy + 16;
        if (cursor.visible && cursor.image) {
            pc.visible = 1;
            pc.left = (cursor.x * (w - 1) - cursor.image->hotX) / w;
            pc.top = (cursor.y * (h - 1) - cursor.image->hotY) / h;
            pc.width = float(cursor.image->width) / w;
            pc.height = float(cursor.image->height) / h;
        }
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1,
                                &descriptor, 0, nullptr);
        vkCmdPushConstants(command, pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc),
                           &pc);
        vkCmdDraw(command, 3, 1, 0, 0);
        vkCmdEndRenderPass(command);
        if (queries)
            vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, 2);
        vkcheck(vkEndCommandBuffer(command), "end command");
        vkcheck(vkResetFences(device, 1, &fence), "reset fence");
        VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &acquired;
        submit.pWaitDstStageMask = &wait;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &finished[index];
        vkcheck(vkQueueSubmit(queue, 1, &submit, fence), "queue submit");
        uint64_t submitted = td::nowNs();
        timing.record = (submitted - acquiredAt) / 1e6;
        queryPending = queries != VK_NULL_HANDLE;
        queryWasDecode = decode;
        if (uploadCursor) {
            cursorUploaded = true;
            uploadedCursorImage = cursor.image;
        }
        if (uploadStats) {
            statsUploaded = true;
            statsDirty = false;
        }
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &finished[index];
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain;
        present.pImageIndices = &index;
        r = vkQueuePresentKHR(queue, &present);
        if (r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_OUT_OF_DATE_KHR)
            dirty = true;
        else
            vkcheck(r, "present");
        timing.presentCPU = (td::nowNs() - submitted) / 1e6;
        imagesUsed = true;
        if (decode && !upload)
            diagnoseColor();
        return true;
    }

    ~Impl() {
        if (!device)
            return;
        vkDeviceWaitIdle(device);
        destroyVideoBuffer();
        if (queries)
            vkDestroyQueryPool(device, queries, nullptr);
        if (decoder)
            pyrowave_decoder_destroy(decoder);
        if (codec)
            pyrowave_device_destroy(codec);
        for (auto &i : images)
            destroyImage(i);
        destroyImage(cursorImage);
        destroyImage(statsImage);
        if (statsPixels)
            vkUnmapMemory(device, statsMemory);
        if (statsBuffer)
            vkDestroyBuffer(device, statsBuffer, nullptr);
        if (statsMemory)
            vkFreeMemory(device, statsMemory, nullptr);
        if (cursorPixels)
            vkUnmapMemory(device, cursorMemory);
        if (cursorBuffer)
            vkDestroyBuffer(device, cursorBuffer, nullptr);
        if (cursorMemory)
            vkFreeMemory(device, cursorMemory, nullptr);
        if (pipeline)
            vkDestroyPipeline(device, pipeline, nullptr);
        destroySwapchainViews();
        if (renderPass)
            vkDestroyRenderPass(device, renderPass, nullptr);
        if (sampler)
            vkDestroySampler(device, sampler, nullptr);
        if (pipelineLayout)
            vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        if (descriptorPool)
            vkDestroyDescriptorPool(device, descriptorPool, nullptr);
        if (descriptorLayout)
            vkDestroyDescriptorSetLayout(device, descriptorLayout, nullptr);
        if (acquired)
            vkDestroySemaphore(device, acquired, nullptr);
        if (fence)
            vkDestroyFence(device, fence, nullptr);
        if (pool)
            vkDestroyCommandPool(device, pool, nullptr);
        if (swapchain)
            vkDestroySwapchainKHR(device, swapchain, nullptr);
        vkDestroyDevice(device, nullptr);
        if (surface)
            vkDestroySurfaceKHR(instance, surface, nullptr);
        if (instance)
            vkDestroyInstance(instance, nullptr);
    }
};

VulkanView::VulkanView(SDL_Window *w) : impl(std::make_unique<Impl>(w)) {
}

VulkanView::~VulkanView() = default;

bool VulkanView::show(int w, int h, int c, const std::vector<std::vector<uint8_t>> &u) {
    return impl->show(w, h, c, u);
}

void VulkanView::resize() {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(impl->window, &w, &h);
    impl->requestedSize = (uint64_t(uint32_t(w)) << 32) | uint32_t(h);
    impl->resizeRequested = true;
}

const char *VulkanView::gpuName() const {
    return impl->properties.deviceName;
}

const VulkanView::Timings &VulkanView::timings() const {
    return impl->timing;
}

void VulkanView::setCursor(const td::CursorState &cursor) {
    impl->cursor = cursor;
}

bool VulkanView::redraw() {
    static const std::vector<std::vector<uint8_t>> empty;
    return impl->show(impl->width, impl->height, impl->chroma, empty, false);
}

bool VulkanView::pollCompletion(Completion &result, bool pollGPU) {
    return impl->pollCompletion(result, pollGPU);
}

bool VulkanView::completionPending() const {
    return impl->queryPending || impl->completionReady;
}

void VulkanView::setStatsOverlay(const std::string &text) {
    if (text.empty()) {
        impl->statsVisible = false;
        return;
    }
    impl->statsVisible = true;
    if (text != impl->statsText) {
        impl->statsText = text;
        impl->statsDirty = true;
    }
}

bool VulkanView::showYuv(const YuvFrame &image) {
    static const std::vector<std::vector<uint8_t>> empty;
    return impl->show(image.width, image.height, 0, empty, true, &image);
}
