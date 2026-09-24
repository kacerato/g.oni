# eng::ecs — World + sparse-sets + handles geracionais
eng_add_module(ecs
    src/Ecs.cpp
)
# Arestas do grafo da FASE 2 (missão §B.0): ecs sobre core e reflect. Nenhum
# símbolo de reflect é consumido no código nesta fase (missão §B.4: sem
# dependência forçada; integração futura permitida) — o link declara a camada.
# Detalhe em ADR-024 e docs/architecture/05-ecs.md.
target_link_libraries(eng_ecs PRIVATE eng::core eng::reflect)

if(ENG_BUILD_TESTS)
    add_executable(eng_ecs_tests
        tests/EcsTests.cpp
    )
    target_link_libraries(eng_ecs_tests PRIVATE eng::ecs Catch2::Catch2WithMain)
    eng_apply_test_policy(eng_ecs_tests)
    add_test(NAME ecs COMMAND eng_ecs_tests)
endif()
