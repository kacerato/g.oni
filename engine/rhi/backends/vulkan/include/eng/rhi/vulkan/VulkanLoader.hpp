#pragma once

/// Loader dinâmico de Vulkan — estilo volk, minimal.
///
/// Carrega o loader da plataforma (`libvulkan.so.1` no Linux,
/// `libvulkan.so` no Android) via `dlopen` e resolve `vkGetInstanceProcAddr`.
/// FUNÇÕES globais (instance-level) são carregadas com
/// `vkGetInstanceProcAddr(NULL, ...)`; as demais por instance/device.
///
/// Zero link com libvulkan: o backend compila sem SDK de loader e roda em
/// qualquer ambiente que tenha (ou não — probe reporta) a biblioteca.

#include <cstdint>
#include <string>

// Surface Android real (VkAndroidSurfaceCreateInfoKHR exige a
// platform-macro; apenas no NDK — o Linux não a define nem a usa). Sem
// qualquer header de JNI aqui (missão §II.4: NDK API, fronteira rhi).
#ifdef __ANDROID__
#define VK_USE_PLATFORM_ANDROID_KHR 1
#endif

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

namespace eng::rhi::vulkan {

/// Tabela de funções Vulkan usadas pelo backend. Todas são ponteiros —
/// resolvidos em `open()`/`loadInstance()`/`loadDevice()`.
struct VulkanFunctions {
    // --- globais (loader-level) ------------------------------------------
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr{nullptr};
    PFN_vkEnumerateInstanceVersion vkEnumerateInstanceVersion{nullptr};
    PFN_vkEnumerateInstanceExtensionProperties vkEnumerateInstanceExtensionProperties{nullptr};
    PFN_vkEnumerateInstanceLayerProperties vkEnumerateInstanceLayerProperties{nullptr};
    PFN_vkCreateInstance vkCreateInstance{nullptr};

    // --- instance-level -----------------------------------------------------
    PFN_vkDestroyInstance vkDestroyInstance{nullptr};
    PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices{nullptr};
    PFN_vkGetPhysicalDeviceProperties2 vkGetPhysicalDeviceProperties2{nullptr};
    PFN_vkGetPhysicalDeviceFeatures vkGetPhysicalDeviceFeatures{nullptr};
    PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties{nullptr};
    PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties{nullptr};
    PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties{nullptr};
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR vkGetPhysicalDeviceSurfaceSupportKHR{nullptr};
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR vkGetPhysicalDeviceSurfaceCapabilitiesKHR{nullptr};
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR vkGetPhysicalDeviceSurfaceFormatsKHR{nullptr};
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR vkGetPhysicalDeviceSurfacePresentModesKHR{nullptr};
    PFN_vkCreateDevice vkCreateDevice{nullptr};
    PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr{nullptr};
    PFN_vkDestroySurfaceKHR vkDestroySurfaceKHR{nullptr};
    PFN_vkCreateDebugUtilsMessengerEXT vkCreateDebugUtilsMessengerEXT{nullptr};
    PFN_vkDestroyDebugUtilsMessengerEXT vkDestroyDebugUtilsMessengerEXT{nullptr};
    // Surface por plataforma — apenas Headless na FASE 5 (auditoria F5 §3).
    PFN_vkCreateHeadlessSurfaceEXT vkCreateHeadlessSurfaceEXT{nullptr};
#ifdef __ANDROID__
    PFN_vkCreateAndroidSurfaceKHR vkCreateAndroidSurfaceKHR{nullptr};
#endif

