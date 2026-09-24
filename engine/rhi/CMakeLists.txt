# eng::rhi — Renderer Abstraction
# Interface de hardware gráfico: conceitos de alto nível, ZERO dependência
# de Vulkan/OpenGL ES/EGL. Backends reais entram em backends/ nas FASES 5/6
# (opções ENG_BUILD_RHI_VULKAN / ENG_BUILD_RHI_GLES) e são linkados no
# executável final — nunca aqui.
eng_add_module(rhi
    src/Renderer.cpp
    src/Frame.cpp
)
# Arestas: rhi sobre core (PUBLIC — Result/Error na API pública) e log
# (PRIVATE — logging interno por ENG_LOG_CATEGORY("rhi")). NENHUMA
# dependência gráfica em compile/link — verificável no target abaixo.
target_link_libraries(eng_rhi PUBLIC eng::core PRIVATE eng::log)

# Backends REAIS (FASES 5/6): targets separados, linkados no executável
# final. O option controla a construção — o módulo interface (acima) não
# conhece nenhum deles.
option(ENG_BUILD_RHI_VULKAN "Backend Vulkan real (FASE 5)" ON)
if(ENG_BUILD_RHI_VULKAN)
    add_subdirectory(backends/vulkan)
endif()
option(ENG_BUILD_RHI_GLES "Backend OpenGL ES real (FASE 6)" ON)
if(ENG_BUILD_RHI_GLES)
    add_subdirectory(backends/gles)
endif()

if(ENG_BUILD_TESTS)
    add_executable(eng_rhi_tests
        tests/RhiTests.cpp
        tests/FakeBackend.cpp
    )
    # FakeBackend vive EXCLUSIVAMENTE aqui (missão §11): nunca na biblioteca,
    # nunca em produção, nunca satisfaz testes de integração gráfica real.
    target_link_libraries(eng_rhi_tests PRIVATE eng::rhi Catch2::Catch2WithMain)
    eng_apply_test_policy(eng_rhi_tests)
    add_test(NAME rhi COMMAND eng_rhi_tests)
endif()
