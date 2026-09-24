# eng::reflect — metadados de tipos
eng_add_module(reflect
    src/Reflect.cpp
)
# Aresta arquitetural do grafo da FASE 2 (missão §B.0): reflect situa-se sobre
# eng::core. Nenhum símbolo de core é consumido nesta fase — o link declara a
# camada (detalhe registrado em ADR-021 e docs/architecture/02-reflect.md).
target_link_libraries(eng_reflect PRIVATE eng::core)

if(ENG_BUILD_TESTS)
    add_executable(eng_reflect_tests
        tests/ReflectTests.cpp
    )
    target_link_libraries(eng_reflect_tests PRIVATE eng::reflect Catch2::Catch2WithMain)
    eng_apply_test_policy(eng_reflect_tests)
    add_test(NAME reflect COMMAND eng_reflect_tests)
endif()
