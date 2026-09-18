include(FetchContent)

# GoogleTest, pinned to a release tarball. A URL fetch avoids the git clone/update
# subbuild, which is both slower and prone to MSYS2 fork failures on Windows.
# Updates are disconnected so a reconfigure never re-runs the download step.
set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
# Don't let GoogleTest override this project's compiler/linker settings on MSVC.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
# These tests use plain GoogleTest, never GoogleMock. Skipping gmock cuts build
# time and keeps the deepest header path short enough for Windows' 260-char limit.
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(lob_tests test_order_book.cpp test_fast_order_book.cpp)
target_link_libraries(lob_tests PRIVATE lob_core GTest::gtest_main)

# ITCH reconstruction tests replay synthetic, spec-accurate ITCH 5.0 streams, so
# they validate the decoder without needing a market-data download.
add_executable(lob_itch_tests test_itch_reconstructor.cpp)
target_link_libraries(lob_itch_tests PRIVATE lob_itch GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(lob_tests)
gtest_discover_tests(lob_itch_tests)
