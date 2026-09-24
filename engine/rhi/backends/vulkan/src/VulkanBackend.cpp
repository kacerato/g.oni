/// Backend Vulkan — probe/initialize/instance/layers/GPU/device/swapchain
///. Recursos e frame estão nos TU irmãos.

#include "eng/rhi/vulkan/VulkanBackend.hpp"

#include <cstring>
#include <utility>

#include "eng/log/Macros.hpp"
#include "eng/rhi/Progress.hpp"

// Surface Android (NDK API — NÃO é JNI; missão §II.4/§IX).
#ifdef __ANDROID__
#include <android/native_window.h>
#endif

ENG_LOG_CATEGORY("rhi.vulkan")

namespace eng::rhi::vulkan {
namespace {

using eng::core::Error;
using eng::core::Result;
using eng::core::StatusCode;

[[nodiscard]] Error vkErr(StatusCode code, std::string_view what, VkResult result) {
    return Error{code, std::string{what} + " (" + vkResultName(result) + ")"};
}

/// Log de validação: mensagens Khronos chegando pelo debug messenger.
VKAPI_ATTR VkBool32 VKAPI_CALL debugMessengerCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void* /*userData*/) {
    const auto level = [&] {
        if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
            return eng::log::LogLevel::Error;
        }
        if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0) {
            return eng::log::LogLevel::Warn;
        }
        return eng::log::LogLevel::Info;
    }();
    eng::log::Logger::get().log(level, "rhi.vulkan.validation", data->pMessage);
    return VK_FALSE;  // não aborta a chamada — reporta e segue
}

[[nodiscard]] std::string versionString(std::uint32_t version) {
    return std::to_string(VK_API_VERSION_MAJOR(version)) + "." +
           std::to_string(VK_API_VERSION_MINOR(version)) + "." +
           std::to_string(VK_API_VERSION_PATCH(version));
}

[[nodiscard]] eng::rhi::DeviceKind deviceKindOf(VkPhysicalDeviceType type) noexcept {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return eng::rhi::DeviceKind::DiscreteGpu;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return eng::rhi::DeviceKind::IntegratedGpu;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return eng::rhi::DeviceKind::VirtualGpu;
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return eng::rhi::DeviceKind::Cpu;
    default: return eng::rhi::DeviceKind::Unknown;
    }
}

[[nodiscard]] const char* surfaceKindExtension(eng::rhi::NativeWindowKind kind) noexcept {
    using K = eng::rhi::NativeWindowKind;
    // Literais (não as macros de vulkan.h): estas exigiriam
    // VK_USE_PLATFORM_* e headers de plataforma — o backend não inclui
    // nenhum (missão §14/§30); a CRIAÇÃO destas surfaces fica com as
    // fases de plataforma.
    switch (kind) {
    case K::Xcb: return "VK_KHR_xcb_surface";
    case K::Xlib: return "VK_KHR_xlib_surface";
    case K::Wayland: return "VK_KHR_wayland_surface";
    case K::Win32: return "VK_KHR_win32_surface";
    case K::Headless: return VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME;
    case K::None: return nullptr;
    case K::Android: return "VK_KHR_android_surface";  // FASE 7 (JNI)
    }
    return nullptr;
}

