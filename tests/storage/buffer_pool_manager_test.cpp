#include "minidb/storage/buffer_pool_manager.h"
#include "minidb/storage/disk_manager.h"
#include "minidb/storage/database_page_manager.h"
#include "minidb/storage/page_format.h"
#include "minidb/storage/wal/log_manager.h"
#include "minidb/storage/wal/log_record.h"

#include <cmath>
#include <filesystem>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <cassert>
#include <stdexcept>
#include <filesystem>

using namespace minidb::storage;

void test_valid_capacity() {

    std::filesystem::remove("build/buffer_pool_valid.db");

    DiskManager disk("build/buffer_pool_valid.db");
    DatabasePageManager page_manager(disk);

    wal::LogManager log_manager("build/buffer_pool_valid.wal");
    log_manager.open(); //to prevent overwriting next_lsn_

    BufferPoolManager pool(page_manager, log_manager, 10);
    std::cout << "[PASS] valid capacity\n";
}

void test_zero_capacity_rejected() {

    std::filesystem::remove("build/buffer_pool_zero.db");

    DiskManager disk("build/buffer_pool_zero.db");

    wal::LogManager log_manager("build/buffer_pool_zero.wal");
    log_manager.open(); //to prevent overwriting next_lsn_

    DatabasePageManager page_manager(disk);

    bool exception_thrown = false;

    try {
        BufferPoolManager pool(page_manager, log_manager, 0);
    } catch (const std::invalid_argument&) {
        exception_thrown = true;
    }

    assert(exception_thrown); //if exception_thrown == F, assert() aborts with cerr message
    std::cout << "[PASS] zero capacity rejected\n";
}

void test_fetch_page_cache_miss() {

    std::filesystem::remove("build/buffer_pool_cache_miss.db");

    DiskManager disk("build/buffer_pool_cache_miss.db");
    DatabasePageManager page_manager(disk);

    wal::LogManager log_manager("build/buffer_pool_cache_miss.wal");
    log_manager.open(); //to prevent overwriting next_lsn_

    //Since PageManager only allows reads of 'allocated' pages
    std::uint64_t page_id = page_manager.allocate_page();

    BufferPoolManager pool(page_manager, log_manager, 2);
    Frame &frame = pool.fetch_page(page_id);

    assert(frame.is_occupied);
    assert(!frame.is_dirty);
    assert(frame.pin_count == 1);
    assert(frame.page_id == page_id);

    std::cout << "[PASS] fetch page on cache miss\n";
}

void test_fetch_page_cache_hit() {

    std::filesystem::remove("build/buffer_pool_cache_miss.db");

    DiskManager disk("build/buffer_pool_cache_miss.db");
    DatabasePageManager page_manager(disk);

    wal::LogManager log_manager("build/buffer_pool_cache_miss.wal");
    log_manager.open(); //to prevent overwriting next_lsn_

    //Since PageManager only allows reads of 'allocated' pages
    std::uint64_t page_id = page_manager.allocate_page();

    BufferPoolManager pool(page_manager, log_manager, 2);

    Frame &frame1 = pool.fetch_page(page_id); //1st fetch encounters a cache miss and loads the page
    Frame &frame2 = pool.fetch_page(page_id); //2nd fetch encounters a cache hit and reuses the existing in-memory frame

    //both fetch should refer to the SAME cached frame
    assert(&frame1 == &frame2);

    //each fetch pins the page
    assert(frame1.pin_count == 2);

    std::cout << "[PASS] fetch page on cache hit\n";
}

void test_unpin_page() {

    std::filesystem::remove("build/buffer_pool_unpin.db");

    DiskManager disk("build/buffer_pool_unpin.db");
    DatabasePageManager page_manager(disk);

    wal::LogManager log_manager("build/buffer_pool_unpin.wal");
    log_manager.open(); //to prevent overwriting next_lsn_

    std::uint64_t page_id = page_manager.allocate_page();

    BufferPoolManager pool(page_manager, log_manager, 2);

    Frame& frame = pool.fetch_page(page_id);

    assert(frame.pin_count == 1);
    assert(!frame.is_dirty);

    pool.unpin_page(page_id, true);

    assert(frame.pin_count == 0);
    assert(frame.is_dirty);

    std::cout << "PASS: unpin page and mark dirty\n";
}

