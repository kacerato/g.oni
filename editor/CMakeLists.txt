# =============================================================================
# editor/ — núcleo C++ do Native Mobile Editor
#
# CONSUMIDOR de engine/ (nunca o contrário — regra 00-overview.md):
#   - no LINUX (raiz, ENG_BUILD_TESTS): biblioteca + suite Catch2 com
#     backends REAIS (lavapipe/llvmpipe) — o mesmo código que o APK embute;
#   - no APK (android/app/src/main/cpp): add_subdirectory deste arquivo
#     com ENG_BUILD_TESTS=OFF — apenas a biblioteca eng::editor, fontes
#     JAMAIS duplicadas.
#
# jni.h NÃO aparece aqui (fronteira vive em android/app/EditorJni.cpp).
# Depende de eng::rhi::vulkan/gles (registrar fábricas é papel de HOST —
# mesma decisão do runtime Android FASE 7, ADR-036/039).
# =============================================================================

add_library(eng_editor STATIC
    src/EditorDocument.cpp
    src/EditorProtocol.cpp
    src/ProjectZip.cpp
    src/Gizmo.cpp
    src/Inspector.cpp
    src/AssetBrowser.cpp
    src/Viewport.cpp
    src/ViewportRenderer.cpp
    src/EditorHost.cpp
    src/PerfGovernor.cpp
    src/ComponentRegistration.cpp
    src/NiRuntime.cpp
    src/TextureCache.cpp
    src/AnimationAssets.cpp
    src/Diagnostics.cpp
)
add_library(eng::editor ALIAS eng_editor)

target_include_directories(eng_editor PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(eng_editor PUBLIC cxx_std_20)

eng_apply_warnings(eng_editor)
eng_apply_sanitizers(eng_editor)
eng_apply_lto(eng_editor)
# Mesmo contrato das libs do motor: sem exceções/RTTI.
target_compile_options(eng_editor PRIVATE ${ENG_RUNTIME_FLAGS})

target_link_libraries(eng_editor PUBLIC
    eng::niscript
    eng::tick
    eng::scene
    eng::serial
    eng::project
    eng::assets
    eng::image
    eng::reflect
    eng::input
    eng::physics
    eng::animation
    eng::particles
    eng::audio
    eng::render
    eng::log
    eng::core
    eng::rhi
    eng::rhi::vulkan
    eng::rhi::gles)

if(NOT ENG_BUILD_TESTS)
    return()
endif()

# Contrato de vinculação JNI — o TU Android EditorJni.cpp compila
# AQUI no Linux contra o shim hermético tests/jni_shim (jni.h +
# ANativeWindow mínimos, SEM JDK) e EditorTests.cpp prova por
# dlsym(RTLD_DEFAULT, …) que cada `external fun` de EditorJni.kt tem um
# símbolo C limpo. ENABLE_EXPORTS (+rdynamic) põe os símbolos JNIEXPORT na
# tabela dinâmica — a mesma resolução nome→símbolo que o ART faz no device.
# Regressão travada: as 8 funções P4.5 fora de extern "C" (mangling →
# UnsatisfiedLinkError no arranque do C33) agora falham NESTE build.
add_executable(eng_editor_tests
    tests/EditorTests.cpp
    tests/ContractTests.cpp
    tests/PerfTests.cpp
    tests/ProtocolTests.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../android/app/src/main/cpp/EditorJni.cpp")
target_include_directories(eng_editor_tests BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/jni_shim"
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(eng_editor_tests PRIVATE
    eng::editor
    eng::fs
    Catch2::Catch2WithMain)
target_compile_definitions(eng_editor_tests PRIVATE
    GONI_NATIVE_BRIDGE_KT="${CMAKE_CURRENT_SOURCE_DIR}/../android/app/src/main/java/com/goni/app/NativeBridge.kt")
eng_apply_test_policy(eng_editor_tests)
set_target_properties(eng_editor_tests PROPERTIES ENABLE_EXPORTS ON)
add_test(NAME editor COMMAND eng_editor_tests)
set_tests_properties(editor PROPERTIES
    SKIP_RETURN_CODE 4
    LABELS "rhi_hardware"
    # LSan: origens de terceiros (ICD/layers/drivers) — ver
    # tests/drivers_lsan.supp; leaks do MOTOR continuam detectáveis.
    ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/drivers_lsan.supp")
