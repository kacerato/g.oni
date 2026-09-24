# =============================================================================
# eng::rhi::vulkan — backend Vulkan REAL
#
# - Headers: FetchContent Khronos/Vulkan-Headers v1.4.309 (hash fixado —
#   mesmo padrão de Catch2/nlohmann, ADR-030). SEM dependência de SDK local.
# - Loader: dlopen em runtime (VulkanLoader.cpp) — NENHUM link com libvulkan;
#   probe reporta ausência honestamente (Unavailable) sem falhar o build.
# - Este target é linkado pelo EXECUTÁVEL final (junto a eng::rhi) — o módulo
#   interface nunca conhece este diretório (regra 00-overview.md).
# =============================================================================
include(FetchContent)
FetchContent_Declare(VulkanHeaders
    URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.309.tar.gz
    URL_HASH SHA256=437925ada160d86ed763d29dcb9318c1bb0d024d7deaf77bc7c170b8eb6b6f10
)
FetchContent_MakeAvailable(VulkanHeaders)

add_library(eng_rhi_vulkan STATIC
    src/VulkanLoader.cpp
    src/VulkanBackend.cpp
    src/VulkanResources.cpp
    src/VulkanFrame.cpp
)
add_library(eng::rhi::vulkan ALIAS eng_rhi_vulkan)

target_include_directories(eng_rhi_vulkan PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(eng_rhi_vulkan PUBLIC cxx_std_20)

# Arestas: rhi (PUBLIC — implementa eng::rhi::RhiBackend), Vulkan-Headers
# (headers de vulkan/), dl (dlopen/dlsym). eng::core/log chegam
# transitivamente via eng::rhi (PUBLIC core / PRIVATE log — a ABI exposta
# não inclui tipos de log, então PRIVATE basta aqui).
target_link_libraries(eng_rhi_vulkan
    PUBLIC eng::rhi Vulkan-Headers
    PRIVATE ${CMAKE_DL_LIBS} eng::log)

eng_apply_warnings(eng_rhi_vulkan)
eng_apply_sanitizers(eng_rhi_vulkan)
eng_apply_lto(eng_rhi_vulkan)
target_compile_options(eng_rhi_vulkan PRIVATE ${ENG_RUNTIME_FLAGS})

if(ENG_BUILD_TESTS)
    add_executable(eng_rhi_vulkan_tests
        tests/VulkanTests.cpp
    )
    target_include_directories(eng_rhi_vulkan_tests PRIVATE
        "${CMAKE_SOURCE_DIR}/tests/shaders")
    target_link_libraries(eng_rhi_vulkan_tests
        PRIVATE eng_rhi_vulkan Catch2::Catch2WithMain)
    eng_apply_test_policy(eng_rhi_vulkan_tests)
    # LABELS: hardware tests distinguem PASS/SKIPPED/UNAVAILABLE/FAILED
    # (missão §29/§41) — sem loader/ICD o teste SKIPA com motivo, nunca
    # falha por ausência de GPU.
    add_test(NAME rhi_vulkan COMMAND eng_rhi_vulkan_tests)
    set_tests_properties(rhi_vulkan PROPERTIES
    SKIP_RETURN_CODE 4
    LABELS "rhi;rhi_hardware"
        # LSan: alocações internas do ICD lavapipe e da layer Khronos não são
        # liberadas em vkDestroyInstance (comportamento conhecido de
        # terceiros). A detecção de leaks PERMANECE ATIVA para o código do
        # motor — ver tests/lavapipe_lsan.supp. O ICD/layers são
        # selecionados por env (VK_ICD_FILENAMES/VK_LAYER_PATH) fora daqui.
        ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/lavapipe_lsan.supp")
endif()
