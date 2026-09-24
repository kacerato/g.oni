/// Loader Vulkan — implementação. Ver VulkanLoader.hpp.

#include "eng/rhi/vulkan/VulkanLoader.hpp"

#include <dlfcn.h>

#include <utility>

#include "eng/log/Macros.hpp"

ENG_LOG_CATEGORY("rhi.vulkan")

namespace eng::rhi::vulkan {
namespace {

/// Candidatos de biblioteca, em ordem (Linux/Android; FASE 5 roda em Linux
/// e o mesmo caminho serve ao Android — missão §30).
constexpr const char* kLoaderCandidates[] = {
    "libvulkan.so.1",
    "libvulkan.so",
};

/// Resolve uma função instance-level por nome (via table do loader).
template <typename Fn>
Fn instanceProc(const VulkanFunctions& fn, VkInstance instance, const char* name) {
    return reinterpret_cast<Fn>(fn.vkGetInstanceProcAddr(instance, name));
}

} // namespace

VulkanLibrary::~VulkanLibrary() {
    close();
}

VulkanLibrary::VulkanLibrary(VulkanLibrary&& other) noexcept
    : library_(other.library_), functions_(other.functions_) {
    other.library_ = nullptr;
    other.functions_ = VulkanFunctions{};
}

VulkanLibrary& VulkanLibrary::operator=(VulkanLibrary&& other) noexcept {
    if (this != &other) {
        close();
        library_ = other.library_;
        functions_ = other.functions_;
        other.library_ = nullptr;
        other.functions_ = VulkanFunctions{};
    }
    return *this;
}

void VulkanLibrary::close() noexcept {
    // NÃO dlclose: drivers/loaders gráficos retêm threads e estruturas
    // internas além da destruição canônica (prática consolidada — volk
    // idem); o dlclose quebra a atribuição de stacks do LSan e pode
    // derrubar estados globais do driver. O handle vive até o fim do
    // processo e o OS recupera a memória. A tabela apenas é zerada.
    library_ = nullptr;
    functions_ = VulkanFunctions{};
}

bool VulkanLibrary::open(std::string& outError) {
    if (library_ != nullptr) {
        return true;
    }
    for (const char* candidate : kLoaderCandidates) {
        library_ = dlopen(candidate, RTLD_NOW | RTLD_LOCAL);
        if (library_ != nullptr) {
            ENG_INFO("rhi.vulkan: loader '{}' carregado", candidate);
            return true;
        }
    }
    outError = std::string{"loader Vulkan ausente: "} + dlerror();
    return false;
}

bool VulkanLibrary::loadGlobalFunctions(std::uint32_t& outInstanceVersion,
                                         std::string& outError) {
    if (library_ == nullptr) {
        outError = "loader não aberto";
        return false;
    }
    functions_.vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        dlsym(library_, "vkGetInstanceProcAddr"));
    if (functions_.vkGetInstanceProcAddr == nullptr) {
        outError = "vkGetInstanceProcAddr ausente no loader";
        return false;
    }
    // vkEnumerateInstanceVersion é carregável com instance == NULL (1.1+).
    functions_.vkEnumerateInstanceVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
        functions_.vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));
    if (functions_.vkEnumerateInstanceVersion == nullptr) {
        // Sem a função: Vulkan 1.0 puro — abaixo do mínimo do backend.
        outInstanceVersion = VK_MAKE_VERSION(1, 0, 0);
    } else if (functions_.vkEnumerateInstanceVersion(&outInstanceVersion) != VK_SUCCESS) {
        outError = "vkEnumerateInstanceVersion falhou";
        return false;
    }
    if (outInstanceVersion < VK_API_VERSION_1_1) {
        outError = "Vulkan " +
                   std::to_string(VK_API_VERSION_MAJOR(outInstanceVersion)) + "." +
                   std::to_string(VK_API_VERSION_MINOR(outInstanceVersion)) +
                   " — o backend exige >= 1.1";
        return false;
    }

    functions_.vkEnumerateInstanceExtensionProperties =
        reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
            functions_.vkGetInstanceProcAddr(
                nullptr, "vkEnumerateInstanceExtensionProperties"));
    functions_.vkEnumerateInstanceLayerProperties =
        reinterpret_cast<PFN_vkEnumerateInstanceLayerProperties>(
            functions_.vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceLayerProperties"));
    functions_.vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(
        functions_.vkGetInstanceProcAddr(nullptr, "vkCreateInstance"));
    if (functions_.vkEnumerateInstanceExtensionProperties == nullptr ||
        functions_.vkEnumerateInstanceLayerProperties == nullptr ||
        functions_.vkCreateInstance == nullptr) {
        outError = "funções globais essenciais ausentes no loader";
        return false;
    }
    return true;
}

