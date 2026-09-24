# eng::events — EventBus + Subscription RAII
eng_add_module(events
    src/Events.cpp
)
# Aresta arquitetural do grafo da FASE 2 (missão §B.0): events situa-se sobre
# eng::core. Nenhum símbolo de core é consumido nesta fase — o link declara a
# camada (detalhe registrado em ADR-022 e docs/architecture/03-events.md).
target_link_libraries(eng_events PRIVATE eng::core)

if(ENG_BUILD_TESTS)
    add_executable(eng_events_tests
        tests/EventsTests.cpp
    )
    target_link_libraries(eng_events_tests PRIVATE eng::events Catch2::Catch2WithMain)
    eng_apply_test_policy(eng_events_tests)
    add_test(NAME events COMMAND eng_events_tests)
endif()
