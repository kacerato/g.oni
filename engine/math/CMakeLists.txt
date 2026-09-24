# eng::math — Vec2/3/4, Mat4, Quat, Transform
eng_add_module(math
    src/Vec.cpp
    src/Mat4.cpp
    src/Quat.cpp
    src/Transform.cpp
)

if(ENG_BUILD_TESTS)
    add_executable(eng_math_tests
        tests/VecTests.cpp
        tests/Mat4Tests.cpp
        tests/QuatTests.cpp
        tests/TransformTests.cpp
    )
    target_link_libraries(eng_math_tests PRIVATE eng::math Catch2::Catch2WithMain)
    eng_apply_test_policy(eng_math_tests)
    add_test(NAME math COMMAND eng_math_tests)
endif()
