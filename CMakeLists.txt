cmake_minimum_required(VERSION 3.28)

# VERSION é a fonte única da verdade do semver do projeto (§8: formatVersion).
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" ENG_VERSION_TEXT)
string(STRIP "${ENG_VERSION_TEXT}" ENG_VERSION_TEXT)
if(NOT ENG_VERSION_TEXT MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "VERSION inválido: '${ENG_VERSION_TEXT}' (esperado MAJOR.MINOR.PATCH)")
endif()

project(eng
    VERSION ${ENG_VERSION_TEXT}
    DESCRIPTION "eng — engine de jogos (codinome provisório; FASE 1: core, math, mem, log)"
    LANGUAGES CXX
)

# ADR-001: C++20 obrigatório, sem extensões de compilador.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# FASE 1 não usa módulos C++20: a varredura automática do CMake 3.28+ injeta
# -fmodules-ts, que muda a estratégia de emissão de vtables/typeinfo e quebra
# a combinação lib(-fno-rtti) + testes(RTTI p/ Catch2). Desligada até a FASE
# que adotar módulos de fato (decisão registrada no relatório §17.4).
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# Módulos de apoio do build vivem em cmake/ (§2 — PARTE 2).
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(EngineOptions)        # opções globais + eng_add_module()
include(EngineWarnings)       # ADR-020: -Wall -Wextra -Wpedantic [-Werror]
include(EngineSanitizers)     # ASan+UBSan (presets de debug)
include(EngineDependencies)   # FASE 1: Catch2 v3.5.2 · FASE 3: nlohmann/json v3.11.3 (ADR-030)

include(CTest)

add_subdirectory(engine)
# FASE 3 (missão §2.8): integração e2e — pode compor scene+assets+project.
add_subdirectory(tests)
# FASE 8: núcleo C++ do editor (CONSUMIDOR de engine/) — biblioteca
# eng::editor + suite no Linux; o APK reusa ESTA biblioteca via
# add_subdirectory (android/app/src/main/cpp), sem duplicar fontes.
if(ENG_BUILD_TESTS)
    add_subdirectory(editor)
endif()

message(STATUS "eng ${PROJECT_VERSION} — FASES 1-12 COMPLETAS (roadmap finalizado)")
message(STATUS "Presets canônicos: linux-debug, linux-release — ver docs/build.md")
