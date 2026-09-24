# eng::fs — abstração de caminhos e I/O de arquivos
eng_add_module(fs
    src/Path.cpp
    src/File.cpp
    src/MemoryFileSystem.cpp
    src/NativeFileSystem.cpp
    src/RootedFileSystem.cpp
)
# Arestas do grafo da FASE 3 (missão §5.1): fs sobre core e log. fs NÃO
# depende de platform (fronteira ADR-026). core é PUBLIC (Result aparece nos
# headers públicos de fs). log é declarada sem consumo de símbolos nesta
# fase: falhas de I/O trafegam por Result (core) e o reporte por log
# pertence ao chamador — aresta declarada para a camada futura.
target_link_libraries(eng_fs PUBLIC eng::core PRIVATE eng::log)

if(ENG_BUILD_TESTS)
    add_executable(eng_fs_tests
        tests/FsTests.cpp
    )
    target_link_libraries(eng_fs_tests PRIVATE eng::fs Catch2::Catch2WithMain)
    eng_apply_test_policy(eng_fs_tests)
    add_test(NAME fs COMMAND eng_fs_tests)
endif()
