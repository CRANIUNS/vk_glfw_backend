#include "vk_glfw_backend.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef IMGUI_IMPL_VULKAN_USE_VOLK
#define VOLK_IMPLEMENTATION
#include "volk/volk.h"
#endif

// Definição real do contexto de janela — opaco para quem inclui o header.
struct VkGlfwWindowContext
{
    ImGui_ImplVulkanH_Window Window;
    bool SwapChainRebuild = false;
};

namespace
{
    // Estado global: compartilhado por todas as janelas (uma instância,
    // um device lógico, uma fila — o modelo normal do Vulkan multi-janela).
    VkAllocationCallbacks*   g_Allocator      = nullptr;
    VkInstance               g_Instance       = VK_NULL_HANDLE;
    VkPhysicalDevice         g_PhysicalDevice = VK_NULL_HANDLE;
    VkDevice                 g_Device         = VK_NULL_HANDLE;
    uint32_t                 g_QueueFamily    = (uint32_t)-1;
    VkQueue                  g_Queue          = VK_NULL_HANDLE;
    VkDescriptorPool         g_DescriptorPool = VK_NULL_HANDLE;

    uint32_t g_MinImageCount = 2;
    ImVec4   g_ClearColor    = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);

    bool                        g_ValidationEnabled = false;
    VkDebugUtilsMessengerEXT    g_DebugMessenger    = VK_NULL_HANDLE;

    void check_vk_result(VkResult err)
    {
        if (err == VK_SUCCESS)
            return;
        fprintf(stderr, "[VkGlfwBackend] Vulkan Error: %d\n", err);
        if (err < 0)
            abort();
    }

    bool isExtensionAvailable(const ImVector<VkExtensionProperties>& properties, const char* extension)
    {
        for (const VkExtensionProperties& p : properties)
            if (strcmp(p.extensionName, extension) == 0)
                return true;
        return false;
    }

    // VK_EXT_debug_utils: substitui o antigo VK_EXT_debug_report
    // (deprecated), com mais contexto por mensagem e um único callback
    // cobrindo validação, performance e mensagens gerais.
    VKAPI_ATTR VkBool32 VKAPI_CALL debug_utils_callback(
        VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
        VkDebugUtilsMessageTypeFlagsEXT messageType,
        const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
        void* pUserData)
    {
        (void)messageType; (void)pUserData;
        const char* severity = "INFO";
        if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
            severity = "ERROR";
        else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
            severity = "WARNING";
        fprintf(stderr, "[vulkan][%s] %s\n\n", severity, pCallbackData->pMessage);
        return VK_FALSE;
    }

    VkDebugUtilsMessengerCreateInfoEXT MakeDebugMessengerCreateInfo()
    {
        VkDebugUtilsMessengerCreateInfoEXT ci{};
        ci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        ci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                            | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        ci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                        | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                        | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        ci.pfnUserCallback = debug_utils_callback;
        return ci;
    }
}