void test_unpin_page_with_zero_pin_count() {

    std::filesystem::remove("build/buffer_pool_invalid_unpin.db");

    DiskManager disk("build/buffer_pool_invalid_unpin.db");
    DatabasePageManager page_manager(disk);

    wal::LogManager log_manager("build/buffer_pool_invalid_unpin.wal");
    log_manager.open(); //to prevent overwriting next_lsn_

    std::uint64_t page_id = page_manager.allocate_page();

    BufferPoolManager pool(page_manager, log_manager, 2);

    pool.fetch_page(page_id);
    pool.unpin_page(page_id, false); //valid

    bool throw_exception = false;

    try {
        pool.unpin_page(page_id, false); //invalid
    } catch (const std::runtime_error&) {
        throw_exception = true;
    }

    assert(throw_exception);
    std::cout << "[PASS] Unpin with zero pin count rejected\n";
}

void test_lru_eviction() {

    std::filesystem::remove("build/buffer_pool_lru.db");

    DiskManager disk("build/buffer_pool_lru.db");
    DatabasePageManager page_manager(disk);

    wal::LogManager log_manager("build/buffer_pool_lru.wal");
    log_manager.open(); //to prevent overwriting next_lsn_

    const std::uint64_t page1 = page_manager.allocate_page();
    const std::uint64_t page2 = page_manager.allocate_page();
    const std::uint64_t page3 = page_manager.allocate_page();

    BufferPoolManager pool(page_manager, log_manager, 2);

    Frame &frame1 = pool.fetch_page(page1);
    frame1.data[100] = std::byte{0x11}; //making page dirty
    pool.unpin_page(page1, true);

    Frame &frame2 = pool.fetch_page(page2);
    assert(frame2.page_id == page2);
    pool.unpin_page(page2, false);

    //Make page1 more recently used wrt page2
    Frame &again1 = pool.fetch_page(page1);
    pool.unpin_page(page1, false);

    //both Frames are unpinned now, LRU victim = Page2
    Frame &frame3 = pool.fetch_page(page3);
    assert(frame3.page_id == page3);
    pool.unpin_page(page3, false);

    //Page1 should still be in cache, only page2 was evicted
    Frame &cached = pool.fetch_page(page1);
    assert(&cached == &again1);
}

void test_pinned_pages_are_not_evicted() {

    std::filesystem::remove("build/buffer_pool_pinned.db");

    DiskManager disk("build/buffer_pool_pinned.db");
    DatabasePageManager page_manager(disk);

    wal::LogManager log_manager("build/buffer_pool_pinned.wal");
    log_manager.open(); //to prevent overwriting next_lsn_

    const std::uint64_t page1 = page_manager.allocate_page();
    const std::uint64_t page2 = page_manager.allocate_page();
    const std::uint64_t page3 = page_manager.allocate_page();

    BufferPoolManager pool(page_manager, log_manager, 2);

    pool.fetch_page(page1);
    pool.fetch_page(page2);

    // Both frames are pinned. No frame can be evicted.
    bool throw_exception = false;

    try {
        pool.fetch_page(page3);
    } catch (const std::runtime_error &) {
        throw_exception = true;
    }
    assert(throw_exception);

    //Release both pages
    pool.unpin_page(page1, false);
    pool.unpin_page(page2, false);

    std::cout << "[PASS] pinned pages are not evicted\n";
}