/// Extensões de instância exigidas pelo tipo de surface.
[[nodiscard]] bool hasExtension(const std::vector<VkExtensionProperties>& available,
                                const char* name) {
    for (const auto& extension : available) {
        if (std::strcmp(extension.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

} // namespace

// =============================================================================
// Fábrica + probe (missão §10/§47)
// =============================================================================

std::unique_ptr<RhiBackend> createBackend() {
    return std::unique_ptr<RhiBackend>(new VulkanBackend());
}

BackendProbe VulkanBackend::probe() {
    // Sem efeitos colaterais: abre, consulta versão, fecha.
    VulkanLibrary library;
    std::string error{};
    if (!library.open(error)) {
        return BackendProbe{eng::rhi::Availability::Unavailable, error};
    }
    std::uint32_t version = 0;
    if (!library.loadGlobalFunctions(version, error)) {
        return BackendProbe{eng::rhi::Availability::Unavailable, error};
    }
    // Loader carregável + versão >= 1.1: nível honesto máximo do probe
    // (criar instance para validar mais pertence a initialize — ADR-036 L2).
    return BackendProbe{eng::rhi::Availability::Detected,
                        "loader presente, instance " + versionString(version)};
}

// =============================================================================
// initialize (missão §17–§20/§23)
// =============================================================================

Result<void> VulkanBackend::initialize(const RendererConfig& config,
                                        RendererCapabilities& outCapabilities) {
    if (initialized_) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::InvalidArgument, "rhi.vulkan: já inicializado"));
    }

    // --- loader + funções globais ------------------------------------------------
    std::string error{};
    if (!library_.open(error)) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::NotFound, "rhi.vulkan: " + error));
    }
    std::uint32_t instanceVersion = 0;
    if (!library_.loadGlobalFunctions(instanceVersion, error)) {
        return eng::core::makeUnexpected(
            makeError(StatusCode::NotSupported, "rhi.vulkan: " + error));
    }

    // --- validação honesta (missão §18) -------------------------------------------
    bool wantValidation = config.enableValidation;
    bool validationLayerFound = false;
    if (wantValidation) {
        std::uint32_t count = 0;
        if (library_.functions().vkEnumerateInstanceLayerProperties(&count, nullptr) ==
            VK_SUCCESS) {
            std::vector<VkLayerProperties> layers(count);
            if (library_.functions().vkEnumerateInstanceLayerProperties(&count, layers.data()) ==
                VK_SUCCESS) {
                for (const auto& layer : layers) {
                    if (std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
                        validationLayerFound = true;
                        break;
                    }
                }
            }
        }
        if (!validationLayerFound) {
            // Pedida e ausente: reporta Unavailable — NÃO mascara.
            capabilities_.validationState = eng::rhi::ValidationState::Unavailable;
            ENG_WARN("rhi.vulkan: validation pedida mas VK_LAYER_KHRONOS_validation ausente");
        }
    } else {
        capabilities_.validationState = eng::rhi::ValidationState::DisabledByConfiguration;
    }

    // --- extensões de instance ------------------------------------------------------
    std::uint32_t extensionCount = 0;
    if (library_.functions().vkEnumerateInstanceExtensionProperties(
            nullptr, &extensionCount, nullptr) != VK_SUCCESS) {
        return eng::core::makeUnexpected(vkErr(StatusCode::Unknown,
                                                "rhi.vulkan: enumeração de extensões falhou",
                                                VK_ERROR_INITIALIZATION_FAILED));
    }
    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    if (library_.functions().vkEnumerateInstanceExtensionProperties(
            nullptr, &extensionCount, availableExtensions.data()) != VK_SUCCESS) {
        return eng::core::makeUnexpected(
            vkErr(StatusCode::Unknown, "rhi.vulkan: enumeração de extensões falhou",
                  VK_ERROR_INITIALIZATION_FAILED));
    }

    std::vector<const char*> extensions{};
    bool debugUtilsAvailable = false;
    if (validationLayerFound) {
        if (hasExtension(availableExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            debugUtilsAvailable = true;
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
    }
    const bool wantSurface = config.surface.isValid();
    if (wantSurface) {
        if (!hasExtension(availableExtensions, VK_KHR_SURFACE_EXTENSION_NAME)) {
            return eng::core::makeUnexpected(
                makeError(StatusCode::NotSupported,
                                         "rhi.vulkan: VK_KHR_surface ausente"));
        }
        extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
        const char* kindExtension = surfaceKindExtension(config.surface.window.kind);
        if (kindExtension == nullptr) {
            return eng::core::makeUnexpected(makeError(
                StatusCode::NotSupported,
                std::string{"rhi.vulkan: surface kind "} +
                    (config.surface.window.kind == eng::rhi::NativeWindowKind::Android
                         ? "Android (FASE 7 — JNI)"
                         : "None/inválida") +
                    " não suportada"));
        }
        if (!hasExtension(availableExtensions, kindExtension)) {
            return eng::core::makeUnexpected(makeError(
                StatusCode::NotSupported,
                std::string{"rhi.vulkan: extensão de surface ausente: "} + kindExtension));
        }
        extensions.push_back(kindExtension);
    }

    // --- instance real (missão §17) ----------------------------------------------------
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = config.applicationName.empty() ? "eng" : config.applicationName.data();
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName = "eng";
    app.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app.apiVersion = VK_API_VERSION_1_1;  // mínimo honesto do backend

    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &app;
    instanceInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    instanceInfo.ppEnabledExtensionNames = extensions.data();
    if (validationLayerFound) {
        static const char* kValidationLayers[] = {"VK_LAYER_KHRONOS_validation"};
        instanceInfo.enabledLayerCount = 1;
        instanceInfo.ppEnabledLayerNames = kValidationLayers;
    }

    VkResult result = library_.functions().vkCreateInstance(&instanceInfo, nullptr, &instance_);
    if (result != VK_SUCCESS) {
        eng::rhi::reportProgress(eng::rhi::rhi_stage::Instance, "failed",
                                 std::string{vkResultName(result)}.c_str());
        return eng::core::makeUnexpected(
            vkErr(StatusCode::NotSupported, "rhi.vulkan: vkCreateInstance", result));
    }
    library_.loadInstanceFunctions(instance_);
    // Micro-mark — VkInstance criada.
    eng::rhi::reportProgress(eng::rhi::rhi_stage::Instance, "ok", "VkInstance");
    ENG_INFO("rhi.vulkan: VkInstance criada (loader {})", versionString(instanceVersion));

    // --- debug messenger (quando habilitado) -----------------------------------------------
    if (validationLayerFound && debugUtilsAvailable) {
        VkDebugUtilsMessengerCreateInfoEXT messengerInfo{};
        messengerInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        messengerInfo.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        messengerInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        messengerInfo.pfnUserCallback = &debugMessengerCallback;
        result = library_.functions().vkCreateDebugUtilsMessengerEXT(
            instance_, &messengerInfo, nullptr, &messenger_);
        if (result != VK_SUCCESS) {
            // Pedida, presente, mas falhou ao inicializar.
            capabilities_.validationState = eng::rhi::ValidationState::FailedToInitialize;
            ENG_ERROR("rhi.vulkan: debug messenger falhou ({})", vkResultName(result));
            messenger_ = VK_NULL_HANDLE;
        } else {
            capabilities_.validationState = eng::rhi::ValidationState::Enabled;
            stats_.validationLayerActive = true;
            ENG_INFO("rhi.vulkan: validation layer Khronos ATIVA");
        }
    }

    // --- surface (Headless no Linux — auditoria F5 §3; Android na FASE 7) --------------------
    if (wantSurface) {
        switch (config.surface.window.kind) {
        case eng::rhi::NativeWindowKind::Headless: {
            VkHeadlessSurfaceCreateInfoEXT surfaceInfo{};
            surfaceInfo.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT;
            result = library_.functions().vkCreateHeadlessSurfaceEXT(instance_, &surfaceInfo,
                                                                    nullptr, &surface_);
            if (result != VK_SUCCESS) {
                return eng::core::makeUnexpected(vkErr(
                    StatusCode::NotSupported, "rhi.vulkan: vkCreateHeadlessSurfaceEXT", result));
            }
            break;
        }
#ifdef __ANDROID__
        case eng::rhi::NativeWindowKind::Android: {
            // Surface REAL a partir da janela nativa entregue pela camada
            // Android (missão §IX). O handle é opaco na abstraction — aqui
            // é o ANativeWindow* adquirido pelo runtime.
            VkAndroidSurfaceCreateInfoKHR surfaceInfo{};
            surfaceInfo.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
            surfaceInfo.window =
                static_cast<ANativeWindow*>(const_cast<void*>(config.surface.window.handle));
            result = library_.functions().vkCreateAndroidSurfaceKHR(instance_, &surfaceInfo,
                                                                     nullptr, &surface_);
            if (result != VK_SUCCESS) {
                return eng::core::makeUnexpected(vkErr(StatusCode::NotSupported,
                                                       "rhi.vulkan: vkCreateAndroidSurfaceKHR",
                                                       result));
            }
            break;
        }
#endif
        default:
            return eng::core::makeUnexpected(makeError(
                StatusCode::NotSupported,
                "rhi.vulkan: surface kind não suportada neste build "
                "(Xcb/Wayland/Win32 vêm com as fases de desktop)"));
        }
        hasSurface_ = true;
        // Micro-mark — VkSurfaceKHR pronta.
        eng::rhi::reportProgress(eng::rhi::rhi_stage::Surface, "ok",
                                 "VkSurfaceKHR");
    }

    // --- seleção de GPU (missão §19: TODAS enumeradas, motivo por rejeição) --------------------
    std::uint32_t deviceCount = 0;
    result = library_.functions().vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr);
    if (result != VK_SUCCESS || deviceCount == 0) {
        return eng::core::makeUnexpected(vkErr(StatusCode::NotSupported,
                                               "rhi.vulkan: nenhuma GPU física "
                                               "(vkEnumeratePhysicalDevices)",
                                               result != VK_SUCCESS ? result : VK_ERROR_UNKNOWN));
    }
    std::vector<VkPhysicalDevice> devices(deviceCount);
    result = library_.functions().vkEnumeratePhysicalDevices(instance_, &deviceCount,
                                                             devices.data());
    if (result != VK_SUCCESS) {
        return eng::core::makeUnexpected(
            vkErr(StatusCode::Unknown, "rhi.vulkan: vkEnumeratePhysicalDevices", result));
    }
    stats_.gpusEnumerated = deviceCount;

    // --- device real (missão §20) + swapchain exigida quando há surface -------------------------
    std::string rejectionReasons{};
    RendererCapabilities caps{};  // preenchida na GPU selecionada (missão §9)
    for (const VkPhysicalDevice candidate : devices) {
        VkPhysicalDeviceProperties2 properties2{};
        properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        VkPhysicalDeviceDriverProperties driverProperties{};
        driverProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
        properties2.pNext = &driverProperties;
        library_.functions().vkGetPhysicalDeviceProperties2(candidate, &properties2);

        // Requisito: fila de graphics.
        std::uint32_t familyCount = 0;
        library_.functions().vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount,
                                                                      nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        library_.functions().vkGetPhysicalDeviceQueueFamilyProperties(
            candidate, &familyCount, families.data());

        std::uint32_t graphicsFamily = 0xFFFFFFFFu;
        std::uint32_t presentFamily = 0xFFFFFFFFu;
        for (std::uint32_t family = 0; family < familyCount; ++family) {
            if ((families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 &&
                graphicsFamily == 0xFFFFFFFFu) {
                graphicsFamily = family;
            }
            if (wantSurface && presentFamily == 0xFFFFFFFFu) {
                VkBool32 supported = VK_FALSE;
                if (library_.functions().vkGetPhysicalDeviceSurfaceSupportKHR(
                        candidate, family, surface_, &supported) == VK_SUCCESS &&
                    supported == VK_TRUE) {
                    presentFamily = family;
                }
            }
        }

        // Requisito: extensão de swapchain quando apresentando.
        bool swapchainExtensionOk = !wantSurface;
        if (wantSurface) {
            std::uint32_t deviceExtensionCount = 0;
            library_.functions().vkEnumerateDeviceExtensionProperties(
                candidate, nullptr, &deviceExtensionCount, nullptr);
            std::vector<VkExtensionProperties> deviceExtensions(deviceExtensionCount);
            library_.functions().vkEnumerateDeviceExtensionProperties(
                candidate, nullptr, &deviceExtensionCount, deviceExtensions.data());
            swapchainExtensionOk =
                hasExtension(deviceExtensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        }

        const bool suitable = graphicsFamily != 0xFFFFFFFFu && (!wantSurface ||
                               (presentFamily != 0xFFFFFFFFu && swapchainExtensionOk));
        if (!suitable) {
            stats_.gpusRejected++;
            rejectionReasons += std::string{properties2.properties.deviceName} + ": ";
            if (graphicsFamily == 0xFFFFFFFFu) {
                rejectionReasons += "sem fila de graphics; ";
            } else if (wantSurface && presentFamily == 0xFFFFFFFFu) {
                rejectionReasons += "sem suporte de present na surface; ";
            } else {
                rejectionReasons += "sem VK_KHR_swapchain; ";
            }
            continue;
        }

        // Selecionada: valida e cria o device.
        physicalDevice_ = candidate;
        graphicsFamily_ = graphicsFamily;
        presentFamily_ = presentFamily;

        const float queuePriority = 1.0f;
        std::vector<VkDeviceQueueCreateInfo> queueInfos{};
        VkDeviceQueueCreateInfo graphicsQueueInfo{};
        graphicsQueueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        graphicsQueueInfo.queueFamilyIndex = graphicsFamily;
        graphicsQueueInfo.queueCount = 1;
        graphicsQueueInfo.pQueuePriorities = &queuePriority;
        queueInfos.push_back(graphicsQueueInfo);
        if (wantSurface && presentFamily != graphicsFamily) {
            VkDeviceQueueCreateInfo presentQueueInfo{};
            presentQueueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            presentQueueInfo.queueFamilyIndex = presentFamily;
            presentQueueInfo.queueCount = 1;
            presentQueueInfo.pQueuePriorities = &queuePriority;
            queueInfos.push_back(presentQueueInfo);
        }

        std::vector<const char*> deviceExtensions{};
        if (wantSurface) {
            deviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        }

        VkPhysicalDeviceFeatures features{};  // nenhuma exigida
        VkDeviceCreateInfo deviceInfo{};
        deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        deviceInfo.queueCreateInfoCount = static_cast<std::uint32_t>(queueInfos.size());
        deviceInfo.pQueueCreateInfos = queueInfos.data();
        deviceInfo.enabledExtensionCount = static_cast<std::uint32_t>(deviceExtensions.size());
        deviceInfo.ppEnabledExtensionNames = deviceExtensions.data();
        deviceInfo.pEnabledFeatures = &features;

        result = library_.functions().vkCreateDevice(candidate, &deviceInfo, nullptr, &device_);
        if (result != VK_SUCCESS) {
            physicalDevice_ = VK_NULL_HANDLE;
            stats_.gpusRejected++;
            rejectionReasons += std::string{properties2.properties.deviceName} + ": vkCreateDevice " +
                                vkResultName(result) + "; ";
            graphicsFamily_ = 0xFFFFFFFFu;
            presentFamily_ = 0xFFFFFFFFu;
            continue;
        }
        library_.loadDeviceFunctions(device_);
        library_.functions().vkGetDeviceQueue(device_, graphicsFamily_, 0, &graphicsQueue_);
        presentQueue_ = graphicsQueue_;
        // Micro-mark — device lógico + queues prontos.
        eng::rhi::reportProgress(eng::rhi::rhi_stage::Device, "ok",
                                 properties2.properties.deviceName);
        if (wantSurface && presentFamily != graphicsFamily) {
            library_.functions().vkGetDeviceQueue(device_, presentFamily_, 0, &presentQueue_);
        }
        library_.functions().vkGetPhysicalDeviceMemoryProperties(candidate, &memoryProperties_);
        ENG_INFO("rhi.vulkan: GPU selecionada: {} ({} rejeitadas)", properties2.properties.deviceName,
                 stats_.gpusRejected);

        // --- capabilities REAIS (missão §9) — da GPU selecionada ------------------------------
        caps = RendererCapabilities{};
        caps.backendName = "Vulkan";
        caps.apiVersion = versionString(properties2.properties.apiVersion);
        caps.device.name = properties2.properties.deviceName;
        caps.device.apiVersion = versionString(properties2.properties.apiVersion);
        caps.device.kind = deviceKindOf(properties2.properties.deviceType);
        caps.device.vendor = std::to_string(properties2.properties.vendorID);
        caps.device.driver = std::string{driverProperties.driverName} + " " +
                             driverProperties.driverInfo;
        caps.softwareRendering =
            properties2.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ||
            driverProperties.driverID == VK_DRIVER_ID_MESA_LLVMPIPE ||
            driverProperties.driverID == VK_DRIVER_ID_GOOGLE_SWIFTSHADER;
        const VkPhysicalDeviceLimits& limits = properties2.properties.limits;
        caps.maxTextureSize = limits.maxImageDimension2D;
        caps.maxVertexAttributes = limits.maxVertexInputAttributes;
        caps.maxColorAttachments = limits.maxColorAttachments;
        caps.maxUniformBufferSize = limits.maxUniformBufferRange;
        caps.instancing = true;   // core desde 1.0 (draw não-instanciado usa o mesmo caminho)
        caps.compute = (families[graphicsFamily].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
        caps.multisample = limits.framebufferColorSampleCounts > 1;
        caps.wireframe = false;  // fillModeNonSolid não habilitada no device
        for (std::uint8_t formatValue = 1;
             formatValue <= static_cast<std::uint8_t>(eng::rhi::Format::D24UnormS8Uint);
             ++formatValue) {
            const VkFormat vkFormat = toVkFormat(static_cast<eng::rhi::Format>(formatValue));
            VkFormatProperties formatProperties{};
            // vkGetPhysicalDeviceFormatProperties é instance-level clássico:
            using FormatPropsFn = void(VKAPI_PTR*)(VkPhysicalDevice, VkFormat,
                                                   VkFormatProperties*);
            auto formatProps = reinterpret_cast<FormatPropsFn>(
                library_.functions().vkGetInstanceProcAddr(
                    instance_, "vkGetPhysicalDeviceFormatProperties"));
            if (formatProps != nullptr) {
                formatProps(candidate, vkFormat, &formatProperties);
                if (formatProperties.optimalTilingFeatures != 0 ||
                    formatProperties.linearTilingFeatures != 0) {
                    caps.supportedFormats.push_back(static_cast<eng::rhi::Format>(formatValue));
                }
            }
        }
        caps.presentation = wantSurface;  // surface criada/validada acima
        caps.validationState = capabilities_.validationState;  // honesto
        stats_.rejectedGpuReasons = rejectionReasons;
        break;
    }

    if (physicalDevice_ == VK_NULL_HANDLE) {
        return eng::core::makeUnexpected(makeError(
            StatusCode::NotSupported,
            "rhi.vulkan: nenhuma GPU atende aos requisitos — " + rejectionReasons));
    }

    // --- pool de comandos + frames in flight (missão §22) ---------------------------------------
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = graphicsFamily_;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    result = library_.functions().vkCreateCommandPool(device_, &poolInfo, nullptr,
                                                       &commandPool_);
    if (result != VK_SUCCESS) {
        return eng::core::makeUnexpected(
            vkErr(StatusCode::Unknown, "rhi.vulkan: vkCreateCommandPool", result));
    }

    const std::uint32_t framesInFlight =
        config.framesInFlight == 0 ? 1u
                                   : (config.framesInFlight > 2u ? 2u : config.framesInFlight);
    frameSlots_.resize(framesInFlight);
    for (auto& slot : frameSlots_) {
        VkCommandBufferAllocateInfo commandInfo{};
        commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        commandInfo.commandPool = commandPool_;
        commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandInfo.commandBufferCount = 1;
        result = library_.functions().vkAllocateCommandBuffers(device_, &commandInfo,
                                                                &slot.command);
        if (result != VK_SUCCESS) {
            return eng::core::makeUnexpected(vkErr(
                StatusCode::Unknown, "rhi.vulkan: vkAllocateCommandBuffers", result));
        }

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        result = library_.functions().vkCreateFence(device_, &fenceInfo, nullptr, &slot.fence);
        if (result != VK_SUCCESS) {
            return eng::core::makeUnexpected(
                vkErr(StatusCode::Unknown, "rhi.vulkan: vkCreateFence", result));
        }

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        result = library_.functions().vkCreateSemaphore(device_, &semaphoreInfo, nullptr,
                                                        &slot.imageAvailable);
        if (result != VK_SUCCESS) {
            return eng::core::makeUnexpected(vkErr(
                StatusCode::Unknown, "rhi.vulkan: vkCreateSemaphore (imageAvailable)", result));
        }
        result = library_.functions().vkCreateSemaphore(device_, &semaphoreInfo, nullptr,
                                                        &slot.renderFinished);
        if (result != VK_SUCCESS) {
            return eng::core::makeUnexpected(vkErr(
                StatusCode::Unknown, "rhi.vulkan: vkCreateSemaphore (renderFinished)", result));
        }
    }

    // --- swapchain (missão §23) — quando há surface --------------------------------------------
    if (hasSurface_) {
        const auto created = createSwapchain(config.surface.width, config.surface.height);
        if (!created) {
            return eng::core::makeUnexpected(created.error());
        }
        // Micro-mark — swapchain pronta (último sub-passo da
        // janela resume→surface antes do primeiro frame).
        eng::rhi::reportProgress(eng::rhi::rhi_stage::Swapchain, "ok",
                                 "createSwapchain");
    }

    // --- texturas (evolução): layout de descriptor set + pool compartilhados ---------
    // Set 0 com 1 combined image sampler (fragment). TODOS os pipelines
    // recebem este layout — shaders que não amostram ignoram o binding
    // (legal em Vulkan; unifica o modelo de bind).
    VkDescriptorSetLayoutBinding samplerBinding{};
    samplerBinding.binding = 0;
    samplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    samplerBinding.descriptorCount = 1;
    samplerBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    samplerBinding.pImmutableSamplers = nullptr;
    VkDescriptorSetLayoutCreateInfo setLayoutInfo{};
    setLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setLayoutInfo.bindingCount = 1;
    setLayoutInfo.pBindings = &samplerBinding;
    result = library_.functions().vkCreateDescriptorSetLayout(
        device_, &setLayoutInfo, nullptr, &textureSetLayout_);
    if (result != VK_SUCCESS) {
        return eng::core::makeUnexpected(vkErr(
            StatusCode::Unknown, "rhi.vulkan: vkCreateDescriptorSetLayout (textura)", result));
    }

    // --- uniforms do frame (P3 §2): set 1 com UBO DINÂMICO -------------------------
    // Cada frame-slot tem seu PRÓPRIO buffer HOST_VISIBLE (mapeado
    // persistentemente) e um descriptor set escrito UMA vez — a região é
    // escolhida por dynamic offset no momento do bind. A escrita acontece
    // ANTES da submissão do frame e é lida apenas por ele: sem hazard.
    VkDescriptorSetLayoutBinding uniformBinding{};
    uniformBinding.binding = 0;
    uniformBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    uniformBinding.descriptorCount = 1;
    uniformBinding.stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    uniformBinding.pImmutableSamplers = nullptr;
    VkDescriptorSetLayoutCreateInfo uniformLayoutInfo{};
    uniformLayoutInfo.sType =
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    uniformLayoutInfo.bindingCount = 1;
    uniformLayoutInfo.pBindings = &uniformBinding;
    result = library_.functions().vkCreateDescriptorSetLayout(
        device_, &uniformLayoutInfo, nullptr, &uniformSetLayout_);
    if (result != VK_SUCCESS) {
        library_.functions().vkDestroyDescriptorSetLayout(device_, textureSetLayout_,
                                                          nullptr);
        textureSetLayout_ = VK_NULL_HANDLE;
        return eng::core::makeUnexpected(vkErr(
            StatusCode::Unknown, "rhi.vulkan: vkCreateDescriptorSetLayout (uniform)",
            result));
    }

    // Pool: sets NUNCA são liberados individualmente (cache por par
    // textura+sampler; invalidação = destroy do pool inteiro quando vazio de
    // uso — nesta escala o pool é destruído junto com o backend). 256 pares é
    // folga honesta para sprites/UI; exaustão = erro preciso (não silêncio).
    // +1 UBO dinâmico por frame-slot (P3 §2 — luzes 2D).
    const VkDescriptorPoolSize poolSizes[2] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 256},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
         static_cast<std::uint32_t>(frameSlots_.size())},
    };
    VkDescriptorPoolCreateInfo descriptorPoolInfo{};
    descriptorPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    descriptorPoolInfo.maxSets = 256 + static_cast<std::uint32_t>(frameSlots_.size());
    descriptorPoolInfo.poolSizeCount = 2;
    descriptorPoolInfo.pPoolSizes = poolSizes;
    result = library_.functions().vkCreateDescriptorPool(
        device_, &descriptorPoolInfo, nullptr, &textureDescriptorPool_);
    if (result != VK_SUCCESS) {
        library_.functions().vkDestroyDescriptorSetLayout(device_, textureSetLayout_,
                                                           nullptr);
        textureSetLayout_ = VK_NULL_HANDLE;
        library_.functions().vkDestroyDescriptorSetLayout(device_, uniformSetLayout_,
                                                          nullptr);
        uniformSetLayout_ = VK_NULL_HANDLE;
        return eng::core::makeUnexpected(vkErr(
            StatusCode::Unknown, "rhi.vulkan: vkCreateDescriptorPool (textura)", result));
    }

    // UBO por frame-slot: buffer + memória mapeada + descriptor escrito 1x.
    for (auto& slot : frameSlots_) {
        VkBufferCreateInfo uniformInfo{};
        uniformInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        uniformInfo.size = kMaxFrameUniformData;
        uniformInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        uniformInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        result = library_.functions().vkCreateBuffer(device_, &uniformInfo, nullptr,
                                                    &slot.uniformBuffer);
        if (result != VK_SUCCESS) {
            return eng::core::makeUnexpected(vkErr(
                StatusCode::Unknown, "rhi.vulkan: vkCreateBuffer (uniform)", result));
        }
        VkMemoryRequirements uniformRequirements{};
        library_.functions().vkGetBufferMemoryRequirements(
            device_, slot.uniformBuffer, &uniformRequirements);
        auto uniformType = pickMemoryType(
            uniformRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            "uniform do frame");
        if (!uniformType) {
            return eng::core::makeUnexpected(uniformType.error());
        }
        VkMemoryAllocateInfo uniformAlloc{};
        uniformAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        uniformAlloc.allocationSize = uniformRequirements.size;
        uniformAlloc.memoryTypeIndex = uniformType.value();
        result = library_.functions().vkAllocateMemory(device_, &uniformAlloc, nullptr,
                                                       &slot.uniformMemory);
        if (result != VK_SUCCESS) {
            return eng::core::makeUnexpected(vkErr(
                StatusCode::OutOfMemory, "rhi.vulkan: vkAllocateMemory (uniform)", result));
        }
        result = library_.functions().vkBindBufferMemory(
            device_, slot.uniformBuffer, slot.uniformMemory, 0);
        if (result != VK_SUCCESS) {
            return eng::core::makeUnexpected(vkErr(
                StatusCode::Unknown, "rhi.vulkan: vkBindBufferMemory (uniform)", result));
        }
        result = library_.functions().vkMapMemory(device_, slot.uniformMemory, 0,
                                                  kMaxFrameUniformData, 0,
                                                  &slot.uniformMapped);
        if (result != VK_SUCCESS || slot.uniformMapped == nullptr) {
            return eng::core::makeUnexpected(vkErr(
                StatusCode::Unknown, "rhi.vulkan: vkMapMemory (uniform)", result));
        }
        VkDescriptorSetAllocateInfo uniformSetInfo{};
        uniformSetInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        uniformSetInfo.descriptorPool = textureDescriptorPool_;
        uniformSetInfo.descriptorSetCount = 1;
        uniformSetInfo.pSetLayouts = &uniformSetLayout_;
        result = library_.functions().vkAllocateDescriptorSets(
            device_, &uniformSetInfo, &slot.uniformDescriptor);
        if (result != VK_SUCCESS) {
            return eng::core::makeUnexpected(vkErr(
                StatusCode::OutOfMemory,
                "rhi.vulkan: descriptor set de uniform (pool exaurido?)", result));
        }
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = slot.uniformBuffer;
        bufferInfo.offset = 0;
        bufferInfo.range = kMaxFrameUniformData;
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = slot.uniformDescriptor;
        write.dstBinding = 0;
        write.dstArrayElement = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        write.pBufferInfo = &bufferInfo;
        library_.functions().vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    }

    initialized_ = true;
    capabilities_ = caps;  // caps REAIS preenchidas na seleção da GPU
    capabilities_.presentation = hasSurface_;
    outCapabilities = capabilities_;
    ENG_INFO("rhi.vulkan: inicializado ({} frames in flight, {})",
             framesInFlight, hasSurface_ ? "com surface" : "device-only");
    return {};
}

