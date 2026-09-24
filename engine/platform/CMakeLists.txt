# eng::platform — fatos do sistema hospedeiro
eng_add_module(platform
    src/Environment.cpp
    src/PlatformInfo.cpp
    src/PlatformPaths.cpp
    src/ProcessInfo.cpp
)
# Arestas do grafo da FASE 3 (missão §5.1): platform sobre core, log e fs.
# fs é PUBLIC (fs::Path é value type dos headers públicos). core PUBLIC
# (Result no header de ProcessInfo). log PRIVATE (consumida nos warns de
# fallback de PlatformPaths). platform NÃO faz I/O além de readlink/getenv.
target_link_libraries(eng_platform PUBLIC eng::core eng::fs PRIVATE eng::log)

if(ENG_BUILD_TESTS)
    add_executable(eng_platform_tests
        tests/PlatformTests.cpp
    )
    target_link_libraries(eng_platform_tests PRIVATE eng::platform Catch2::Catch2WithMain)
    eng_apply_test_policy(eng_platform_tests)
    add_test(NAME platform COMMAND eng_platform_tests)
endif()
