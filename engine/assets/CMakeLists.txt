# eng::assets — identidade, catálogo e ciclo de vida de assets (FASE 3,
# missão §2.5; ADR-028/029)
eng_add_module(assets
    src/AssetManager.cpp
    src/AssetRegistry.cpp
    src/AssetResolver.cpp
    src/AssetType.cpp
)
# Arestas do grafo da FASE 3 (missão §5.1): assets sobre core, log, fs,
# serial, reflect e events.
# - fs/serial PUBLIC (Path e JsonValue aparecem nos headers públicos);
#   core PUBLIC (Result); reflect PUBLIC (não consumido em headers próprios
#   nesta fase — aresta declarada como camada de integração, mesmo padrão
#   de ecs→reflect na FASE 2).
# - log PRIVATE (avisos de referência quebrada de asset — usados pelo
#   consumidor de cena; mantida para a camada).
# - events PRIVATE DECLARADA sem consumo: a missão §5.1 fixa a aresta, o
#   consumo entra na FASE 4 com loadAsync (ADR-029 registra a intenção).
#   Precedente: ecs → reflect.
# - jobs PROIBIDA nesta fase (missão §2.10/R14) — nenhum async existe.
target_link_libraries(eng_assets
    PUBLIC eng::core eng::fs eng::serial eng::reflect
    PRIVATE eng::log eng::events
)

if(ENG_BUILD_TESTS)
    add_executable(eng_assets_tests
        tests/AssetTests.cpp
    )
    target_link_libraries(eng_assets_tests PRIVATE eng::assets Catch2::Catch2WithMain)
    eng_apply_test_policy(eng_assets_tests)
    add_test(NAME assets COMMAND eng_assets_tests)
endif()
