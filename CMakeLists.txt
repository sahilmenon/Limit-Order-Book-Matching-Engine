cmake_minimum_required(VERSION 3.20)

project(lob
    VERSION 0.1.0
    DESCRIPTION "Low-latency limit order book and matching engine"
    LANGUAGES CXX)

# --- Global settings -------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(LOB_BUILD_TESTS "Build unit tests" ON)

# --- Core library ----------------------------------------------------------
add_library(lob_core src/order_book.cpp src/fast_order_book.cpp)
target_include_directories(lob_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(lob_core PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>)

# --- ITCH reconstruction library + validator -------------------------------
add_library(lob_itch src/itch/reconstructor.cpp)
target_include_directories(lob_itch PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lob_itch PUBLIC lob_core)
target_compile_options(lob_itch PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>)

add_executable(itch_validate apps/itch_validate.cpp)
target_link_libraries(itch_validate PRIVATE lob_itch)

# --- Benchmark harness -----------------------------------------------------
add_executable(bench apps/bench.cpp)
target_link_libraries(bench PRIVATE lob_core)
target_compile_options(bench PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-O3 -march=native>)

# Statically link the GCC/libstdc++ runtime so the tool binaries are self-
# contained and run without the compiler's DLLs on PATH (notably on Windows/MSYS2).
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(itch_validate PRIVATE -static -static-libgcc -static-libstdc++)
    target_link_options(bench PRIVATE -static -static-libgcc -static-libstdc++)
endif()

# --- Tests -----------------------------------------------------------------
if(LOB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
