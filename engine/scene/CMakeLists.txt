# eng::scene — hierarquia de nós sobre o ECS + serialização de
# cena + links/camadas (evolução P0-5,
# ADR-051)
eng_add_module(scene
    src/Scene.cpp
    src/SceneSerializer.cpp
    src/Links.cpp
    src/Layers.cpp
)
# Scene sobre core, ecs e math. ecs/math PUBLIC
# porque headers públicos de scene incluem os dois.
# A serialização de cena vive DENTRO de scene
# (regra dura "ninguém depende de scene exceto tests/" — §5.1). Arestas
# novas PRIVATE: serial (parse/dump/StructCodec, só no .cpp do serializer),
# reflect (ENG_REFLECT dos tipos math + TypeInfo no registro de
# componentes) e log (WARN de componentes internos tolerados no load).
# Nenhum arquivo/semântica de FASE 2 alterado.
target_link_libraries(eng_scene
    PUBLIC eng::ecs eng::math eng::events
    PRIVATE eng::core eng::serial eng::reflect eng::log
)

if(ENG_BUILD_TESTS)
    add_executable(eng_scene_tests
        tests/SceneTests.cpp
        tests/SceneSerializerTests.cpp
        tests/LinksTests.cpp
        tests/LayersTests.cpp
    )
    target_link_libraries(eng_scene_tests PRIVATE eng::scene eng::assets eng::log Catch2::Catch2WithMain)
    eng_apply_test_policy(eng_scene_tests)
    add_test(NAME scene COMMAND eng_scene_tests)
endif()