    // --- device-level ---------------------------------------------------------
    PFN_vkDestroyDevice vkDestroyDevice{nullptr};
    PFN_vkDeviceWaitIdle vkDeviceWaitIdle{nullptr};
    PFN_vkGetDeviceQueue vkGetDeviceQueue{nullptr};
    PFN_vkCreateSwapchainKHR vkCreateSwapchainKHR{nullptr};
    PFN_vkDestroySwapchainKHR vkDestroySwapchainKHR{nullptr};
    PFN_vkGetSwapchainImagesKHR vkGetSwapchainImagesKHR{nullptr};
    PFN_vkAcquireNextImageKHR vkAcquireNextImageKHR{nullptr};
    PFN_vkQueuePresentKHR vkQueuePresentKHR{nullptr};
    PFN_vkQueueSubmit vkQueueSubmit{nullptr};
    PFN_vkCreateCommandPool vkCreateCommandPool{nullptr};
    PFN_vkDestroyCommandPool vkDestroyCommandPool{nullptr};
    PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers{nullptr};
    PFN_vkFreeCommandBuffers vkFreeCommandBuffers{nullptr};
    PFN_vkBeginCommandBuffer vkBeginCommandBuffer{nullptr};
    PFN_vkEndCommandBuffer vkEndCommandBuffer{nullptr};
    PFN_vkResetCommandBuffer vkResetCommandBuffer{nullptr};
    PFN_vkCreateSemaphore vkCreateSemaphore{nullptr};
    PFN_vkDestroySemaphore vkDestroySemaphore{nullptr};
    PFN_vkCreateFence vkCreateFence{nullptr};
    PFN_vkDestroyFence vkDestroyFence{nullptr};
    PFN_vkWaitForFences vkWaitForFences{nullptr};
    PFN_vkResetFences vkResetFences{nullptr};
    PFN_vkCreateBuffer vkCreateBuffer{nullptr};
    PFN_vkDestroyBuffer vkDestroyBuffer{nullptr};
    PFN_vkAllocateMemory vkAllocateMemory{nullptr};
    PFN_vkFreeMemory vkFreeMemory{nullptr};
    PFN_vkMapMemory vkMapMemory{nullptr};
    PFN_vkUnmapMemory vkUnmapMemory{nullptr};
    PFN_vkBindBufferMemory vkBindBufferMemory{nullptr};
    PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements{nullptr};
    PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier{nullptr};
    PFN_vkCmdCopyBuffer vkCmdCopyBuffer{nullptr};
    PFN_vkCreateImage vkCreateImage{nullptr};
    PFN_vkDestroyImage vkDestroyImage{nullptr};
    PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements{nullptr};
    PFN_vkBindImageMemory vkBindImageMemory{nullptr};
    PFN_vkCreateSampler vkCreateSampler{nullptr};
    PFN_vkDestroySampler vkDestroySampler{nullptr};
    PFN_vkCmdCopyBufferToImage vkCmdCopyBufferToImage{nullptr};
    PFN_vkCmdCopyImageToBuffer vkCmdCopyImageToBuffer{nullptr};
    PFN_vkCmdBlitImage vkCmdBlitImage{nullptr};
    PFN_vkCreateDescriptorPool vkCreateDescriptorPool{nullptr};
    PFN_vkDestroyDescriptorPool vkDestroyDescriptorPool{nullptr};
    PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout{nullptr};
    PFN_vkDestroyDescriptorSetLayout vkDestroyDescriptorSetLayout{nullptr};
    PFN_vkAllocateDescriptorSets vkAllocateDescriptorSets{nullptr};
    PFN_vkUpdateDescriptorSets vkUpdateDescriptorSets{nullptr};
    PFN_vkCmdBindDescriptorSets vkCmdBindDescriptorSets{nullptr};
    PFN_vkResetDescriptorPool vkResetDescriptorPool{nullptr};
    PFN_vkCreateShaderModule vkCreateShaderModule{nullptr};
    PFN_vkDestroyShaderModule vkDestroyShaderModule{nullptr};
    PFN_vkCreatePipelineLayout vkCreatePipelineLayout{nullptr};
    PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout{nullptr};
    PFN_vkCreateRenderPass vkCreateRenderPass{nullptr};
    PFN_vkDestroyRenderPass vkDestroyRenderPass{nullptr};
    PFN_vkCreateGraphicsPipelines vkCreateGraphicsPipelines{nullptr};
    PFN_vkDestroyPipeline vkDestroyPipeline{nullptr};
    PFN_vkCreateFramebuffer vkCreateFramebuffer{nullptr};
    PFN_vkDestroyFramebuffer vkDestroyFramebuffer{nullptr};
    PFN_vkCreateImageView vkCreateImageView{nullptr};
    PFN_vkDestroyImageView vkDestroyImageView{nullptr};
    PFN_vkCmdBeginRenderPass vkCmdBeginRenderPass{nullptr};
    PFN_vkCmdEndRenderPass vkCmdEndRenderPass{nullptr};
    PFN_vkCmdSetViewport vkCmdSetViewport{nullptr};
    PFN_vkCmdSetScissor vkCmdSetScissor{nullptr};
    PFN_vkCmdBindPipeline vkCmdBindPipeline{nullptr};
    PFN_vkCmdBindVertexBuffers vkCmdBindVertexBuffers{nullptr};
    PFN_vkCmdBindIndexBuffer vkCmdBindIndexBuffer{nullptr};
    PFN_vkCmdDraw vkCmdDraw{nullptr};
    PFN_vkCmdDrawIndexed vkCmdDrawIndexed{nullptr};
    PFN_vkCmdClearAttachments vkCmdClearAttachments{nullptr};
};

/// Handle da biblioteca carregada + tabela de funções.
class VulkanLibrary {
public:
    VulkanLibrary() = default;
    ~VulkanLibrary();

    VulkanLibrary(const VulkanLibrary&) = delete;
    VulkanLibrary& operator=(const VulkanLibrary&) = delete;
    VulkanLibrary(VulkanLibrary&& other) noexcept;
    VulkanLibrary& operator=(VulkanLibrary&& other) noexcept;

    /// Abre o loader da plataforma. `false` + motivo em `outError` quando
    /// ausente (probe Unavailable — missão §47).
    [[nodiscard]] bool open(std::string& outError);

    /// Carrega as funções globais (vkGetInstanceProcAddr(NULL, ...)).
    /// Requer `open()` bem-sucedido. `false` se a versão de instance é
    /// < 1.1 (mínimo do backend — ver ADR-037).
    [[nodiscard]] bool loadGlobalFunctions(std::uint32_t& outInstanceVersion,
                                           std::string& outError);

    /// Carrega funções instance-level para `instance`.
    void loadInstanceFunctions(VkInstance instance);

    /// Carrega funções device-level para `device` (via vkGetDeviceProcAddr
    /// quando disponível — ponteiros mais eficientes e corretos por device).
    void loadDeviceFunctions(VkDevice device);

    [[nodiscard]] bool isOpen() const noexcept { return library_ != nullptr; }
    [[nodiscard]] const VulkanFunctions& functions() const noexcept { return functions_; }
    [[nodiscard]] VulkanFunctions& functions() noexcept { return functions_; }

private:
    void close() noexcept;

    void* library_{nullptr};
    VulkanFunctions functions_{};
};

/// Nome canônico do VkResult (mensagens de erro precisas — missão §20).
[[nodiscard]] std::string vkResultName(VkResult result);

} // namespace eng::rhi::vulkan
