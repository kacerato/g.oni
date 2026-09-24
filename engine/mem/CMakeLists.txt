# eng::mem — Allocator, HeapAllocator, ArenaAllocator
eng_add_module(mem
    src/Allocator.cpp
    src/HeapAllocator.cpp
    src/ArenaAllocator.cpp
)

if(ENG_BUILD_TESTS)
    add_executable(eng_mem_tests
        tests/HeapTests.cpp
        tests/ArenaTests.cpp
    )
    target_link_libraries(eng_mem_tests PRIVATE eng::mem Catch2::Catch2WithMain)
    eng_apply_test_policy(eng_mem_tests)
    add_test(NAME mem COMMAND eng_mem_tests)
endif()
