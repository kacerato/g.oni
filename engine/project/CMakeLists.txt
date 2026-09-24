# eng::project — descreve e localiza um projeto de jogo (FASE 3, missão
# §2.6; ADR-032)
eng_add_module(project
    src/ProjectFile.cpp
    src/ProjectPaths.cpp
)
# Arestas do grafo da FASE 3 (missão §5.1): project sobre core, log, fs,
# platform, assets e serial.
# - core PUBLIC (Result/Version), fs PUBLIC (Path em headers), serial
#   PUBLIC (JsonValue em header de ProjectFile).
# - platform PRIVATE consumida de fato (ProjectPaths::systemDefaults →
#   PlatformPaths::detect).
# - assets e log DECLARADAS sem consumo de símbolos nesta fase: o pipeline
#   projeto→registry→assets é montado pelo CHAMADOR (tests/ de integração);
#   aresta fixada por §5.1, consumo documentado como composição externa.
#   Precedente: ecs → reflect.
target_link_libraries(eng_project
    PUBLIC eng::core eng::fs eng::serial
    PRIVATE eng::platform eng::assets eng::log
)

if(ENG_BUILD_TESTS)
    add_executable(eng_project_tests
        tests/ProjectTests.cpp
    )
    target_link_libraries(eng_project_tests PRIVATE eng::project Catch2::Catch2WithMain)
    eng_apply_test_policy(eng_project_tests)
    add_test(NAME project COMMAND eng_project_tests)
endif()
