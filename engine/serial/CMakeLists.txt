# eng::serial — JSON, envelope binário, migrations, StructCodec
#
eng_add_module(serial
    src/Binary.cpp
    src/Envelope.cpp
    src/Json.cpp
    src/SchemaVersion.cpp
    src/StructCodec.cpp
)
# Arestas do grafo da FASE 3 (missão §5.1): serial sobre core, log, reflect
# e fs (+ nlohmann/json externa, ADR-030).
# - core PUBLIC (Result em todos os headers) e reflect PUBLIC (TypeInfo no
#   header público do StructCodec) — consumidas de fato.
# - fs e log são arestas DECLARADAS sem consumo de símbolos nesta fase:
#   serial é transformação pura bytes/strings ⇄ valores (I/O pertence a
#   fs/chamador; telemetria de parse fica para fase com runtime). Precedente
#   da FASE 2 (ecs → reflect declarada, §B.4/ADR-024).
# - nlohmann_json PUBLIC: JsonValue (header público) envolve nlohmann::json
#   por valor — includes precisam propagar (headers SYSTEM: dependência
#   compila com flags nativos, ADR-020/policy EngineWarnings).
target_link_libraries(eng_serial
    PUBLIC eng::core eng::reflect nlohmann_json::nlohmann_json
    PRIVATE eng::log eng::fs
)

if(ENG_BUILD_TESTS)
    add_executable(eng_serial_tests
        tests/SerialTests.cpp
    )
    target_link_libraries(eng_serial_tests PRIVATE eng::serial Catch2::Catch2WithMain)
    eng_apply_test_policy(eng_serial_tests)
    add_test(NAME serial COMMAND eng_serial_tests)
endif()
