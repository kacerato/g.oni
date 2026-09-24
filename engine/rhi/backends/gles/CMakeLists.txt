# =============================================================================
# eng::rhi::gles — backend OpenGL ES REAL
#
# - Headers: FetchContent Khronos EGL-Registry + OpenGL-Registry, COMMITS
#   fixados com URL_HASH (os repos não publicam tags estáveis). Mesmo
#   padrão hermético de Vulkan-Headers.
# - Loader: dlopen(libEGL/libGLESv2) em runtime — NENHUM link com as libs;
#   probe reporta ausência honestamente (Unavailable) sem falhar o build.
# =============================================================================
include(FetchContent)
FetchContent_Declare(EglRegistry
    URL https://codeload.github.com/KhronosGroup/EGL-Registry/tar.gz/5961a7fe64cf8a126890ced6f13d69e0a1e1b83e
    URL_HASH SHA256=b360a7542cb51a74ad87f9a423903c1a88d9739167744da9397eb7190ebe83d0
)
FetchContent_MakeAvailable(EglRegistry)
FetchContent_Declare(OpenGLRegistry
    URL https://codeload.github.com/KhronosGroup/OpenGL-Registry/tar.gz/1cdd228e34966dd6b95bd203e9f84faba0f371a1
    URL_HASH SHA256=d8ac2e70b4a8173d4e30ceb726eb8b3137883d459e4666983eb1d2444483c62c
)
FetchContent_MakeAvailable(OpenGLRegistry)

add_library(eng_rhi_gles STATIC
    src/GlesLoader.cpp
    src/GlesBackend.cpp
)
add_library(eng::rhi::gles ALIAS eng_rhi_gles)

target_include_directories(eng_rhi_gles PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    # Headers Khronos: api/ contém EGL/ e KHR/ (EGL-Registry) e GLES3/ etc.
    "${eglregistry_SOURCE_DIR}/api"
    "${openglregistry_SOURCE_DIR}/api")
target_compile_features(eng_rhi_gles PUBLIC cxx_std_20)

# Arestas: rhi (PUBLIC — implementa eng::rhi::RhiBackend) + dl. core/log
# chegam transitivamente via eng::rhi.
target_link_libraries(eng_rhi_gles
    PUBLIC eng::rhi
    PRIVATE ${CMAKE_DL_LIBS} eng::log)

eng_apply_warnings(eng_rhi_gles)
eng_apply_sanitizers(eng_rhi_gles)
eng_apply_lto(eng_rhi_gles)
target_compile_options(eng_rhi_gles PRIVATE ${ENG_RUNTIME_FLAGS})

if(ENG_BUILD_TESTS)
    add_executable(eng_rhi_gles_tests
        tests/GlesTests.cpp
    )
    target_include_directories(eng_rhi_gles_tests PRIVATE
        "${CMAKE_SOURCE_DIR}/tests/shaders")
    target_compile_definitions(eng_rhi_gles_tests PRIVATE
        ENG_SHADERS_DIR="${CMAKE_SOURCE_DIR}/tests/shaders")
    target_link_libraries(eng_rhi_gles_tests
        PRIVATE eng_rhi_gles Catch2::Catch2WithMain)
    # Teste de PARIDADE Vulkan/GLES (missão §40): mesmo triangle, mesmos
    # vértices, mesma intenção — linka o backend Vulkan quando existir.
    if(TARGET eng_rhi_vulkan)
        target_link_libraries(eng_rhi_gles_tests PRIVATE eng_rhi_vulkan)
        target_compile_definitions(eng_rhi_gles_tests PRIVATE ENG_HAVE_PARITY_VULKAN=1)
    endif()
    eng_apply_test_policy(eng_rhi_gles_tests)
    add_test(NAME rhi_gles COMMAND eng_rhi_gles_tests)
    set_tests_properties(rhi_gles PROPERTIES
    SKIP_RETURN_CODE 4
    LABELS "rhi;rhi_hardware"
        # LSan: arenas JIT do llvmpipe (módulo desconhecido p/ o LSan) e o
        # próprio runtime do UBSan retêm alocações — ver
        # tests/mesa_lsan.supp. Detecção de leaks do MOTOR permanece ativa.
        ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/mesa_lsan.supp")
endif()