void VulkanLibrary::loadInstanceFunctions(VkInstance instance) {
    auto& fn = functions_;
    fn.vkDestroyInstance = instanceProc<PFN_vkDestroyInstance>(fn, instance, "vkDestroyInstance");
    fn.vkEnumeratePhysicalDevices =
        instanceProc<PFN_vkEnumeratePhysicalDevices>(fn, instance, "vkEnumeratePhysicalDevices");
    fn.vkGetPhysicalDeviceProperties2 = instanceProc<PFN_vkGetPhysicalDeviceProperties2>(
        fn, instance, "vkGetPhysicalDeviceProperties2");
    fn.vkGetPhysicalDeviceFeatures = instanceProc<PFN_vkGetPhysicalDeviceFeatures>(
        fn, instance, "vkGetPhysicalDeviceFeatures");
    fn.vkGetPhysicalDeviceQueueFamilyProperties =
        instanceProc<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            fn, instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    fn.vkGetPhysicalDeviceMemoryProperties =
        instanceProc<PFN_vkGetPhysicalDeviceMemoryProperties>(
            fn, instance, "vkGetPhysicalDeviceMemoryProperties");
    fn.vkEnumerateDeviceExtensionProperties =
        instanceProc<PFN_vkEnumerateDeviceExtensionProperties>(
            fn, instance, "vkEnumerateDeviceExtensionProperties");
    fn.vkGetPhysicalDeviceSurfaceSupportKHR =
        instanceProc<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(
            fn, instance, "vkGetPhysicalDeviceSurfaceSupportKHR");
    fn.vkGetPhysicalDeviceSurfaceCapabilitiesKHR =
        instanceProc<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
            fn, instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    fn.vkGetPhysicalDeviceSurfaceFormatsKHR = instanceProc<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(
        fn, instance, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    fn.vkGetPhysicalDeviceSurfacePresentModesKHR =
        instanceProc<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>(
            fn, instance, "vkGetPhysicalDeviceSurfacePresentModesKHR");
    fn.vkCreateDevice =
        instanceProc<PFN_vkCreateDevice>(fn, instance, "vkCreateDevice");
    fn.vkGetDeviceProcAddr =
        instanceProc<PFN_vkGetDeviceProcAddr>(fn, instance, "vkGetDeviceProcAddr");
    fn.vkDestroySurfaceKHR =
        instanceProc<PFN_vkDestroySurfaceKHR>(fn, instance, "vkDestroySurfaceKHR");
    fn.vkCreateDebugUtilsMessengerEXT =
        instanceProc<PFN_vkCreateDebugUtilsMessengerEXT>(
            fn, instance, "vkCreateDebugUtilsMessengerEXT");
    fn.vkDestroyDebugUtilsMessengerEXT =
        instanceProc<PFN_vkDestroyDebugUtilsMessengerEXT>(
            fn, instance, "vkDestroyDebugUtilsMessengerEXT");
    fn.vkCreateHeadlessSurfaceEXT = instanceProc<PFN_vkCreateHeadlessSurfaceEXT>(
        fn, instance, "vkCreateHeadlessSurfaceEXT");
#ifdef __ANDROID__
    fn.vkCreateAndroidSurfaceKHR = instanceProc<PFN_vkCreateAndroidSurfaceKHR>(
        fn, instance, "vkCreateAndroidSurfaceKHR");
#endif
}

void VulkanLibrary::loadDeviceFunctions(VkDevice device) {
    auto& fn = functions_;
    // Preferência: ponteiros device-level (corretos para o device, evitam
    // dispatch por instance). Fallback: instance-level (vkDestroyDevice e
    // vkGetDeviceQueue SÓ existem em instance-level na prática).
    auto dev = [&fn, device](const char* name) -> PFN_vkVoidFunction {
        PFN_vkVoidFunction proc = nullptr;
        if (fn.vkGetDeviceProcAddr != nullptr) {
            proc = fn.vkGetDeviceProcAddr(device, name);
        }
        if (proc == nullptr && fn.vkGetInstanceProcAddr != nullptr) {
            proc = fn.vkGetInstanceProcAddr(nullptr, name);
        }
        return proc;
    };
#define ENG_VK_LOAD(name) \
    fn.name = reinterpret_cast<PFN_##name>(dev(#name))
    ENG_VK_LOAD(vkDestroyDevice);
    ENG_VK_LOAD(vkDeviceWaitIdle);
    ENG_VK_LOAD(vkGetDeviceQueue);
    ENG_VK_LOAD(vkCreateSwapchainKHR);
    ENG_VK_LOAD(vkDestroySwapchainKHR);
    ENG_VK_LOAD(vkGetSwapchainImagesKHR);
    ENG_VK_LOAD(vkAcquireNextImageKHR);
    ENG_VK_LOAD(vkQueuePresentKHR);
    ENG_VK_LOAD(vkQueueSubmit);
    ENG_VK_LOAD(vkCreateCommandPool);
    ENG_VK_LOAD(vkDestroyCommandPool);
    ENG_VK_LOAD(vkAllocateCommandBuffers);
    ENG_VK_LOAD(vkFreeCommandBuffers);
    ENG_VK_LOAD(vkBeginCommandBuffer);
    ENG_VK_LOAD(vkEndCommandBuffer);
    ENG_VK_LOAD(vkResetCommandBuffer);
    ENG_VK_LOAD(vkCreateSemaphore);
    ENG_VK_LOAD(vkDestroySemaphore);
    ENG_VK_LOAD(vkCreateFence);
    ENG_VK_LOAD(vkDestroyFence);
    ENG_VK_LOAD(vkWaitForFences);
    ENG_VK_LOAD(vkResetFences);
    ENG_VK_LOAD(vkCreateBuffer);
    ENG_VK_LOAD(vkDestroyBuffer);
    ENG_VK_LOAD(vkAllocateMemory);
    ENG_VK_LOAD(vkFreeMemory);
    ENG_VK_LOAD(vkMapMemory);
    ENG_VK_LOAD(vkUnmapMemory);
    ENG_VK_LOAD(vkBindBufferMemory);
    ENG_VK_LOAD(vkGetBufferMemoryRequirements);
    ENG_VK_LOAD(vkCmdPipelineBarrier);
    ENG_VK_LOAD(vkCmdCopyBuffer);
    ENG_VK_LOAD(vkCreateImage);
    ENG_VK_LOAD(vkDestroyImage);
    ENG_VK_LOAD(vkGetImageMemoryRequirements);
    ENG_VK_LOAD(vkBindImageMemory);
    ENG_VK_LOAD(vkCreateSampler);
    ENG_VK_LOAD(vkDestroySampler);
    ENG_VK_LOAD(vkCmdCopyBufferToImage);
    ENG_VK_LOAD(vkCmdBlitImage);
    ENG_VK_LOAD(vkCreateDescriptorPool);
    ENG_VK_LOAD(vkDestroyDescriptorPool);
    ENG_VK_LOAD(vkCreateDescriptorSetLayout);
    ENG_VK_LOAD(vkDestroyDescriptorSetLayout);
    ENG_VK_LOAD(vkAllocateDescriptorSets);
    ENG_VK_LOAD(vkUpdateDescriptorSets);
    ENG_VK_LOAD(vkCmdBindDescriptorSets);
    ENG_VK_LOAD(vkResetDescriptorPool);
    ENG_VK_LOAD(vkCreateShaderModule);
    ENG_VK_LOAD(vkDestroyShaderModule);
    ENG_VK_LOAD(vkCreatePipelineLayout);
    ENG_VK_LOAD(vkDestroyPipelineLayout);
    ENG_VK_LOAD(vkCreateRenderPass);
    ENG_VK_LOAD(vkDestroyRenderPass);
    ENG_VK_LOAD(vkCreateGraphicsPipelines);
    ENG_VK_LOAD(vkDestroyPipeline);
    ENG_VK_LOAD(vkCreateFramebuffer);
    ENG_VK_LOAD(vkDestroyFramebuffer);
    ENG_VK_LOAD(vkCreateImageView);
    ENG_VK_LOAD(vkDestroyImageView);
    ENG_VK_LOAD(vkCmdBeginRenderPass);
    ENG_VK_LOAD(vkCmdEndRenderPass);
    ENG_VK_LOAD(vkCmdSetViewport);
    ENG_VK_LOAD(vkCmdSetScissor);
    ENG_VK_LOAD(vkCmdBindPipeline);
    ENG_VK_LOAD(vkCmdBindVertexBuffers);
    ENG_VK_LOAD(vkCmdBindIndexBuffer);
    ENG_VK_LOAD(vkCmdDraw);
    ENG_VK_LOAD(vkCmdDrawIndexed);
    ENG_VK_LOAD(vkCmdClearAttachments);
#undef ENG_VK_LOAD
}

std::string vkResultName(VkResult result) {
    switch (result) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_EVENT_SET: return "VK_EVENT_SET";
    case VK_EVENT_RESET: return "VK_EVENT_RESET";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
    case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
    case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_INCOMPATIBLE_DISPLAY_KHR: return "VK_ERROR_INCOMPATIBLE_DISPLAY_KHR";
    case VK_ERROR_VALIDATION_FAILED_EXT: return "VK_ERROR_VALIDATION_FAILED_EXT";
    case VK_ERROR_FRAGMENTATION: return "VK_ERROR_FRAGMENTATION";
    case VK_ERROR_INVALID_DEVICE_ADDRESS_EXT: return "VK_ERROR_INVALID_DEVICE_ADDRESS_EXT";
    case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT:
        return "VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT";
    case VK_THREAD_IDLE_KHR: return "VK_THREAD_IDLE_KHR";
    case VK_THREAD_DONE_KHR: return "VK_THREAD_DONE_KHR";
    case VK_OPERATION_DEFERRED_KHR: return "VK_OPERATION_DEFERRED_KHR";
    case VK_OPERATION_NOT_DEFERRED_KHR: return "VK_OPERATION_NOT_DEFERRED_KHR";
    case VK_PIPELINE_COMPILE_REQUIRED: return "VK_PIPELINE_COMPILE_REQUIRED";
    default: return "VK_ERROR_<código " + std::to_string(static_cast<int>(result)) + ">";
    }
}

} // namespace eng::rhi::vulkan
