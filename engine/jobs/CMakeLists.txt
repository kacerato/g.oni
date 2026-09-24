# eng::jobs — sistema de tarefas work-stealing
eng_add_module(jobs
    src/Jobs.cpp
)
# Aresta arquitetural do grafo da FASE 2 (missão §B.0): jobs situa-se sobre
# eng::core (nenhum símbolo consumido nesta fase — o link declara a camada;
# detalhe em ADR-023 e docs/architecture/04-jobs.md).
target_link_libraries(eng_jobs PRIVATE eng::core)

# --- Exceções em jobs (missão §B.3) -------------------------------------------
# ENG_JOBS_CATCH_EXCEPTIONS=ON: captura exceções dos jobs, armazena via
# exception_ptr e repropaga em JobHandle::wait() (default ON; linux-debug o
# fixa explicitamente). OFF (linux-release): exceção em job propaga no worker
# e encerra o processo — política de release documentada em ADR-023.
#
# isenção REGISTRADA ao ADR-004 (libs sem exceções): Jobs.cpp compila com
# -fexceptions APÓS os flags do módulo (última flag prevalece no GCC) porque
# try/catch é exigido pela semântica de propagação da missão.
option(ENG_JOBS_CATCH_EXCEPTIONS "eng::jobs: capturar exceções de jobs e propagar via wait()" ON)
if(ENG_JOBS_CATCH_EXCEPTIONS)
    target_compile_options(eng_jobs PRIVATE -fexceptions)
    target_compile_definitions(eng_jobs PUBLIC ENG_JOBS_CATCH_EXCEPTIONS=1)
endif()

if(ENG_BUILD_TESTS)
    add_executable(eng_jobs_tests
        tests/JobSystemTests.cpp
    )
    target_link_libraries(eng_jobs_tests PRIVATE eng::jobs Catch2::Catch2WithMain)
    eng_apply_test_policy(eng_jobs_tests)
    add_test(NAME jobs COMMAND eng_jobs_tests)
endif()
