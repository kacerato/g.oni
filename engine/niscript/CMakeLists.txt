# =============================================================================
# engine/niscript — NI-Script: linguagem própria do G.oni
#
# Pipeline: .nis → Lexer → Parser → Sema → Compiler → bytecode → NI VM.
# SEM Lua, SEM Python, SEM camada de navegador, SEM JIT.
#
# Camadas (grafo 00-overview):
#   - núcleo da linguagem depende APENAS de core/math/ecs(Entity POD)/reflect
#     (nenhuma dependência de RHI/Android/editor — regra de fronteiras);
#   - bindings de COMPONENTES de gameplay são registrados pelo CONSUMIDOR
#     (editor) — MESMO padrão do catálogo do SceneSerializer.
# =============================================================================
add_library(eng_niscript STATIC
    src/NiValue.cpp
    src/Lexer.cpp
    src/Parser.cpp
    src/Sema.cpp
    src/Compiler.cpp
    src/Vm.cpp
    src/Bindings.cpp
    src/NiScript.cpp
)
add_library(eng::niscript ALIAS eng_niscript)

target_include_directories(eng_niscript PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
# NiScript.cpp inclui src/NiAst.hpp (interno) — PRIVATE: não vaza.
target_include_directories(eng_niscript PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(eng_niscript PUBLIC cxx_std_20)
eng_apply_warnings(eng_niscript)
eng_apply_sanitizers(eng_niscript)
eng_apply_lto(eng_niscript)
# Mesmo contrato das libs do motor: sem exceções/RTTI.
target_compile_options(eng_niscript PRIVATE ${ENG_RUNTIME_FLAGS})

target_link_libraries(eng_niscript PUBLIC
    eng::core
    eng::math
    eng::ecs
    eng::reflect)

if(NOT ENG_BUILD_TESTS)
    return()
endif()

add_executable(eng_niscript_tests tests/NiScriptTests.cpp)
target_link_libraries(eng_niscript_tests PRIVATE
    eng::niscript
    eng::scene
    # O TestHost delega move_and_slide para a física REAL.
    eng::physics
    Catch2::Catch2WithMain)
eng_apply_test_policy(eng_niscript_tests)
add_test(NAME niscript COMMAND eng_niscript_tests)