bool VkGlfwBackend::InitVulkan(const VkGlfwBackendConfig& config)
{
    g_MinImageCount     = config.min_image_count;
    g_ClearColor        = config.clear_color;
    g_ValidationEnabled = config.enable_validation_layers;

    VkResult err;

#ifdef IMGUI_IMPL_VULKAN_USE_VOLK
    volkInitialize();
#endif

    // ---- Instância ----
    {
        uint32_t glfw_ext_count = 0;
        const char** glfw_exts = glfwGetRequiredInstanceExtensions(&glfw_ext_count);

        ImVector<const char*> instance_extensions;
        for (uint32_t i = 0; i < glfw_ext_count; i++)
            instance_extensions.push_back(glfw_exts[i]);
        for (const char* ext : config.extra_instance_extensions)
            instance_extensions.push_back(ext);

        VkInstanceCreateInfo create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;

        uint32_t properties_count;
        ImVector<VkExtensionProperties> properties;
        vkEnumerateInstanceExtensionProperties(nullptr, &properties_count, nullptr);
        properties.resize(properties_count);
        err = vkEnumerateInstanceExtensionProperties(nullptr, &properties_count, properties.Data);
        check_vk_result(err);

        if (isExtensionAvailable(properties, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
            instance_extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
        if (isExtensionAvailable(properties, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME))
        {
            instance_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            create_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
#endif

        const char* layers[] = { "VK_LAYER_KHRONOS_validation" };
        VkDebugUtilsMessengerCreateInfoEXT debug_messenger_ci{};
        if (g_ValidationEnabled)
        {
            create_info.enabledLayerCount   = 1;
            create_info.ppEnabledLayerNames = layers;
            instance_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

            // Encadeado em pNext: cobre também vkCreateInstance/vkDestroyInstance,
            // que o messenger criado depois da instância não alcança.
            debug_messenger_ci = MakeDebugMessengerCreateInfo();
            create_info.pNext = &debug_messenger_ci;
        }

        VkApplicationInfo app_info = {};
        app_info.sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app_info.pApplicationName = config.app_name;
        app_info.apiVersion       = VK_API_VERSION_1_0;
        create_info.pApplicationInfo = &app_info;

        create_info.enabledExtensionCount   = (uint32_t)instance_extensions.Size;
        create_info.ppEnabledExtensionNames = instance_extensions.Data;
        err = vkCreateInstance(&create_info, g_Allocator, &g_Instance);
        check_vk_result(err);

#ifdef IMGUI_IMPL_VULKAN_USE_VOLK
        volkLoadInstance(g_Instance);
#endif

        if (g_ValidationEnabled)
        {
            auto f_vkCreateDebugUtilsMessengerEXT =
                (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_Instance, "vkCreateDebugUtilsMessengerEXT");
            IM_ASSERT(f_vkCreateDebugUtilsMessengerEXT != nullptr);
            VkDebugUtilsMessengerCreateInfoEXT ci = MakeDebugMessengerCreateInfo();
            err = f_vkCreateDebugUtilsMessengerEXT(g_Instance, &ci, g_Allocator, &g_DebugMessenger);
            check_vk_result(err);
        }
    }

    // ---- GPU física ----
    g_PhysicalDevice = ImGui_ImplVulkanH_SelectPhysicalDevice(g_Instance);
    IM_ASSERT(g_PhysicalDevice != VK_NULL_HANDLE);

    g_QueueFamily = ImGui_ImplVulkanH_SelectQueueFamilyIndex(g_PhysicalDevice);
    IM_ASSERT(g_QueueFamily != (uint32_t)-1);

    // ---- Device lógico ----
    {
        ImVector<const char*> device_extensions;
        device_extensions.push_back("VK_KHR_swapchain");
        for (const char* ext : config.extra_device_extensions)
            device_extensions.push_back(ext);

        uint32_t properties_count;
        ImVector<VkExtensionProperties> properties;
        vkEnumerateDeviceExtensionProperties(g_PhysicalDevice, nullptr, &properties_count, nullptr);
        properties.resize(properties_count);
        vkEnumerateDeviceExtensionProperties(g_PhysicalDevice, nullptr, &properties_count, properties.Data);
#ifdef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
        if (isExtensionAvailable(properties, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME))
            device_extensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
#endif

        const float queue_priority[] = { 1.0f };
        VkDeviceQueueCreateInfo queue_info[1] = {};
        queue_info[0].sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info[0].queueFamilyIndex = g_QueueFamily;
        queue_info[0].queueCount       = 1;
        queue_info[0].pQueuePriorities = queue_priority;

        VkDeviceCreateInfo create_info      = {};
        create_info.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        create_info.queueCreateInfoCount    = sizeof(queue_info) / sizeof(queue_info[0]);
        create_info.pQueueCreateInfos       = queue_info;
        create_info.enabledExtensionCount   = (uint32_t)device_extensions.Size;
        create_info.ppEnabledExtensionNames = device_extensions.Data;
        err = vkCreateDevice(g_PhysicalDevice, &create_info, g_Allocator, &g_Device);
        check_vk_result(err);
        vkGetDeviceQueue(g_Device, g_QueueFamily, 0, &g_Queue);
    }

    // ---- Descriptor pool ----
    {
        VkDescriptorPoolSize pool_sizes[] =
        {
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE },
            { VK_DESCRIPTOR_TYPE_SAMPLER,       IMGUI_IMPL_VULKAN_MINIMUM_SAMPLER_POOL_SIZE },
        };
        VkDescriptorPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool_info.maxSets = 0;
        for (VkDescriptorPoolSize& pool_size : pool_sizes)
            pool_info.maxSets += pool_size.descriptorCount;
        pool_info.poolSizeCount = (uint32_t)IM_COUNTOF(pool_sizes);
        pool_info.pPoolSizes    = pool_sizes;
        err = vkCreateDescriptorPool(g_Device, &pool_info, g_Allocator, &g_DescriptorPool);
        check_vk_result(err);
    }

    return true;
}

VkGlfwWindowContext* VkGlfwBackend::CreateWindowContext(VkSurfaceKHR surface, int width, int height)
{
    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(g_PhysicalDevice, g_QueueFamily, surface, &supported);
    if (supported != VK_TRUE)
    {
        fprintf(stderr, "[VkGlfwBackend] A GPU/fila escolhida nao suporta apresentar nesta surface (WSI).\n");
        return nullptr;
    }

    auto* ctx = new VkGlfwWindowContext();
    ImGui_ImplVulkanH_Window* wd = &ctx->Window;

    wd->ClearValue.color.float32[0] = g_ClearColor.x;
    wd->ClearValue.color.float32[1] = g_ClearColor.y;
    wd->ClearValue.color.float32[2] = g_ClearColor.z;
    wd->ClearValue.color.float32[3] = g_ClearColor.w;

    const VkFormat requestSurfaceImageFormat[] = {
        VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_B8G8R8_UNORM,   VK_FORMAT_R8G8B8_UNORM
    };
    const VkColorSpaceKHR requestSurfaceColorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR;

    wd->Surface = surface;
    wd->SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(
        g_PhysicalDevice, wd->Surface, requestSurfaceImageFormat,
        (size_t)IM_COUNTOF(requestSurfaceImageFormat), requestSurfaceColorSpace);

#ifdef APP_USE_UNLIMITED_FRAME_RATE
    VkPresentModeKHR present_modes[] = { VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_FIFO_KHR };
#else
    VkPresentModeKHR present_modes[] = { VK_PRESENT_MODE_FIFO_KHR };
#endif
    wd->PresentMode = ImGui_ImplVulkanH_SelectPresentMode(g_PhysicalDevice, wd->Surface, present_modes, IM_COUNTOF(present_modes));

    IM_ASSERT(g_MinImageCount >= 2);
    ImGui_ImplVulkanH_CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device, wd, g_QueueFamily, g_Allocator, width, height, g_MinImageCount, 0);

    return ctx;
}

void VkGlfwBackend::DestroyWindowContext(VkGlfwWindowContext* ctx)
{
    if (!ctx)
        return;
    ImGui_ImplVulkanH_DestroyWindow(g_Instance, g_Device, &ctx->Window, g_Allocator);
    delete ctx;
}

void VkGlfwBackend::ResizeSwapChain(VkGlfwWindowContext* ctx, int width, int height)
{
    if (!ctx || width <= 0 || height <= 0)
        return;

    ImGui_ImplVulkan_SetMinImageCount(g_MinImageCount);
    ImGui_ImplVulkanH_CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device, &ctx->Window, g_QueueFamily, g_Allocator, width, height, g_MinImageCount, 0);
    ctx->Window.FrameIndex = 0;
    ctx->SwapChainRebuild = false;
}

