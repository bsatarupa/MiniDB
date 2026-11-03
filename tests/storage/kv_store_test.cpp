
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <vector>

#include "minidb/storage/disk_manager.h"
#include "minidb/storage/index_page_manager.h"
#include "minidb/storage/kv_store.h"

using namespace minidb::storage;

static void cleanup(const char* filename) {
    std::remove(filename);
}

static void test_empty_store() {
    const char* filename = "build/kv_empty_test.db";
    cleanup(filename);

    DiskManager disk(filename);
    IndexPageManager index_manager(disk);
    KVStore db(index_manager);

    assert(!db.get(10).has_value());
    assert(!db.get(-10).has_value());

    const auto entries = db.scan(0, 100);
    assert(entries.empty());

    cleanup(filename);
    std::cout << "PASS: empty store\n";
}

static void test_put_and_get_single_key() {
    const char* filename = "build/kv_single_key_test.db";
    cleanup(filename);

    DiskManager disk(filename);
    IndexPageManager index_manager(disk);
    KVStore db(index_manager);

    db.put(10, 100);

    const auto value = db.get(10);
    assert(value.has_value());
    assert(*value == 100);

    assert(!db.get(20).has_value());

    cleanup(filename);
    std::cout << "PASS: put and get single key\n";
}

static void test_multiple_keys() {
    const char* filename = "build/kv_multiple_keys_test.db";
    cleanup(filename);

    DiskManager disk(filename);
    IndexPageManager index_manager(disk);
    KVStore db(index_manager);

    db.put(10, 100);
    db.put(20, 200);
    db.put(30, 300);
    db.put(40, 400);

    assert(db.get(10) == 100);
    assert(db.get(20) == 200);
    assert(db.get(30) == 300);
    assert(db.get(40) == 400);

    assert(!db.get(50).has_value());

    cleanup(filename);
    std::cout << "PASS: multiple keys\n";
}

static void test_negative_keys() {
    const char* filename = "build/kv_negative_keys_test.db";
    cleanup(filename);

    DiskManager disk(filename);
    IndexPageManager index_manager(disk);
    KVStore db(index_manager);

    db.put(-100, 1000);
    db.put(-10, 2000);
    db.put(0, 3000);
    db.put(10, 4000);

    assert(db.get(-100) == 1000);
    assert(db.get(-10) == 2000);
    assert(db.get(0) == 3000);
    assert(db.get(10) == 4000);

    cleanup(filename);
    std::cout << "PASS: negative keys\n";
}

static void test_duplicate_key_rejected() {
    const char* filename = "build/kv_duplicate_test.db";
    cleanup(filename);

    DiskManager disk(filename);
    IndexPageManager index_manager(disk);
    KVStore db(index_manager);

    db.put(10, 100);

    bool exception_thrown = false;

    try {
        db.put(10, 200);
    } catch (const std::runtime_error&) {
        exception_thrown = true;
    }

    assert(exception_thrown);

    // The original value must remain unchanged.
    assert(db.get(10) == 100);

    cleanup(filename);
    std::cout << "PASS: duplicate key rejected\n";
}

static void test_scan_empty_store() {
    const char* filename = "build/kv_scan_empty_test.db";
    cleanup(filename);

    DiskManager disk(filename);
    IndexPageManager index_manager(disk);
    KVStore db(index_manager);

    const auto entries = db.scan(10, 100);
    assert(entries.empty());

    cleanup(filename);
    std::cout << "PASS: scan empty store\n";
}

static void test_scan_within_one_leaf() {
    const char* filename = "build/kv_scan_single_leaf_test.db";
    cleanup(filename);

    DiskManager disk(filename);
    IndexPageManager index_manager(disk);
    KVStore db(index_manager);

    db.put(10, 100);
    db.put(20, 200);
    db.put(30, 300);
    db.put(40, 400);
    db.put(50, 500);

    const auto entries = db.scan(20, 40);

    assert(entries.size() == 3);
    assert(entries[0].key == 20);
    assert(entries[0].value == 200);
    assert(entries[1].key == 30);
    assert(entries[1].value == 300);
    assert(entries[2].key == 40);
    assert(entries[2].value == 400);

    cleanup(filename);
    std::cout << "PASS: scan within one leaf\n";
}

static void test_scan_excludes_outside_keys() {
    const char* filename = "build/kv_scan_boundaries_test.db";
    cleanup(filename);

    DiskManager disk(filename);
    IndexPageManager index_manager(disk);
    KVStore db(index_manager);

    for (std::int64_t key = 1; key <= 10; ++key) {
        db.put(key, key * 100);
    }

    const auto entries = db.scan(4, 7);

    assert(entries.size() == 4);

    for (std::size_t i = 0; i < entries.size(); ++i) {
        assert(entries[i].key == static_cast<std::int64_t>(i + 4));
        assert(entries[i].value == entries[i].key * 100);
    }

    cleanup(filename);
    std::cout << "PASS: scan excludes outside keys\n";
}

static void test_scan_reversed_range() {
    const char* filename = "build/kv_scan_reversed_test.db";
    cleanup(filename);

    DiskManager disk(filename);
    IndexPageManager index_manager(disk);
    KVStore db(index_manager);

    db.put(10, 100);
    db.put(20, 200);
    db.put(30, 300);

    const auto entries = db.scan(30, 10);
    assert(entries.empty());

    cleanup(filename);
    std::cout << "PASS: scan reversed range\n";
}