void test_dirty_page_survives_eviction() {

    std::filesystem::remove("build/buffer_pool_dirty.db");

    DiskManager disk("build/buffer_pool_dirty.db");
    DatabasePageManager page_manager(disk);

    wal::LogManager log_manager("build/buffer_pool_dirty.wal");
    log_manager.open(); //to prevent overwriting next_lsn_

    const std::uint64_t page1 = page_manager.allocate_page();
    const std::uint64_t page2 = page_manager.allocate_page();

    BufferPoolManager pool(page_manager, log_manager, 1);

    Frame &frame1 = pool.fetch_page(page1);
    frame1.data[100] = std::byte{0x5A};

    pool.unpin_page(page1, true);

    // The one-frame pool must evict page1 to load page2.
    pool.fetch_page(page2);
    pool.unpin_page(page2, false);

    // Fetch page1 again. Its data should come from disk.
    Frame &reloaded = pool.fetch_page(page1);

    assert(reloaded.data[100] == std::byte{0x5A});

    pool.unpin_page(page1, false);
    std::cout << "[PASS] dirty page survives eviction\n";
}

void test_flush_page() {

    std::filesystem::remove("build/buffer_pool_flush.db");

    DiskManager disk("build/buffer_pool_flush.db");
    DatabasePageManager page_manager(disk);

    wal::LogManager log_manager("build/buffer_pool_flush.wal");
    log_manager.open(); //to prevent overwriting next_lsn_

    const std::uint64_t page1 = page_manager.allocate_page();

    BufferPoolManager pool(page_manager, log_manager, 1);

    Frame& frame = pool.fetch_page(page1);
    frame.data[100] = std::byte{0x7B};

    pool.unpin_page(page1, true);

    pool.flush_page(page1);

    assert(!frame.is_dirty);

    // Fetching page2 forces page1 out of the one-frame buffer pool
    const std::uint64_t page2 = page_manager.allocate_page();

    Frame& second = pool.fetch_page(page2);
    assert(second.page_id == page2);
    pool.unpin_page(page2, false);

    // Reload page1 from disk and verify that the flushed data persists
    Frame& reloaded = pool.fetch_page(page1);

    assert(reloaded.data[100] == std::byte{0x7B});

    pool.unpin_page(page1, false);
    pool.flush_all_pages();

    std::cout << "[PASS] flush dirty page\n";
}

void test_flush_all_pages() {

    std::filesystem::remove("build/buffer_pool_flush_all.db");

    DiskManager disk("build/buffer_pool_flush_all.db");
    DatabasePageManager page_manager(disk);

    wal::LogManager log_manager("build/buffer_pool_flush_all.wal");
    log_manager.open(); //to prevent overwriting next_lsn_

    const std::uint64_t page1 = page_manager.allocate_page();
    const std::uint64_t page2 = page_manager.allocate_page();

    BufferPoolManager pool(page_manager, log_manager, 2);

    Frame& frame1 = pool.fetch_page(page1);
    frame1.data[100] = std::byte{0x12};
    pool.unpin_page(page1, true);

    Frame& frame2 = pool.fetch_page(page2);
    frame2.data[200] = std::byte{0x34};
    pool.unpin_page(page2, true);

    pool.flush_all_pages();

    assert(!frame1.is_dirty);
    assert(!frame2.is_dirty);

    // Evict both pages so that subsequent fetches must read from storage.
    const std::uint64_t page3 = page_manager.allocate_page();
    const std::uint64_t page4 = page_manager.allocate_page();

    pool.fetch_page(page3);
    pool.unpin_page(page3, false);

    pool.fetch_page(page4);
    pool.unpin_page(page4, false);

    Frame& reloaded1 = pool.fetch_page(page1);
    assert(reloaded1.data[100] == std::byte{0x12});
    pool.unpin_page(page1, false);

    Frame& reloaded2 = pool.fetch_page(page2);
    assert(reloaded2.data[200] == std::byte{0x34});
    pool.unpin_page(page2, false);

    pool.flush_all_pages();

    std::cout << "PASS: flush all dirty pages\n";
}

