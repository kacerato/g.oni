# =============================================================================
# eng:: — Warnings
# Aplicado apenas a alvos do próprio projeto; dependências (FetchContent)
# compilam com seus flags nativos.
# =============================================================================
function(eng_apply_warnings target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        set(ENG_WARN_FLAGS -Wall -Wextra -Wpedantic)
        if(ENG_ENABLE_WERROR)
            list(APPEND ENG_WARN_FLAGS -Werror)
        endif()
        target_compile_options(${target} PRIVATE ${ENG_WARN_FLAGS})
    else()
        message(FATAL_ERROR "eng_apply_warnings: conjunto de warnings não definido para ${CMAKE_CXX_COMPILER_ID} (FASE 1 = GCC/Clang)")
    endif()
endfunction()
