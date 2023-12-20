cmake_minimum_required(VERSION 3.16)
project(lsmkv CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(LSMKV_BUILD_TESTS "Build tests" ON)
set(LSMKV_SANITIZE "" CACHE STRING "Sanitizer to build with (address, thread, ...)")
if(LSMKV_SANITIZE)
  add_compile_options(-fsanitize=${LSMKV_SANITIZE} -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${LSMKV_SANITIZE})
endif()

find_package(Threads REQUIRED)

add_library(lsmkv
  src/block.cc
  src/db_impl.cc
  src/file.cc
  src/iterators.cc
  src/log.cc
  src/memtable.cc
  src/table.cc
  src/version.cc
  src/write_batch.cc)
target_include_directories(lsmkv PUBLIC include)
target_compile_options(lsmkv PRIVATE -Wall -Wextra)
target_link_libraries(lsmkv PUBLIC Threads::Threads)

add_executable(db_bench bench/db_bench.cc)
target_link_libraries(db_bench PRIVATE lsmkv)
add_executable(kvcli tools/kvcli.cc)
target_link_libraries(kvcli PRIVATE lsmkv)

if(LSMKV_BUILD_TESTS)
  enable_testing()
  find_package(GTest QUIET)
  if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()
  add_executable(lsmkv_tests tests/format_test.cc tests/db_test.cc)
  target_link_libraries(lsmkv_tests PRIVATE lsmkv GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(lsmkv_tests DISCOVERY_TIMEOUT 30)
endif()
