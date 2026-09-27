# vk_glfw_backend

Camada reutilizável de inicialização **Vulkan + GLFW** para uso com [Dear ImGui](https://github.com/ocornut/imgui). Extraída e generalizada a partir de um boilerplate de setup manual, para poder ser importada em qualquer projeto sem estado global amarrado a uma única aplicação.

Modelo: **uma única** `VkInstance`/`VkDevice`/fila (`InitVulkan`), compartilhados por **N janelas/swapchains** independentes, cada uma representada por um `VkGlfwWindowContext` opaco (`CreateWindowContext`). É o modelo normal do Vulkan para apps multi-janela: um device lógico, várias swapchains.

O que fica de fora, por design, é tudo que é específico de cada projeto: criação da janela GLFW, `ImGui_ImplVulkan_Init`, upload de fontes/texturas e o desenho da UI em si.

## Arquivos

- `vk_glfw_backend.h` — declarações públicas (`VkGlfwBackendConfig`, `VkGlfwWindowContext` opaco e o namespace `VkGlfwBackend`).
- `vk_glfw_backend.cpp` — implementação; o estado global (instância, device) e o layout real de `VkGlfwWindowContext` ficam encapsulados neste arquivo, não expostos no header.

## Dependências

- Vulkan SDK (headers + loader), com suporte a `VK_EXT_debug_utils` para validação (parte do SDK/loader padrão)
- GLFW (compilado com suporte a Vulkan, sem contexto OpenGL/GLES)
- Dear ImGui, com os backends `imgui_impl_vulkan` (e `imgui_impl_glfw` para input/janela, usado pelo app que consome esta lib)
- Opcional: [volk](https://github.com/zeux/volk), se `IMGUI_IMPL_VULKAN_USE_VOLK` estiver definido

## Instalação

Copie `vk_glfw_backend.h` e `vk_glfw_backend.cpp` para dentro do seu projeto e adicione o `.cpp` à sua build (CMake, Makefile, etc.), junto com os arquivos do próprio Dear ImGui (`imgui.cpp`, `imgui_impl_glfw.cpp`, `imgui_impl_vulkan.cpp`, ...).

```cmake
add_executable(meu_app
    main.cpp
    vk_glfw_backend.cpp
    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/backends/imgui_impl_glfw.cpp
    imgui/backends/imgui_impl_vulkan.cpp
)
target_link_libraries(meu_app glfw Vulkan::Vulkan)
```

## Uso (uma janela)

```cpp
#include "vk_glfw_backend.h"
#include "imgui_impl_glfw.h"

int main()
{
    glfwInit();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(1280, 720, "Meu App", nullptr, nullptr);

    // 1. Inicializa instância, GPU, device, fila e descriptor pool
    VkGlfwBackendConfig config;
    config.app_name = "Meu App";
    config.enable_validation_layers = true; // só em debug
    VkGlfwBackend::InitVulkan(config);

    // 2. Cria a surface da janela e o contexto de swapchain
    VkSurfaceKHR surface;
    glfwCreateWindowSurface(VkGlfwBackend::GetInstance(), window, nullptr, &surface);

    int width, height;
    glfwGetFramebufferSize(window, &width, &height);
    VkGlfwWindowContext* ctx = VkGlfwBackend::CreateWindowContext(surface, width, height);

    // 3. Inicializa ImGui usando os handles expostos pelos getters
    ImGui::CreateContext();
    ImGui_ImplGlfw_InitForVulkan(window, true);

    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance       = VkGlfwBackend::GetInstance();
    init_info.PhysicalDevice = VkGlfwBackend::GetPhysicalDevice();
    init_info.Device         = VkGlfwBackend::GetDevice();
    init_info.QueueFamily    = VkGlfwBackend::GetQueueFamily();
    init_info.Queue          = VkGlfwBackend::GetQueue();
    init_info.DescriptorPool = VkGlfwBackend::GetDescriptorPool();
    init_info.PipelineInfoMain.RenderPass = VkGlfwBackend::GetRenderPass(ctx);
    init_info.MinImageCount  = VkGlfwBackend::GetMinImageCount();
    init_info.ImageCount     = VkGlfwBackend::GetMinImageCount();
    ImGui_ImplVulkan_Init(&init_info);

    // 4. Loop principal
    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();

        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        if (VkGlfwBackend::WantsSwapChainRebuild(ctx) && w > 0 && h > 0)
            VkGlfwBackend::ResizeSwapChain(ctx, w, h);

        VkGlfwBackend::NewFrame();
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::ShowDemoWindow();

        ImGui::Render();
        VkGlfwBackend::RenderFrame(ctx, ImGui::GetDrawData());
        VkGlfwBackend::PresentFrame(ctx);
    }

    // 5. Limpeza, na ordem inversa da criação
    vkDeviceWaitIdle(VkGlfwBackend::GetDevice());
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    VkGlfwBackend::DestroyWindowContext(ctx);
    VkGlfwBackend::CleanupVulkan();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
```

## Uso (múltiplas janelas)

`InitVulkan` é chamado uma única vez; cada janela adicional só precisa da sua própria surface e do seu próprio `VkGlfwWindowContext`, todos compartilhando a mesma instância/device/fila:

```cpp
VkGlfwBackend::InitVulkan(config); // uma vez só

VkSurfaceKHR surface_a, surface_b;
glfwCreateWindowSurface(VkGlfwBackend::GetInstance(), window_a, nullptr, &surface_a);
glfwCreateWindowSurface(VkGlfwBackend::GetInstance(), window_b, nullptr, &surface_b);

VkGlfwWindowContext* ctx_a = VkGlfwBackend::CreateWindowContext(surface_a, w_a, h_a);
VkGlfwWindowContext* ctx_b = VkGlfwBackend::CreateWindowContext(surface_b, w_b, h_b);

// cada uma renderiza/apresenta de forma independente:
VkGlfwBackend::RenderFrame(ctx_a, draw_data_a);
VkGlfwBackend::PresentFrame(ctx_a);
VkGlfwBackend::RenderFrame(ctx_b, draw_data_b);
VkGlfwBackend::PresentFrame(ctx_b);

// ao fechar cada uma:
VkGlfwBackend::DestroyWindowContext(ctx_a);
VkGlfwBackend::DestroyWindowContext(ctx_b);
// só depois de destruir todas as janelas:
VkGlfwBackend::CleanupVulkan();
```

Cada janela usa sua própria instância de `ImGui_ImplVulkan` (contextos ImGui separados) se as UIs forem independentes — a lib não impõe isso, só fornece o `RenderPass`/swapchain de cada uma via `GetRenderPass(ctx)`.

> **Versão do Dear ImGui**: os trechos acima assumem uma versão a partir de 2025-09-26, quando `RenderPass`, `Subpass` e `MSAASamples` saíram direto de `ImGui_ImplVulkan_InitInfo` e passaram a ficar agrupados em `init_info.PipelineInfoMain` (do tipo `ImGui_ImplVulkan_PipelineInfo`). Numa versão anterior a essa, é `init_info.RenderPass = VkGlfwBackend::GetRenderPass(ctx);` direto, sem o `PipelineInfoMain.`.

## API

| Função | Escopo | O que faz |
|---|---|---|
| `InitVulkan(config)` | global | Cria instância, escolhe GPU, cria device lógico, fila e descriptor pool. Chamar uma única vez. |
| `CreateWindowContext(surface, w, h)` | por janela | Cria swapchain/render pass/framebuffers para uma surface. Retorna `nullptr` se a GPU/fila não suportar apresentar nela. |
| `DestroyWindowContext(ctx)` | por janela | Libera a swapchain dessa janela. |
| `ResizeSwapChain(ctx, w, h)` | por janela | Recria a swapchain dessa janela num novo tamanho. |
| `WantsSwapChainRebuild(ctx)` | por janela | `true` se a última acquire/present indicou swapchain desatualizada. |
| `NewFrame()` | global | Ponto de extensão antes de `ImGui::NewFrame()` (hoje é um no-op). |
| `RenderFrame(ctx, draw_data)` | por janela | Grava e submete o command buffer com os draw data do ImGui. |
| `PresentFrame(ctx)` | por janela | Apresenta a imagem renderizada dessa janela. |
| `CleanupVulkan()` | global | Libera instância/device/fila/descriptor pool. Chamar por último. |
| `GetInstance()`, `GetDevice()`, `GetPhysicalDevice()`, `GetQueue()`, `GetQueueFamily()`, `GetDescriptorPool()`, `GetAllocator()`, `GetMinImageCount()` | global | Acessores compartilhados por todas as janelas. |
| `GetRenderPass(ctx)` | por janela | Render pass daquela swapchain específica, para `ImGui_ImplVulkan_InitInfo::PipelineInfoMain.RenderPass` (ou `::RenderPass` direto, em versões do ImGui anteriores a 2025-09-26). |

## Correções feitas em relação ao código original

- `g_instance` (minúsculo, usado por engano ao criar o debug callback) corrigido para `g_Instance`.
- `IsExtensionAvailable` / `isExtensionAvailable` — capitalização inconsistente unificada.
- **Suporte a múltiplas janelas**: o estado de swapchain (`ImGui_ImplVulkanH_Window`, flag de rebuild) saiu de globais únicas (`g_MainWindowData`, `g_SwapChainRebuild`) e virou um `VkGlfwWindowContext` por janela, alocado por `CreateWindowContext`. Instância/device/fila continuam globais, como é normal em Vulkan.
- **Migração de `VK_EXT_debug_report` para `VK_EXT_debug_utils`**: a extensão de debug antiga está deprecated. A nova cobre validação, performance e mensagens gerais num único callback, com mais contexto por mensagem, e é encadeada via `pNext` na criação da instância para também capturar problemas em `vkCreateInstance`/`vkDestroyInstance`.
- `CleanupVulkan` só tenta destruir o messenger de debug se ele foi de fato criado (evita destruição incondicional de um handle nulo quando a validação está desligada).
- Debug/validação virou opção de runtime (`enable_validation_layers`) em vez de `#ifdef _DEBUG` fixo em tempo de compilação.
- **Vazamento de `VkSurfaceKHR` corrigido**: desde a versão do ImGui de 2025-09-26, `ImGui_ImplVulkanH_DestroyWindow` parou de destruir a surface internamente (ela é criada pelo chamador, então virou responsabilidade dele). `DestroyWindowContext` agora chama `vkDestroySurfaceKHR` explicitamente depois de `ImGui_ImplVulkanH_DestroyWindow` — a surface passada para `CreateWindowContext` passa a ser propriedade da lib a partir daquele ponto; não a destrua de novo por fora, ou vira double free.

## Limitação que permanece (por design)

- A lib não gerencia carregamento de fontes/texturas do ImGui — isso continua por conta do app, via `GetDevice()`/`GetQueue()`/`GetDescriptorPool()`. Isso não foi tratado como bug: entra em conflito direto com o objetivo de manter a lib pequena e sem opinião sobre como cada projeto organiza upload de assets, e o Dear ImGui já expõe uma API própria para isso (`ImGui_ImplVulkan_CreateFontsTexture`, etc.) que a lib não precisa reembrulhar.
