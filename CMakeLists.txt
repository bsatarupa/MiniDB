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

#common target configuration
function(configure_minidb_target target)
    target_include_directories(${target} PRIVATE 
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_compile_options(${target} PRIVATE 
        -Wall 
        -Wextra 
        -Wpedantic 
    )
endfunction()

#Production executable
add_executable(minidb 
        src/main.cpp 
        src/storage/disk_manager.cpp
        src/storage/database_page_manager.cpp
        src/storage/buffer_pool_manager.cpp
)

configure_minidb_target(minidb)

#Adding Tests
enable_testing()

#Test_1: Disk Manager
add_executable(minidb_tests 
    tests/storage/disk_manager_test.cpp 
    src/storage/disk_manager.cpp
    src/storage/database_page_manager.cpp)

configure_minidb_target(minidb_tests)

add_test(
    NAME minidb_tests
    COMMAND minidb_tests
)

#Test2: Page Format
#Since each test file has its own main(),create separate executables for each test
add_executable(page_format_tests
    tests/storage/page_format_test.cpp)

configure_minidb_target(page_format_tests)

add_test(
    NAME page_format_tests
    COMMAND page_format_tests
)

#Test_3: Page Manager
add_executable(page_manager_tests
    tests/storage/database_page_manager_test.cpp
    src/storage/disk_manager.cpp
    src/storage/database_page_manager.cpp
)

configure_minidb_target(page_manager_tests)

add_test(
    NAME page_manager_tests
    COMMAND page_manager_tests
)

#Test_4: Index Page Manager
add_executable(index_page_manager_tests
    tests/storage/index_page_manager_test.cpp
    src/storage/disk_manager.cpp
    src/storage/index_page_manager.cpp
)

configure_minidb_target(index_page_manager_tests)

add_test(
    NAME index_page_manager_tests
    COMMAND index_page_manager_tests
)

# Test_5: Buffer Pool Manager
add_executable(buffer_pool_manager_tests
    tests/storage/buffer_pool_manager_test.cpp
    src/storage/disk_manager.cpp
    src/storage/database_page_manager.cpp
    src/storage/buffer_pool_manager.cpp
)

configure_minidb_target(buffer_pool_manager_tests)

add_test(
    NAME buffer_pool_manager_tests
    COMMAND buffer_pool_manager_tests
)