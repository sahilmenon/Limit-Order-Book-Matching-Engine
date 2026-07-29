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
add_library(lob_core src/order_book.cpp)
target_include_directories(lob_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(lob_core PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>)

# --- Tests -----------------------------------------------------------------
if(LOB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