bool VkGlfwBackend::WantsSwapChainRebuild(VkGlfwWindowContext* ctx)
{
    return ctx != nullptr && ctx->SwapChainRebuild;
}

void VkGlfwBackend::NewFrame()
{
    // Ponto de extensão: nada de compartilhado entre janelas precisa
    // acontecer aqui hoje; existe para manter o ciclo de frame explícito
    // e simétrico com RenderFrame/PresentFrame.
}

void VkGlfwBackend::RenderFrame(VkGlfwWindowContext* ctx, ImDrawData* draw_data)
{
    if (!ctx)
        return;

    ImGui_ImplVulkanH_Window* wd = &ctx->Window;

    VkSemaphore image_acquired_semaphore  = wd->FrameSemaphores[wd->SemaphoreIndex].ImageAcquiredSemaphore;
    VkSemaphore render_complete_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;

    VkResult err = vkAcquireNextImageKHR(g_Device, wd->Swapchain, UINT64_MAX, image_acquired_semaphore, VK_NULL_HANDLE, &wd->FrameIndex);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR)
        ctx->SwapChainRebuild = true;
    if (err == VK_ERROR_OUT_OF_DATE_KHR)
        return;
    if (err != VK_SUBOPTIMAL_KHR)
        check_vk_result(err);

    ImGui_ImplVulkanH_Frame* fd = &wd->Frames[wd->FrameIndex];
    {
        err = vkWaitForFences(g_Device, 1, &fd->Fence, VK_TRUE, UINT64_MAX);
        check_vk_result(err);
        err = vkResetFences(g_Device, 1, &fd->Fence);
        check_vk_result(err);
    }
    {
        err = vkResetCommandPool(g_Device, fd->CommandPool, 0);
        check_vk_result(err);
        VkCommandBufferBeginInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        err = vkBeginCommandBuffer(fd->CommandBuffer, &info);
        check_vk_result(err);
    }
    {
        VkRenderPassBeginInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        info.renderPass  = wd->RenderPass;
        info.framebuffer = fd->Framebuffer;
        info.renderArea.extent.width  = wd->Width;
        info.renderArea.extent.height = wd->Height;
        info.clearValueCount = 1;
        info.pClearValues    = &wd->ClearValue;
        vkCmdBeginRenderPass(fd->CommandBuffer, &info, VK_SUBPASS_CONTENTS_INLINE);
    }

    ImGui_ImplVulkan_RenderDrawData(draw_data, fd->CommandBuffer);

    vkCmdEndRenderPass(fd->CommandBuffer);
    {
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        info.waitSemaphoreCount   = 1;
        info.pWaitSemaphores      = &image_acquired_semaphore;
        info.pWaitDstStageMask    = &wait_stage;
        info.commandBufferCount   = 1;
        info.pCommandBuffers      = &fd->CommandBuffer;
        info.signalSemaphoreCount = 1;
        info.pSignalSemaphores    = &render_complete_semaphore;

        err = vkEndCommandBuffer(fd->CommandBuffer);
        check_vk_result(err);
        err = vkQueueSubmit(g_Queue, 1, &info, fd->Fence);
        check_vk_result(err);
    }
}

