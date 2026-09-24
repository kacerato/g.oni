add_library(eng_audio STATIC
    src/Audio.cpp
    src/Wav.cpp
    src/AudioAdapt.cpp      # P3.4: conversão canais/formato + resampler + gate
    src/AAudioBackend.cpp   # Linux: TU vazio (guard __ANDROID__); APK: real
    src/OpenSlEsBackend.cpp # P4.1/D6: fallback Legacy (Android; vazio Linux)
    src/AutoBackend.cpp     # P4.1/D6: cadeia de seleção (AAudio→OpenSL|null)
)
add_library(eng::audio ALIAS eng_audio)
target_include_directories(eng_audio PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(eng_audio PUBLIC cxx_std_20)
eng_apply_warnings(eng_audio)
eng_apply_sanitizers(eng_audio)
eng_apply_lto(eng_audio)
# Mesa de áudio usa std::mutex (não é exceção — é sincronização §D7).
target_compile_options(eng_audio PRIVATE ${ENG_RUNTIME_FLAGS})
# Log PRIVADO (o backend AAudio loga a drenagem do callback) —
# mesmo padrão de rhi/fs/platform/scene: eng::log é fundação (regra 2
# do grafo), a aresta não propaga para quem depende de eng::audio.
target_link_libraries(eng_audio PUBLIC eng::core eng::fs eng::serial
                                 PRIVATE eng::log)
# Android: dl para dlopen do libaaudio (padrão dos backends gráficos).
if(ANDROID)
    target_link_libraries(eng_audio PUBLIC dl)
endif()

if(NOT ENG_BUILD_TESTS)
    return()
endif()

add_executable(eng_audio_tests tests/AudioTests.cpp)
target_link_libraries(eng_audio_tests PRIVATE eng::audio eng::fs Catch2::Catch2WithMain)
eng_apply_test_policy(eng_audio_tests)
add_test(NAME audio COMMAND eng_audio_tests)
