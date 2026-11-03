
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>
#include <vector>

#include "minidb/storage/btree/bplus_tree.h"
#include "minidb/storage/disk_manager.h"
#include "minidb/storage/btree/btree_node.h"
#include "minidb/storage/index_page_manager.h"

using namespace minidb::storage;

namespace {

constexpr const char* TEST_DB_FILE = "build/bplus_tree_test.db";

void clean_test_database() {
    std::remove(TEST_DB_FILE);
}

void verify_key(BPlusTree& tree, std::int64_t key, std::uint64_t value) {
    const auto result = tree.search(key);

    assert(result.has_value());
    assert(result.value() == value);
}

// 1. Empty tree.
void test_empty_tree_returns_no_result() {
    clean_test_database();

    DiskManager disk(TEST_DB_FILE);
    IndexPageManager page_manager(disk);
    BPlusTree tree(page_manager);

    assert(!tree.search(10).has_value());

    std::cout << "[PASS] EmptyTreeReturnsNoResult\n";
}

// 2. Single key.
void test_insert_and_search_single_key() {
    clean_test_database();

    DiskManager disk(TEST_DB_FILE);
    IndexPageManager page_manager(disk);
    BPlusTree tree(page_manager);

    tree.insert(10, 100);

    verify_key(tree, 10, 100);

    std::cout << "[PASS] InsertAndSearchSingleKey\n";
}

// 3. Sorted insertion.
void test_insert_multiple_keys_in_sorted_order() {
    clean_test_database();

    DiskManager disk(TEST_DB_FILE);
    IndexPageManager page_manager(disk);
    BPlusTree tree(page_manager);

    for (std::int64_t key = 1; key <= 100; ++key) {
        tree.insert(key, key * 10);
    }

    for (std::int64_t key = 1; key <= 100; ++key) {
        verify_key(tree, key, key * 10);
    }

    std::cout << "[PASS] SortedInsertion\n";
}

// 4. Reverse insertion.
void test_insert_keys_in_reverse_order() {
    clean_test_database();

    DiskManager disk(TEST_DB_FILE);
    IndexPageManager page_manager(disk);
    BPlusTree tree(page_manager);

    for (std::int64_t key = 100; key >= 1; --key) {
        tree.insert(key, key * 10);
    }

    for (std::int64_t key = 1; key <= 100; ++key) {
        verify_key(tree, key, key * 10);
    }

    std::cout << "[PASS] ReverseInsertion\n";
}

// 5. Negative keys.
void test_insert_negative_keys() {
    clean_test_database();

    DiskManager disk(TEST_DB_FILE);
    IndexPageManager page_manager(disk);
    BPlusTree tree(page_manager);

    for (std::int64_t key = -100; key <= 100; ++key) {
        tree.insert(key, static_cast<std::uint64_t>(key + 100));
    }

    for (std::int64_t key = -100; key <= 100; ++key) {
        verify_key(tree, key, static_cast<std::uint64_t>(key + 100));
    }

    std::cout << "[PASS] NegativeAndPositiveKeys\n";
}

// 6. Missing key.
void test_missing_key_returns_no_result() {
    clean_test_database();

    DiskManager disk(TEST_DB_FILE);
    IndexPageManager page_manager(disk);
    BPlusTree tree(page_manager);

    tree.insert(10, 100);
    tree.insert(30, 300);

    assert(!tree.search(20).has_value());
    assert(!tree.search(40).has_value());

    std::cout << "[PASS] MissingKeyReturnsNoResult\n";
}

// 7. Duplicate rejection.
void test_duplicate_key_is_rejected() {
    clean_test_database();

    DiskManager disk(TEST_DB_FILE);
    IndexPageManager page_manager(disk);
    BPlusTree tree(page_manager);

    tree.insert(10, 100);

    bool exception_thrown = false;

    try {
        tree.insert(10, 999);
    } catch (const std::runtime_error&) {
        exception_thrown = true;
    }

    assert(exception_thrown);
    verify_key(tree, 10, 100);

    std::cout << "[PASS] DuplicateKeyIsRejected\n";
}

// 8. Persistence without a split.
void test_root_persists_after_reopening_index() {
    clean_test_database();

    {
        DiskManager disk(TEST_DB_FILE);
        IndexPageManager page_manager(disk);
        BPlusTree tree(page_manager);

        tree.insert(10, 100);
        tree.insert(20, 200);
    }

    {
        DiskManager disk(TEST_DB_FILE);
        IndexPageManager page_manager(disk);
        BPlusTree tree(page_manager);

        verify_key(tree, 10, 100);
        verify_key(tree, 20, 200);
    }

    std::cout << "[PASS] RootPersistsAfterReopeningIndex\n";
}

// 9. First leaf split.
void test_leaf_split() {
    clean_test_database();

    DiskManager disk(TEST_DB_FILE);
    IndexPageManager page_manager(disk);
    BPlusTree tree(page_manager);

    const std::int64_t count =
        BTreeNodeFormat::LEAF_CAPACITY + 1;

    for (std::int64_t key = 1; key <= count; ++key) {
        tree.insert(key, key * 10);
    }

    for (std::int64_t key = 1; key <= count; ++key) {
        verify_key(tree, key, key * 10);
    }

    std::cout << "[PASS] LeafSplit\n";
}

// 10. Multiple leaf splits and parent updates.
void test_multiple_leaf_splits() {
    clean_test_database();

    DiskManager disk(TEST_DB_FILE);
    IndexPageManager page_manager(disk);
    BPlusTree tree(page_manager);

    constexpr std::int64_t count = 2000;

    for (std::int64_t key = 1; key <= count; ++key) {
        tree.insert(key, key * 10);
    }

    for (std::int64_t key = 1; key <= count; ++key) {
        verify_key(tree, key, key * 10);
    }

    std::cout << "[PASS] MultipleLeafSplits\n";
}

// 11. Separator boundary searches after a leaf split.
void test_search_separator_boundaries() {
    clean_test_database();

    DiskManager disk(TEST_DB_FILE);
    IndexPageManager page_manager(disk);
    BPlusTree tree(page_manager);

    constexpr std::int64_t count = 1000;

    for (std::int64_t key = 1; key <= count; ++key) {
        tree.insert(key, key * 10);
    }

    // Test keys around likely leaf separators.
    for (std::int64_t key = 120; key <= count; key += 127) {
        for (std::int64_t offset = -1; offset <= 1; ++offset) {
            const std::int64_t candidate = key + offset;

            if (candidate >= 1 && candidate <= count) {
                verify_key(tree, candidate, candidate * 10);
            }
        }
    }

    std::cout << "[PASS] SearchSeparatorBoundaries\n";
}

// 12. Duplicate rejection after a leaf split.
void test_duplicate_rejection_after_leaf_split() {
    clean_test_database();

    DiskManager disk(TEST_DB_FILE);
    IndexPageManager page_manager(disk);
    BPlusTree tree(page_manager);

    const std::int64_t count =
        BTreeNodeFormat::LEAF_CAPACITY + 1;

    for (std::int64_t key = 1; key <= count; ++key) {
        tree.insert(key, key * 10);
    }

    bool exception_thrown = false;

    try {
        tree.insert(200, 999999);
    } catch (const std::runtime_error&) {
        exception_thrown = true;
    }

    assert(exception_thrown);
    verify_key(tree, 200, 2000);

    std::cout << "[PASS] DuplicateRejectionAfterLeafSplit\n";
}

// 13. Persistence after a leaf split.
void test_persistence_after_leaf_split() {
    clean_test_database();

    constexpr std::int64_t count = 1000;

    {
        DiskManager disk(TEST_DB_FILE);
        IndexPageManager page_manager(disk);
        BPlusTree tree(page_manager);

        for (std::int64_t key = 1; key <= count; ++key) {
            tree.insert(key, key * 10);
        }
    }

    {
        DiskManager disk(TEST_DB_FILE);
        IndexPageManager page_manager(disk);
        BPlusTree tree(page_manager);

        for (std::int64_t key = 1; key <= count; ++key) {
            verify_key(tree, key, key * 10);
        }
    }

    std::cout << "[PASS] PersistenceAfterLeafSplit\n";
}

// 14. Random insertion order.
void test_random_insertion_order() {
    clean_test_database();

    DiskManager disk(TEST_DB_FILE);
    IndexPageManager page_manager(disk);
    BPlusTree tree(page_manager);

    constexpr std::int64_t count = 2000;

    std::vector<std::int64_t> keys(count);
    std::iota(keys.begin(), keys.end(), 1);

    std::mt19937 generator(42);
    std::shuffle(keys.begin(), keys.end(), generator);

    for (const auto key : keys) {
        tree.insert(key, key * 10);
    }

    for (std::int64_t key = 1; key <= count; ++key) {
        verify_key(tree, key, key * 10);
    }

    std::cout << "[PASS] RandomInsertionOrder\n";
}

// 15. Root internal-node split.
//
// With the current 254-key internal capacity, this workload
// creates enough leaf splits to overflow the root internal node.
void test_root_internal_split() {
    clean_test_database();

    DiskManager disk(TEST_DB_FILE);
    IndexPageManager page_manager(disk);
    BPlusTree tree(page_manager);

    constexpr std::int64_t leaf_split_count =
        BTreeNodeFormat::INTERNAL_CAPACITY + 1;

    constexpr std::int64_t initial_leaf_split_size =
        BTreeNodeFormat::LEAF_CAPACITY + 1;

    constexpr std::int64_t subsequent_leaf_split_interval =
        BTreeNodeFormat::LEAF_CAPACITY + 1 -
        ((BTreeNodeFormat::LEAF_CAPACITY + 1) / 2);

    constexpr std::int64_t total_keys =
        initial_leaf_split_size +
        (leaf_split_count - 1) * subsequent_leaf_split_interval;

    std::cout << "  Inserting " << total_keys
              << " keys to trigger root internal split...\n";

    for (std::int64_t key = 1; key <= total_keys; ++key) {
        tree.insert(key, key * 10);
    }

    // Verify all keys, including keys on both sides of
    // the newly created root separator.
    for (std::int64_t key = 1; key <= total_keys; ++key) {
        verify_key(tree, key, key * 10);
    }

    std::cout << "[PASS] RootInternalSplit\n";
}

// 16. Mixed insert and search operations.
void test_mixed_insert_and_search_operations() {
    clean_test_database();

    DiskManager disk(TEST_DB_FILE);
    IndexPageManager page_manager(disk);
    BPlusTree tree(page_manager);

    for (std::int64_t key = 1; key <= 500; ++key) {
        tree.insert(key, key * 10);

        if (key % 10 == 0) {
            verify_key(tree, key, key * 10);
        }
    }

    for (std::int64_t key = 501; key <= 1000; ++key) {
        tree.insert(key, key * 10);
    }

    for (std::int64_t key = 1; key <= 1000; ++key) {
        verify_key(tree, key, key * 10);
    }

    std::cout << "[PASS] MixedInsertAndSearchOperations\n";
}

void test_range_scan_within_single_leaf() {
    clean_test_database();

    DiskManager disk(TEST_DB_FILE);
    IndexPageManager page_manager(disk);
    BPlusTree tree(page_manager);

    for (std::int64_t key = 1; key <= 100; ++key) {
        tree.insert(key, key * 10);
    }

    const auto result = tree.range_scan(20, 30);

    assert(result.size() == 11);

    for (std::size_t i = 0; i < result.size(); ++i) {
        assert(result[i].key == static_cast<std::int64_t>(20 + i));
        assert(result[i].value == static_cast<std::uint64_t>((20 + i) * 10));
    }

    std::cout << "[PASS] RangeScanWithinSingleLeaf\n";
}

void test_range_scan_across_multiple_leaves() {
    clean_test_database();

    DiskManager disk(TEST_DB_FILE);
    IndexPageManager page_manager(disk);
    BPlusTree tree(page_manager);

    constexpr std::int64_t count = 1000;

    for (std::int64_t key = 1; key <= count; ++key) {
        tree.insert(key, key * 10);
    }

    const auto result = tree.range_scan(100, 900);

    assert(result.size() == 801);

    for (std::size_t i = 0; i < result.size(); ++i) {
        const std::int64_t expected_key = 100 + i;

        assert(result[i].key == expected_key);
        assert(result[i].value ==
               static_cast<std::uint64_t>(expected_key * 10));
    }

    std::cout << "[PASS] RangeScanAcrossMultipleLeaves\n";
}

void test_range_scan_separator_boundaries() {
    clean_test_database();

    DiskManager disk(TEST_DB_FILE);
    IndexPageManager page_manager(disk);
    BPlusTree tree(page_manager);

    constexpr std::int64_t count = 1000;

    for (std::int64_t key = 1; key <= count; ++key) {
        tree.insert(key, key * 10);
    }

    const auto result = tree.range_scan(127, 255);

    assert(result.size() == 129);

    for (std::size_t i = 0; i < result.size(); ++i) {
        const std::int64_t expected_key = 127 + i;

        assert(result[i].key == expected_key);
        assert(result[i].value ==
               static_cast<std::uint64_t>(expected_key * 10));
    }

    std::cout << "[PASS] RangeScanSeparatorBoundaries\n";
}

void test_range_scan_no_matching_keys() {
    clean_test_database();

    DiskManager disk(TEST_DB_FILE);
    IndexPageManager page_manager(disk);
    BPlusTree tree(page_manager);

    tree.insert(10, 100);
    tree.insert(20, 200);
    tree.insert(30, 300);

    const auto result = tree.range_scan(40, 50);

    assert(result.empty());

    std::cout << "[PASS] RangeScanNoMatchingKeys\n";
}

void test_range_scan_reversed_range() {
    clean_test_database();

    DiskManager disk(TEST_DB_FILE);
    IndexPageManager page_manager(disk);
    BPlusTree tree(page_manager);

    for (std::int64_t key = 1; key <= 100; ++key) {
        tree.insert(key, key * 10);
    }

    const auto result = tree.range_scan(80, 20);

    assert(result.empty());

    std::cout << "[PASS] RangeScanReversedRange\n";
}

void test_range_scan_empty_tree() {
    clean_test_database();

    DiskManager disk(TEST_DB_FILE);
    IndexPageManager page_manager(disk);
    BPlusTree tree(page_manager);

    const auto result = tree.range_scan(1, 100);

    assert(result.empty());

    std::cout << "[PASS] RangeScanEmptyTree\n";
}

void test_range_scan_negative_keys() {
    clean_test_database();

    DiskManager disk(TEST_DB_FILE);
    IndexPageManager page_manager(disk);
    BPlusTree tree(page_manager);

    for (std::int64_t key = -300; key <= 300; ++key) {
        tree.insert(key, static_cast<std::uint64_t>(key + 300));
    }

    const auto result = tree.range_scan(-100, 100);

    assert(result.size() == 201);

    for (std::size_t i = 0; i < result.size(); ++i) {
        const std::int64_t expected_key = -100 + i;

        assert(result[i].key == expected_key);
        assert(result[i].value ==
               static_cast<std::uint64_t>(expected_key + 300));
    }

    std::cout << "[PASS] RangeScanNegativeKeys\n";
}

} // namespace

int main() {

    test_empty_tree_returns_no_result();
    test_insert_and_search_single_key();
    test_insert_multiple_keys_in_sorted_order();
    test_insert_keys_in_reverse_order();
    test_insert_negative_keys();
    test_missing_key_returns_no_result();
    test_duplicate_key_is_rejected();
    test_root_persists_after_reopening_index();

    test_leaf_split();
    test_multiple_leaf_splits();
    test_search_separator_boundaries();
    test_duplicate_rejection_after_leaf_split();
    test_persistence_after_leaf_split();
    test_random_insertion_order();
    test_mixed_insert_and_search_operations();

    test_root_internal_split();
    test_range_scan_within_single_leaf();
    test_range_scan_across_multiple_leaves();
    test_range_scan_separator_boundaries();
    test_range_scan_no_matching_keys();
    test_range_scan_reversed_range();
    test_range_scan_empty_tree();
    test_range_scan_negative_keys();

    clean_test_database();

    std::cout << "\nAll B+Tree tests passed!\n";
    return 0;
}
