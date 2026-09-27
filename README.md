# vk_glfw_backend

Camada reutilizável de inicialização **Vulkan + GLFW** para uso com [Dear ImGui](https://github.com/ocornut/imgui). Extraída e generalizada a partir de um boilerplate de setup manual, para poder ser importada em qualquer projeto sem estado global amarrado a uma única aplicação.

Ela cuida da parte "chata e repetitiva" de qualquer app Vulkan+ImGui: criar instância, escolher GPU, criar device lógico, descriptor pool, swapchain e o ciclo acquire → render → present. O que fica de fora, por design, é tudo que é específico de cada projeto: criação da janela GLFW, `ImGui_ImplVulkan_Init`, upload de fontes/texturas e o desenho da UI em si.

## Arquivos

- `vk_glfw_backend.h` — declarações públicas (`VkGlfwBackendConfig` e o namespace `VkGlfwBackend`).
- `vk_glfw_backend.cpp` — implementação; todo o estado (instância, device, swapchain etc.) fica encapsulado em variáveis `static` internas ao arquivo, não exposto no header.

## Dependências

- Vulkan SDK (headers + loader)
- GLFW (compilado com suporte a Vulkan, sem contexto OpenGL/GLES)
- Dear ImGui, com os backends `imgui_impl_vulkan` (e `imgui_impl_glfw` para input/janela, usado pelo app que consome esta lib)
- Opcional: [volk](https://github.com/zeux/volk), se `IMGUI_IMPL_VULKAN_USE_VOLK` estiver definido

## Instalação

Copie `vk_glfw_backend.h` e `vk_glfw_backend.cpp` para dentro do seu projeto e adicione o `.cpp` à sua build (CMake, Makefile, etc.), junto com os arquivos do próprio Dear ImGui (`imgui.cpp`, `imgui_impl_glfw.cpp`, `imgui_impl_vulkan.cpp`, ...).

Exemplo de `CMakeLists.txt` mínimo:

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

## Uso

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

    // 2. Cria a surface da janela e a swapchain
    VkSurfaceKHR surface;
    glfwCreateWindowSurface(VkGlfwBackend::GetInstance(), window, nullptr, &surface);

    int width, height;
    glfwGetFramebufferSize(window, &width, &height);
    VkGlfwBackend::InitWindow(surface, width, height);

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
    init_info.RenderPass     = VkGlfwBackend::GetRenderPass();
    init_info.MinImageCount  = VkGlfwBackend::GetMinImageCount();
    init_info.ImageCount     = VkGlfwBackend::GetMinImageCount();
    ImGui_ImplVulkan_Init(&init_info);

    // 4. Loop principal
    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();

        // Redimensionamento pendente da swapchain?
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        if (VkGlfwBackend::WantsSwapChainRebuild() && w > 0 && h > 0)
            VkGlfwBackend::ResizeSwapChain(w, h);

        VkGlfwBackend::NewFrame();
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::ShowDemoWindow();

        ImGui::Render();
        VkGlfwBackend::RenderFrame(ImGui::GetDrawData());
        VkGlfwBackend::PresentFrame();
    }

    // 5. Limpeza, na ordem inversa da criação
    vkDeviceWaitIdle(VkGlfwBackend::GetDevice());
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    VkGlfwBackend::CleanupWindow();
    VkGlfwBackend::CleanupVulkan();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
```

## API

| Função | O que faz |
|---|---|
| `InitVulkan(config)` | Cria instância, escolhe GPU, cria device lógico, fila e descriptor pool. Chamar uma única vez. |
| `InitWindow(surface, w, h)` | Cria swapchain/render pass/framebuffers para uma surface já criada. |
| `ResizeSwapChain(w, h)` | Recria a swapchain num novo tamanho. |
| `WantsSwapChainRebuild()` | `true` se a última acquire/present indicou swapchain desatualizada. |
| `NewFrame()` | Ponto de extensão antes de `ImGui::NewFrame()` (hoje é um no-op; existe para manter o ciclo de frame explícito). |
| `RenderFrame(draw_data)` | Grava e submete o command buffer com os draw data do ImGui. |
| `PresentFrame()` | Apresenta a imagem renderizada. |
| `CleanupWindow()` / `CleanupVulkan()` | Liberação de recursos, na ordem inversa da criação. |
| `GetInstance()`, `GetDevice()`, `GetPhysicalDevice()`, `GetQueue()`, `GetQueueFamily()`, `GetDescriptorPool()`, `GetAllocator()`, `GetRenderPass()`, `GetMinImageCount()` | Acessores para os handles internos, usados ao inicializar `imgui_impl_vulkan` e ao criar recursos extras (texturas, samplers). |
