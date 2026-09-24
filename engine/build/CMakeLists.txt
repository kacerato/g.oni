# =============================================================================
# engine/build — Build & Export Pipeline
#
# Projeto → manifest determinístico → dep-graph/scan → validação → cook
# (envelope GONI) → cache conteúdo-endereçado → bundle verificado → export.
#
# Dados APENAS (missão: engine × dados separados — nunca recompila o motor,
# nunca conhece um projeto específico). Depende de project/assets/serial/
# niscript (validação de scripts) — NÃO conhece rhi/android/editor/scene
# (o scan de referências é texto JSON, agnóstico).
# =============================================================================
add_library(eng_build STATIC
    src/Build.cpp
)
add_library(eng::build ALIAS eng_build)

target_include_directories(eng_build PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(eng_build PUBLIC cxx_std_20)
eng_apply_warnings(eng_build)
eng_apply_sanitizers(eng_build)
eng_apply_lto(eng_build)
# Mesmo contrato das libs do motor: sem exceções/RTTI.
target_compile_options(eng_build PRIVATE ${ENG_RUNTIME_FLAGS})

target_link_libraries(eng_build PUBLIC
    eng::core
    eng::fs
    eng::serial
    eng::project
    eng::assets
    eng::niscript)

if(NOT ENG_BUILD_TESTS)
    return()
endif()

add_executable(eng_build_tests tests/BuildTests.cpp)
target_link_libraries(eng_build_tests PRIVATE
    eng::build
    eng::fs
    Catch2::Catch2WithMain)
eng_apply_test_policy(eng_build_tests)
add_test(NAME build COMMAND eng_build_tests)
