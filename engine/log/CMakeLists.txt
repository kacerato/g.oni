# eng::log — Logger, sinks, format "{}", macros
eng_add_module(log
    src/Format.cpp
    src/ConsoleSink.cpp
    src/Logger.cpp
)

if(ENG_BUILD_TESTS)
    add_executable(eng_log_tests
        tests/FormatTests.cpp
        tests/LoggerTests.cpp
        tests/ConsoleSinkTests.cpp
    )
    target_link_libraries(eng_log_tests PRIVATE eng::log Catch2::Catch2WithMain)
    eng_apply_test_policy(eng_log_tests)
    add_test(NAME log COMMAND eng_log_tests)
endif()