const RendererCapabilities& VulkanBackend::capabilities() const {
    return capabilities_;
}

// =============================================================================
// Destruição total (ownership — ADR-035)
// =============================================================================

void VulkanBackend::destroyAll() noexcept {
    if (device_ != VK_NULL_HANDLE) {
        library_.functions().vkDeviceWaitIdle(device_);
        // esperado: esperas + semáforos + framebuffers + render pass
        destroySwapchain();
        if (renderPass_ != VK_NULL_HANDLE) {
            library_.functions().vkDestroyRenderPass(device_, renderPass_, nullptr);
            renderPass_ = VK_NULL_HANDLE;
        }
        for (auto& [format, pass] : deviceOnlyRenderPasses_) {
            library_.functions().vkDestroyRenderPass(device_, pass, nullptr);
        }
        deviceOnlyRenderPasses_.clear();
        for (auto& slot : frameSlots_) {
            if (slot.fence != VK_NULL_HANDLE) {
                library_.functions().vkDestroyFence(device_, slot.fence, nullptr);
            }
            if (slot.imageAvailable != VK_NULL_HANDLE) {
                library_.functions().vkDestroySemaphore(device_, slot.imageAvailable, nullptr);
            }
            if (slot.renderFinished != VK_NULL_HANDLE) {
                library_.functions().vkDestroySemaphore(device_, slot.renderFinished, nullptr);
            }
        }
        for (auto& slot : frameSlots_) {
            if (slot.uniformMapped != nullptr) {
                library_.functions().vkUnmapMemory(device_, slot.uniformMemory);
                slot.uniformMapped = nullptr;
            }
            if (slot.uniformBuffer != VK_NULL_HANDLE) {
                library_.functions().vkDestroyBuffer(device_, slot.uniformBuffer,
                                                     nullptr);
                slot.uniformBuffer = VK_NULL_HANDLE;
            }
            if (slot.uniformMemory != VK_NULL_HANDLE) {
                library_.functions().vkFreeMemory(device_, slot.uniformMemory, nullptr);
                slot.uniformMemory = VK_NULL_HANDLE;
            }
        }
        frameSlots_.clear();
        pendingPresents_.clear();
        if (commandPool_ != VK_NULL_HANDLE) {
            library_.functions().vkDestroyCommandPool(device_, commandPool_, nullptr);
            commandPool_ = VK_NULL_HANDLE;
        }
        // Recursos pendentes: destruição em cascata — o entry é
        // removido da tabela e o objeto Vulkan destruído.
        for (auto entry : buffers_.drainAll()) {
            library_.functions().vkDestroyBuffer(device_, entry.buffer, nullptr);
            library_.functions().vkFreeMemory(device_, entry.memory, nullptr);
        }
        for (auto entry : shaders_.drainAll()) {
            library_.functions().vkDestroyShaderModule(device_, entry.vertex, nullptr);
            library_.functions().vkDestroyShaderModule(device_, entry.fragment, nullptr);
        }
        for (auto entry : pipelines_.drainAll()) {
            library_.functions().vkDestroyPipeline(device_, entry.pipeline, nullptr);
            library_.functions().vkDestroyPipelineLayout(device_, entry.layout, nullptr);
        }
        for (auto entry : textures_.drainAll()) {
            library_.functions().vkDestroyImageView(device_, entry.view, nullptr);
            library_.functions().vkDestroyImage(device_, entry.image, nullptr);
            library_.functions().vkFreeMemory(device_, entry.memory, nullptr);
        }
        for (auto entry : samplers_.drainAll()) {
            library_.functions().vkDestroySampler(device_, entry.sampler, nullptr);
        }
        textureSetCache_.clear();
        if (textureDescriptorPool_ != VK_NULL_HANDLE) {
            library_.functions().vkDestroyDescriptorPool(device_, textureDescriptorPool_,
                                                          nullptr);
            textureDescriptorPool_ = VK_NULL_HANDLE;
        }
        if (textureSetLayout_ != VK_NULL_HANDLE) {
            library_.functions().vkDestroyDescriptorSetLayout(device_, textureSetLayout_,
                                                                nullptr);
            textureSetLayout_ = VK_NULL_HANDLE;
        }
        if (uniformSetLayout_ != VK_NULL_HANDLE) {
            library_.functions().vkDestroyDescriptorSetLayout(device_, uniformSetLayout_,
                                                               nullptr);
            uniformSetLayout_ = VK_NULL_HANDLE;
        }
        library_.functions().vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (surface_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
        library_.functions().vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    if (messenger_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
        library_.functions().vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
        messenger_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE) {
        library_.functions().vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
    initialized_ = false;
    hasSurface_ = false;
    surfaceLost_ = false;
    swapchainSuboptimal_ = false;
}

VulkanBackend::~VulkanBackend() {
    destroyAll();
}

} // namespace eng::rhi::vulkan