static void test_scan_no_matching_keys() {
    const char* filename = "build/kv_scan_no_match_test.db";
    cleanup(filename);

    DiskManager disk(filename);
    IndexPageManager index_manager(disk);
    KVStore db(index_manager);

    db.put(10, 100);
    db.put(20, 200);
    db.put(30, 300);

    const auto entries = db.scan(40, 50);
    assert(entries.empty());

    cleanup(filename);
    std::cout << "PASS: scan with no matching keys\n";
}

static void test_scan_negative_keys() {
    const char* filename = "build/kv_scan_negative_test.db";
    cleanup(filename);

    DiskManager disk(filename);
    IndexPageManager index_manager(disk);
    KVStore db(index_manager);

    db.put(-30, 300);
    db.put(-20, 200);
    db.put(-10, 100);
    db.put(0, 0);
    db.put(10, 100);

    const auto entries = db.scan(-20, 0);

    assert(entries.size() == 3);
    assert(entries[0].key == -20);
    assert(entries[1].key == -10);
    assert(entries[2].key == 0);

    cleanup(filename);
    std::cout << "PASS: scan negative keys\n";
}

static void test_scan_across_leaf_splits() {
    const char* filename = "build/kv_scan_leaf_splits_test.db";
    cleanup(filename);

    DiskManager disk(filename);
    IndexPageManager index_manager(disk);
    KVStore db(index_manager);

    constexpr std::int64_t KEY_COUNT = 2000;

    for (std::int64_t key = 0; key < KEY_COUNT; ++key) {
        db.put(key, key * 10);
    }

    const auto entries = db.scan(100, 1900);

    assert(entries.size() == 1801);

    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::int64_t expected_key =
            static_cast<std::int64_t>(i) + 100;

        assert(entries[i].key == expected_key);
        assert(entries[i].value == expected_key * 10);
    }

    cleanup(filename);
    std::cout << "PASS: scan across leaf splits\n";
}

static void test_large_insert_and_search() {
    const char* filename = "build/kv_large_insert_test.db";
    cleanup(filename);

    DiskManager disk(filename);
    IndexPageManager index_manager(disk);
    KVStore db(index_manager);

    constexpr std::int64_t KEY_COUNT = 2000;

    for (std::int64_t key = 0; key < KEY_COUNT; ++key) {
        db.put(key, key * 100);
    }

    for (std::int64_t key = 0; key < KEY_COUNT; ++key) {
        const auto value = db.get(key);

        assert(value.has_value());
        assert(*value == key * 100);
    }

    assert(!db.get(KEY_COUNT).has_value());

    cleanup(filename);
    std::cout << "PASS: large insert and search\n";
}

static void test_random_insert_and_search() {
    const char* filename = "build/kv_random_insert_test.db";
    cleanup(filename);

    DiskManager disk(filename);
    IndexPageManager index_manager(disk);
    KVStore db(index_manager);

    // Insert in descending order to exercise a different split pattern.
    constexpr std::int64_t KEY_COUNT = 1000;

    for (std::int64_t key = KEY_COUNT - 1; key >= 0; --key) {
        db.put(key, key + 5000);
    }

    for (std::int64_t key = 0; key < KEY_COUNT; ++key) {
        const auto value = db.get(key);

        assert(value.has_value());
        assert(*value == key + 5000);
    }

    cleanup(filename);
    std::cout << "PASS: reverse-order insert and search\n";
}

static void test_persistence_after_reopen() {
    const char* filename = "build/kv_persistence_test.db";
    cleanup(filename);

    {
        DiskManager disk(filename);
        IndexPageManager index_manager(disk);
        KVStore db(index_manager);

        db.put(10, 100);
        db.put(20, 200);
        db.put(30, 300);
    }

    {
        DiskManager disk(filename);
        IndexPageManager index_manager(disk);
        KVStore db(index_manager);

        assert(db.get(10) == 100);
        assert(db.get(20) == 200);
        assert(db.get(30) == 300);
        assert(!db.get(40).has_value());

        const auto entries = db.scan(10, 30);

        assert(entries.size() == 3);
        assert(entries[0].key == 10);
        assert(entries[1].key == 20);
        assert(entries[2].key == 30);
    }

    cleanup(filename);
    std::cout << "PASS: persistence after reopen\n";
}

static void test_persistence_after_leaf_splits() {
    const char* filename = "build/kv_persistence_splits_test.db";
    cleanup(filename);

    constexpr std::int64_t KEY_COUNT = 1000;

    {
        DiskManager disk(filename);
        IndexPageManager index_manager(disk);
        KVStore db(index_manager);

        for (std::int64_t key = 0; key < KEY_COUNT; ++key) {
            db.put(key, key * 10);
        }
    }

    {
        DiskManager disk(filename);
        IndexPageManager index_manager(disk);
        KVStore db(index_manager);

        for (std::int64_t key = 0; key < KEY_COUNT; ++key) {
            const auto value = db.get(key);

            assert(value.has_value());
            assert(*value == key * 10);
        }

        const auto entries = db.scan(250, 750);

        assert(entries.size() == 501);
        assert(entries.front().key == 250);
        assert(entries.back().key == 750);
    }

    cleanup(filename);
    std::cout << "PASS: persistence after leaf splits\n";
}

int main() {
    test_empty_store();
    test_put_and_get_single_key();
    test_multiple_keys();
    test_negative_keys();
    test_duplicate_key_rejected();

    test_scan_empty_store();
    test_scan_within_one_leaf();
    test_scan_excludes_outside_keys();
    test_scan_reversed_range();
    test_scan_no_matching_keys();
    test_scan_negative_keys();
    test_scan_across_leaf_splits();

    test_large_insert_and_search();
    test_random_insert_and_search();

    test_persistence_after_reopen();
    test_persistence_after_leaf_splits();

    std::cout << "\nAll KVStore tests passed.\n";
    return 0;
}
