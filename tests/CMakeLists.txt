include(FetchContent)

# GoogleTest, pinned. FetchContent downloads on first configure; subsequent
# configures reuse the populated source under the build tree.
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
)
# Don't let GoogleTest override this project's compiler/linker settings on MSVC.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(lob_tests test_order_book.cpp)
target_link_libraries(lob_tests PRIVATE lob_core GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(lob_tests)
