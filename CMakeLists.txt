#CMake provides a reproducible build workflow rather than manually compiling every source file.

cmake_minimum_required(VERSION 3.20)

project(MiniDB
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

#Production executable
add_executable(minidb 
        src/main.cpp 
        src/storage/disk_manager.cpp)

target_include_directories(minidb PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(minidb PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

#Tests
enable_testing()

add_executable(minidb_tests 
    tests/storage/disk_manager_test.cpp 
    src/storage/disk_manager.cpp)
    
target_include_directories(minidb_tests PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(minidb_tests PRIVATE 
    -Wall 
    -Wextra 
    -Wpedantic 
)

add_test(NAME minidb_tests COMMAND minidb_tests)