void test_failed_fetch_preserves_cached_page() {

    std::filesystem::remove("build/buffer_pool_failed_fetch.db");

    DiskManager disk("build/buffer_pool_failed_fetch.db");
    DatabasePageManager page_manager(disk);

    wal::LogManager log_manager("build/buffer_pool_failed_fetch.wal");
    log_manager.open(); //to prevent overwriting next_lsn_

    const std::uint64_t page1 = page_manager.allocate_page();
    BufferPoolManager pool(page_manager, log_manager, 1);

    Frame& cached = pool.fetch_page(page1);
    cached.data[100] = std::byte{0x42};

    pool.unpin_page(page1, true);

    bool throw_exception = false;
    try {
        // This ID has not been allocated by PageManager using allocate_page()
        pool.fetch_page(999999);
    } catch (const std::exception&) {
        throw_exception = true;
    }
    assert(throw_exception);

    //The original page must still be available in the cache.
    Frame& fetched_again = pool.fetch_page(page1);

    assert(&cached == &fetched_again);
    assert(fetched_again.data[100] == std::byte{0x42});

    pool.unpin_page(page1, false);
    std::cout << "[PASS] failed-fetch preserves cached page\n";
}

void test_flush_clean_page() {

    std::filesystem::remove("build/buffer_pool_flush_clean.db");

    DiskManager disk("build/buffer_pool_flush_clean.db");
    DatabasePageManager page_manager(disk);

    wal::LogManager log_manager("build/buffer_pool_flush_clean.wal");
    log_manager.open(); //to prevent overwriting next_lsn_

    const std::uint64_t page_id = page_manager.allocate_page();

    BufferPoolManager pool(page_manager, log_manager, 2);

    Frame& frame = pool.fetch_page(page_id);

    assert(!frame.is_dirty);

    pool.flush_page(page_id);

    assert(!frame.is_dirty);

    pool.unpin_page(page_id, false);

    std::cout << "[PASS] flushing clean page\n";
}

wal::LogRecord make_page_update_record(std::uint64_t page_id) {
    wal::LogRecord record;
    record.type = wal::LogRecordType::PAGE_UPDATE;
    record.page_id = page_id;
    record.payload = {std::byte{0x42}};
    return record;
}

void test_page_lsn_updated_on_unpin() {
    DiskManager disk("build/buffer_pool_page_lsn.db");
    DatabasePageManager page_manager(disk);

    wal::LogManager log_manager("build/buffer_pool_page_lsn.wal");
    log_manager.open();

    const auto page_id = page_manager.allocate_page();

    BufferPoolManager pool(page_manager, log_manager, 1);

    Frame &frame = pool.fetch_page(page_id);

    auto record = make_page_update_record(page_id);
    (void)log_manager.append(record);

    pool.unpin_page(page_id, true, record.lsn);

    assert(format::read_page_lsn(frame.data) == record.lsn);

    std::cout << "[PASS] PageLSN updated on unpin\n";
}

void test_wal_flush_on_flush_page() {

    const std::string db_path = "build/buffer_pool_wal_flush.db";
    const std::string wal_path = "build/buffer_pool_wal_flush.wal";

    std::filesystem::remove(db_path);
    std::filesystem::remove(wal_path);

    DiskManager disk(db_path);
    DatabasePageManager page_manager(disk);

    wal::LogManager log_manager(wal_path);
    log_manager.open();

    const auto page_id = page_manager.allocate_page();

    BufferPoolManager pool(page_manager, log_manager, 1);

    Frame &frame = pool.fetch_page(page_id);

    auto record = make_page_update_record(page_id);
    (void)log_manager.append(record);

    frame.data[100] = std::byte{0x7B};

    pool.unpin_page(page_id, true, record.lsn);

    pool.flush_page(page_id);

    assert(!frame.is_dirty);

    assert(std::filesystem::file_size(wal_path) > 0);

    auto persisted_page = page_manager.read_page(page_id);

    assert(format::read_page_lsn(persisted_page) == record.lsn);
    assert(persisted_page[100] == std::byte{0x7B});

    std::cout << "[PASS] WAL flush on flush_page\n";
}

