#pragma once

// ============================================================================
//  vk_glfw_backend.h
//  Camada reutilizável de inicialização Vulkan + GLFW para uso com Dear ImGui.
//
//  Modelo: uma única VkInstance/VkDevice/fila (InitVulkan), compartilhados
//  por N janelas/swapchains independentes, cada uma representada por um
//  VkGlfwWindowContext opaco (CreateWindowContext). Isso é o modelo normal
//  do Vulkan para apps multi-janela: um device lógico, várias swapchains.
//
//  Uso típico (uma janela; para várias, repita os passos 3-4 por surface):
//      1. glfwInit() + glfwCreateWindow(...)  (GLFW_CLIENT_API = GLFW_NO_API)
//      2. VkGlfwBackend::InitVulkan(config)
//      3. glfwCreateWindowSurface(VkGlfwBackend::GetInstance(), window, nullptr, &surface)
//      4. auto* ctx = VkGlfwBackend::CreateWindowContext(surface, largura, altura)
//      5. ImGui_ImplVulkan_Init(...) usando os handles obtidos pelos getters
//         (RenderPass vem de GetRenderPass(ctx))
//      6. Loop: NewFrame() -> ImGui::NewFrame() -> ... -> RenderFrame(ctx, ...) -> PresentFrame(ctx)
//      7. Ao redimensionar: se WantsSwapChainRebuild(ctx), chamar ResizeSwapChain(ctx, ...)
//      8. Ao encerrar: DestroyWindowContext(ctx) para cada janela, depois CleanupVulkan()
// ============================================================================

#include <vector>
#include <cstdint>

#include "imgui.h"
#include "imgui_impl_vulkan.h"

#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

struct VkGlfwBackendConfig
{
    const char* app_name = "Dear ImGui App";

    // Ativa VK_LAYER_KHRONOS_validation + VK_EXT_debug_utils. Ligar só em
    // builds de debug: tem custo de performance e exige o Vulkan SDK /
    // camadas instaladas na máquina.
    bool enable_validation_layers = false;

    // Extensões extras exigidas pelo projeto específico, além das que o
    // GLFW já pede (via glfwGetRequiredInstanceExtensions) e do
    // VK_KHR_swapchain, que já são adicionadas automaticamente.
    std::vector<const char*> extra_instance_extensions;
    std::vector<const char*> extra_device_extensions;

    uint32_t min_image_count = 2;
    ImVec4   clear_color     = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
};

// Contexto opaco de uma janela/swapchain. O chamador só manipula o
// ponteiro; o layout é um detalhe de implementação do .cpp.
struct VkGlfwWindowContext;

namespace VkGlfwBackend
{
    // Cria VkInstance, escolhe GPU, cria device lógico, fila e descriptor
    // pool. Chamar uma única vez, antes de criar qualquer surface.
    bool InitVulkan(const VkGlfwBackendConfig& config);

    // Cria um novo contexto de janela/swapchain para uma surface já criada
    // (glfwCreateWindowSurface). Pode ser chamada várias vezes para abrir
    // várias janelas simultâneas, todas compartilhando a mesma instância e
    // device. Retorna nullptr se a GPU/fila escolhida não suportar
    // apresentar nessa surface.
    VkGlfwWindowContext* CreateWindowContext(VkSurfaceKHR surface, int width, int height);

    // Libera a swapchain/render pass/framebuffers dessa janela E a
    // VkSurfaceKHR passada para CreateWindowContext (a partir do ImGui
    // 2025-09-26, o helper de destroy não faz mais isso sozinho). Chamar
    // para cada contexto antes de CleanupVulkan(); não chamar
    // vkDestroySurfaceKHR de novo por fora, ou dará double free.
    void DestroyWindowContext(VkGlfwWindowContext* ctx);

    // Recria a swapchain de uma janela específica num novo tamanho.
    void ResizeSwapChain(VkGlfwWindowContext* ctx, int width, int height);
    bool WantsSwapChainRebuild(VkGlfwWindowContext* ctx);

    // Ciclo de frame (por janela, exceto NewFrame que é global)
    void NewFrame();
    void RenderFrame(VkGlfwWindowContext* ctx, ImDrawData* draw_data);
    void PresentFrame(VkGlfwWindowContext* ctx);

    // Libera instância/device/fila/descriptor pool. Chamar por último,
    // depois de DestroyWindowContext em todas as janelas.
    void CleanupVulkan();

    // ---- Acessores globais (compartilhados por todas as janelas) ----
    VkInstance             GetInstance();
    VkPhysicalDevice       GetPhysicalDevice();
    VkDevice               GetDevice();
    VkQueue                GetQueue();
    uint32_t               GetQueueFamily();
    VkDescriptorPool       GetDescriptorPool();
    VkAllocationCallbacks* GetAllocator();
    uint32_t               GetMinImageCount();

    // ---- Acessor por janela ----
    VkRenderPass GetRenderPass(VkGlfwWindowContext* ctx);
}