void VkGlfwBackend::PresentFrame(VkGlfwWindowContext* ctx)
{
    if (!ctx || ctx->SwapChainRebuild)
        return;

    ImGui_ImplVulkanH_Window* wd = &ctx->Window;
    VkSemaphore render_complete_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;

    VkPresentInfoKHR info = {};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores    = &render_complete_semaphore;
    info.swapchainCount     = 1;
    info.pSwapchains        = &wd->Swapchain;
    info.pImageIndices      = &wd->FrameIndex;

    VkResult err = vkQueuePresentKHR(g_Queue, &info);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR)
        ctx->SwapChainRebuild = true;
    if (err == VK_ERROR_OUT_OF_DATE_KHR)
        return;
    if (err != VK_SUBOPTIMAL_KHR)
        check_vk_result(err);

    wd->SemaphoreIndex = (wd->SemaphoreIndex + 1) % wd->SemaphoreCount;
}

void VkGlfwBackend::CleanupVulkan()
{
    vkDestroyDescriptorPool(g_Device, g_DescriptorPool, g_Allocator);

    if (g_ValidationEnabled && g_DebugMessenger != VK_NULL_HANDLE)
    {
        auto f_vkDestroyDebugUtilsMessengerEXT =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_Instance, "vkDestroyDebugUtilsMessengerEXT");
        if (f_vkDestroyDebugUtilsMessengerEXT)
            f_vkDestroyDebugUtilsMessengerEXT(g_Instance, g_DebugMessenger, g_Allocator);
    }

    vkDestroyDevice(g_Device, g_Allocator);
    vkDestroyInstance(g_Instance, g_Allocator);
}

// ---- Acessores globais ----
VkInstance             VkGlfwBackend::GetInstance()       { return g_Instance; }
VkPhysicalDevice       VkGlfwBackend::GetPhysicalDevice() { return g_PhysicalDevice; }
VkDevice               VkGlfwBackend::GetDevice()         { return g_Device; }
VkQueue                VkGlfwBackend::GetQueue()          { return g_Queue; }
uint32_t               VkGlfwBackend::GetQueueFamily()    { return g_QueueFamily; }
VkDescriptorPool       VkGlfwBackend::GetDescriptorPool() { return g_DescriptorPool; }
VkAllocationCallbacks* VkGlfwBackend::GetAllocator()      { return g_Allocator; }
uint32_t               VkGlfwBackend::GetMinImageCount()  { return g_MinImageCount; }

// ---- Acessor por janela ----
VkRenderPass VkGlfwBackend::GetRenderPass(VkGlfwWindowContext* ctx)
{
    return ctx ? ctx->Window.RenderPass : VK_NULL_HANDLE;
}
