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

    # Storage
    src/storage/disk_manager.cpp
    src/storage/database_page_manager.cpp
    src/storage/index_page_manager.cpp
    src/storage/buffer_pool_manager.cpp

    # B+Tree
    src/storage/btree/btree_node.cpp
    src/storage/btree/bplus_tree.cpp

    # KV Store
    src/storage/kv_store.cpp

    # WAL
    src/storage/wal/log_record.cpp
    src/storage/wal/log_manager.cpp
    src/storage/wal/log_reader.cpp
    src/storage/wal/recovery_manager.cpp
)

configure_minidb_target(minidb)

#Adding Tests
enable_testing()

#Test_1: Disk Manager
add_executable(minidb_tests 
    tests/storage/disk_manager_test.cpp 
    src/storage/disk_manager.cpp
    src/storage/database_page_manager.cpp
)

configure_minidb_target(minidb_tests)

add_test(
    NAME minidb_tests
    COMMAND minidb_tests
)

#Test2: Page Format
#Since each test file has its own main(),create separate executables for each test
add_executable(page_format_tests
    tests/storage/page_format_test.cpp
)

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
    src/storage/wal/log_manager.cpp
    src/storage/wal/log_record.cpp
)

configure_minidb_target(buffer_pool_manager_tests)

add_test(
    NAME buffer_pool_manager_tests
    COMMAND buffer_pool_manager_tests
)

# Test_6: B+Tree Node
add_executable(btree_node_tests
    tests/storage/btree/btree_node_test.cpp
    src/storage/disk_manager.cpp
    src/storage/database_page_manager.cpp
    src/storage/index_page_manager.cpp
    src/storage/buffer_pool_manager.cpp
    src/storage/btree/btree_node.cpp
    src/storage/wal/log_manager.cpp
    src/storage/wal/log_record.cpp
)

configure_minidb_target(btree_node_tests)

add_test(
    NAME btree_node_tests
    COMMAND btree_node_tests
)

# Test_7: B+Tree
add_executable(bplus_tree_tests
    tests/storage/btree/bplus_tree_test.cpp
    src/storage/index_page_manager.cpp
    src/storage/btree/btree_node.cpp
    src/storage/btree/bplus_tree.cpp
    src/storage/disk_manager.cpp
    src/storage/wal/log_manager.cpp
    src/storage/wal/log_record.cpp
)

configure_minidb_target(bplus_tree_tests)

add_test(
    NAME bplus_tree_tests
    COMMAND bplus_tree_tests
)

#Test_8: KV_Store API
add_executable(kv_store_tests
    tests/storage/kv_store_test.cpp
    src/storage/kv_store.cpp
    src/storage/disk_manager.cpp
    src/storage/database_page_manager.cpp
    src/storage/index_page_manager.cpp
    src/storage/buffer_pool_manager.cpp
    src/storage/btree/btree_node.cpp
    src/storage/btree/bplus_tree.cpp
    src/storage/wal/log_manager.cpp
    src/storage/wal/log_record.cpp
)

configure_minidb_target(kv_store_tests)

add_test(
    NAME kv_store_tests
    COMMAND kv_store_tests
)

#Test_9: WAL LogRecord
add_executable(log_record_tests
    tests/storage/wal/log_record_test.cpp
    src/storage/wal/log_record.cpp
)

configure_minidb_target(log_record_tests)

add_test(
    NAME log_record_tests
    COMMAND log_record_tests
)

#Test_10: WAL LogManager
add_executable(log_manager_tests
    tests/storage/wal/log_manager_test.cpp
    src/storage/wal/log_record.cpp
    src/storage/wal/log_manager.cpp
)

configure_minidb_target(log_manager_tests)

add_test(
    NAME log_manager_tests
    COMMAND log_manager_tests
)

#Test_11: WAL LogReader
add_executable(log_reader_tests
    tests/storage/wal/log_reader_test.cpp
    src/storage/wal/log_reader.cpp
    src/storage/wal/log_record.cpp
)

configure_minidb_target(log_reader_tests)

add_test(
    NAME log_reader_tests
    COMMAND log_reader_tests
)

#Test_12: WAL RecoveryManager
add_executable(recovery_manager_tests
    tests/storage/wal/recovery_manager_test.cpp
    src/storage/disk_manager.cpp
    src/storage/database_page_manager.cpp
    src/storage/wal/log_record.cpp
    src/storage/wal/log_reader.cpp
    src/storage/wal/log_manager.cpp
    src/storage/wal/recovery_manager.cpp
)

configure_minidb_target(recovery_manager_tests)

add_test(
    NAME recovery_manager_tests
    COMMAND recovery_manager_tests
)