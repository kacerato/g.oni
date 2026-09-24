# =============================================================================
# eng:: — Sanitizers
# UBSan configurado como não-recuperável: qualquer UB aborta o processo e
# falha o ctest (nada de "avisar e passar" — guardrail §15.3/§15.6).
# =============================================================================
function(eng_apply_sanitizers target)
    if(NOT ENG_ENABLE_SANITIZERS)
        return()
    endif()

    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "Sanitizers suportados apenas em GCC/Clang (recebido: ${CMAKE_CXX_COMPILER_ID})")
    endif()

    target_compile_options(${target} PRIVATE
        -fsanitize=address,undefined
        -fno-sanitize-recover=all
        -fno-omit-frame-pointer
    )
    # Em bibliotecas STATIC as opções de link são ignoradas pelo gerador; nos
    # executáveis de teste (mesma função) elas efetivam o link do runtime.
    target_link_options(${target} PRIVATE
        -fsanitize=address,undefined
    )
endfunction()