void test_wal_flush_on_flush_all_pages() {

    const std::string db_path = "build/buffer_pool_wal_flush_all.db";
    const std::string wal_path = "build/buffer_pool_wal_flush_all.wal";

    std::filesystem::remove(db_path);
    std::filesystem::remove(wal_path);

    DiskManager disk(db_path);
    DatabasePageManager page_manager(disk);

    wal::LogManager log_manager(wal_path);
    log_manager.open();

    const auto page_id = page_manager.allocate_page();

    BufferPoolManager pool(page_manager, log_manager, 2);

    Frame &frame = pool.fetch_page(page_id);

    auto record = make_page_update_record(page_id);
    log_manager.append(record);

    frame.data[100] = std::byte{0x7B};

    pool.unpin_page(page_id, true, record.lsn);

    pool.flush_all_pages();

    assert(!frame.is_dirty);

    assert(std::filesystem::file_size(wal_path) > 0);

    auto persisted_page = page_manager.read_page(page_id);

    assert(format::read_page_lsn(persisted_page) == record.lsn);
    assert(persisted_page[100] == std::byte{0x7B});

    std::cout << "[PASS] WAL flush on flush_all_pages\n";
}

void test_wal_flush_on_page_eviction() {

    const std::string db_path = "build/buffer_pool_wal_eviction.db";
    const std::string wal_path = "build/buffer_pool_wal_eviction.wal";

    std::filesystem::remove(db_path);
    std::filesystem::remove(wal_path);

    DiskManager disk(db_path);
    DatabasePageManager page_manager(disk);

    wal::LogManager log_manager(wal_path);
    log_manager.open();

    const auto page_id = page_manager.allocate_page();
    const auto second_page_id = page_manager.allocate_page();

    BufferPoolManager pool(page_manager, log_manager, 1);

    Frame &frame = pool.fetch_page(page_id);

    auto record = make_page_update_record(page_id);
    (void)log_manager.append(record);

    frame.data[100] = std::byte{0x7B};

    pool.unpin_page(page_id, true, record.lsn);

    // Fetching another page forces eviction of the dirty page.
    Frame &second_frame = pool.fetch_page(second_page_id);
    pool.unpin_page(second_page_id, false);

    assert(std::filesystem::file_size(wal_path) > 0);

    auto persisted_page = page_manager.read_page(page_id);

    assert(format::read_page_lsn(persisted_page) == record.lsn);
    assert(persisted_page[100] == std::byte{0x7B});

    std::cout << "[PASS] WAL flush on page eviction\n";
}

void test_page_lsn_cannot_move_backwards() {
    DiskManager disk("build/buffer_pool_page_lsn_backwards.db");
    DatabasePageManager page_manager(disk);

    wal::LogManager log_manager("build/buffer_pool_page_lsn_backwards.wal");
    log_manager.open();

    const auto page_id = page_manager.allocate_page();

    BufferPoolManager pool(page_manager, log_manager, 1);

    Frame &frame = pool.fetch_page(page_id);

    format::write_page_lsn(frame.data, 100);

    bool exception_thrown = false;

    try {
        pool.unpin_page(page_id, true, 99);
    } catch (const std::runtime_error &) {
        exception_thrown = true;
    }

    assert(exception_thrown);
    assert(frame.pin_count == 1);

    pool.unpin_page(page_id, false);

    std::cout << "[PASS] PageLSN cannot move backwards\n";
}

int main() {
    test_valid_capacity();
    test_zero_capacity_rejected();

    test_fetch_page_cache_hit();
    test_fetch_page_cache_miss();

    test_unpin_page();
    test_unpin_page_with_zero_pin_count();

    test_lru_eviction();
    test_pinned_pages_are_not_evicted();
    test_dirty_page_survives_eviction();

    test_flush_page();
    test_flush_all_pages();

    test_failed_fetch_preserves_cached_page();
    test_flush_clean_page();

    test_page_lsn_updated_on_unpin();
    test_page_lsn_cannot_move_backwards();

    test_wal_flush_on_page_eviction();
    test_wal_flush_on_flush_page();
    test_wal_flush_on_flush_all_pages();


    std::cout << "[PASS] All Buffer Pool tests passed.\n";
    return 0;
}