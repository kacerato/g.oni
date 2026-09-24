# =============================================================================
# eng:: — Opções globais e helpers de módulos
# =============================================================================

option(ENG_BUILD_TESTS       "Construir testes unitários do motor"                 ON)
option(ENG_ENABLE_WERROR     "Tratar warnings como erros (ADR-020)"                ON)
option(ENG_ENABLE_SANITIZERS "Habilitar AddressSanitizer + UBSan"                  OFF)
option(ENG_ENABLE_LTO        "Habilitar Link-Time Optimization"                    OFF)
option(ENG_MATH_VULKAN_DEPTH "Faixa de profundidade [0,1] (Vulkan) em eng::math"   OFF)

# --- ADR-004 / ADR-005: runtime sem exceções e sem RTTI ----------------------
# Aplicado apenas às bibliotecas do motor (eng::*). Executáveis de teste são
# isentos — Catch2 exige exceções (justificativa registrada em
# docs/architecture/00-overview.md).
set(ENG_RUNTIME_FLAGS "-fno-exceptions" "-fno-rtti")

# --- LTO: valida suporte antes de habilitar ----------------------------------
if(ENG_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT eng_ipo_supported OUTPUT eng_ipo_output)
    if(NOT eng_ipo_supported)
        message(FATAL_ERROR "LTO solicitado (ENG_ENABLE_LTO=ON) mas não suportado: ${eng_ipo_output}")
    endif()
endif()

# --- eng_apply_lto(<target>) -------------------------------------------------
function(eng_apply_lto target)
    if(ENG_ENABLE_LTO)
        set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endfunction()

# --- eng_apply_test_policy(<target>) -----------------------------------------
# Política para executáveis de teste: warnings, sanitizers, LTO e
# -fno-rtti. O RTTI nos testes forçaria referências a typeinfo de classes do
# motor (compiladas com -fno-rtti, ADR-005) — Catch2 v3.5.2 compila limpo
# sem RTTI nesta configuração, então os testes também ficam sem RTTI.
# Exceções PERMANECEM habilitadas nos testes (Catch2 as exige — ADR-004
# documentado como isenção de testes).
function(eng_apply_test_policy target)
    eng_apply_warnings(${target})
    eng_apply_sanitizers(${target})
    eng_apply_lto(${target})
    target_compile_options(${target} PRIVATE -fno-rtti)
endfunction()

# --- eng_add_module(<name> [fontes...]) ---------------------------------------
# Cria a biblioteca estática eng::<name> com a política completa de flags:
# C++20, warnings, sanitizers, -fno-exceptions/-fno-rtti,
# LTO opcional e include público em include/.
function(eng_add_module name)
    set(ENG_MODULE_SOURCES ${ARGN})
    if(NOT ENG_MODULE_SOURCES)
        message(FATAL_ERROR "eng_add_module(${name}): ao menos um arquivo-fonte é obrigatório (guardrail §15.7)")
    endif()

    add_library(eng_${name} STATIC ${ENG_MODULE_SOURCES})
    add_library(eng::${name} ALIAS eng_${name})

    target_include_directories(eng_${name} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_features(eng_${name} PUBLIC cxx_std_20)

    eng_apply_warnings(eng_${name})
    eng_apply_sanitizers(eng_${name})
    eng_apply_lto(eng_${name})
    target_compile_options(eng_${name} PRIVATE ${ENG_RUNTIME_FLAGS})

    if(name STREQUAL "math" AND ENG_MATH_VULKAN_DEPTH)
        target_compile_definitions(eng_${name} PUBLIC ENG_MATH_VULKAN_DEPTH=1)
    endif()
endfunction()
