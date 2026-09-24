# eng::render — Shader Core / Material / Light2D / DrawList do 2D.
#
# Camada ENTRE o engine high-level e o eng::rhi:
#   Editor/runtime → Shader/Material abstraction → eng::rhi → Vulkan/GLES
#
# Dependências: core (Result), rhi (handles/pipelines), reflect (Light2D),
# serial (codec JSON do material). NENHUMA dependência do editor — o
# render é ABAIXO do editor no grafo (o editor preenche a DrawList).
eng_add_module(render
    src/ShaderLibrary.cpp
    src/SpriteMaterial.cpp
)
target_link_libraries(eng_render
    PUBLIC eng::core eng::rhi eng::reflect eng::serial
)

if(ENG_BUILD_TESTS)
    add_executable(eng_render_tests
        tests/RenderTests.cpp
    )
    target_link_libraries(eng_render_tests PRIVATE eng::render eng::rhi
        eng::rhi::vulkan eng::rhi::gles eng::log Catch2::Catch2WithMain)
    eng_apply_test_policy(eng_render_tests)
    add_test(NAME render COMMAND eng_render_tests)
endif()
