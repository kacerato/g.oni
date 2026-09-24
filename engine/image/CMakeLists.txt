# eng::image — decodificação de imagens em memória (evolução P0-2).
#
# Puro sobre core (Result) — nada de fs/rhi/assets aqui: bytes entram,
# RGBA8 sai. O decodificador (stb_image) vive confinado em StbImageImpl.cpp
# com warnings de terceiro desligados.
eng_add_module(image
    src/Image.cpp
    src/StbImageImpl.cpp
)
target_link_libraries(eng_image
    PUBLIC eng::core
    PRIVATE stb_image
)
# stb: código de terceiro (C) — compilado sem -Wall/-Wextra/-Wpedantic/-Werror
# (as flags de arquivo vêm DEPOIS das do alvo e as desligam para ESTE TU).
set_source_files_properties(src/StbImageImpl.cpp PROPERTIES
    COMPILE_OPTIONS "-w"
)

if(ENG_BUILD_TESTS)
    add_executable(eng_image_tests
        tests/ImageTests.cpp
    )
    target_link_libraries(eng_image_tests PRIVATE eng::image Catch2::Catch2WithMain)
    eng_apply_test_policy(eng_image_tests)
    add_test(NAME image COMMAND eng_image_tests)
endif()
