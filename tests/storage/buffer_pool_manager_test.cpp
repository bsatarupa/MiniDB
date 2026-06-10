#include "minidb/storage/buffer_pool_manager.h"
#include "minidb/storage/disk_manager.h"
#include "minidb/storage/database_page_manager.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <cassert>
#include <stdexcept>

using namespace minidb::storage;

void test_valid_capacity() {
    DiskManager disk("build/buffer_pool_valid.db");
    DatabasePageManager page_manager(disk);

    BufferPoolManager pool(page_manager, 10);
    std::cout << "[PASS] valid capacity\n";
}

void test_zero_capacity_rejected() {
    DiskManager disk("build/buffer_pool_zero.db");
    DatabasePageManager page_manager(disk);

    bool exception_thrown = false;

    try {
        BufferPoolManager pool(page_manager, 0);
    } catch (const std::invalid_argument&) {
        exception_thrown = true;
    }

    assert(exception_thrown); //if exception_thrown == F, assert() aborts with cerr message
    std::cout << "[PASS] zero capacity rejected\n";
}

void test_fetch_page_cache_miss() {

    DiskManager disk("build/buffer_pool_cache_miss.db");
    DatabasePageManager page_manager(disk);

    //Since PageManager only allows reads of 'allocated' pages
    std::uint64_t page_id = page_manager.allocate_page();

    BufferPoolManager pool(page_manager, 2);
    Frame &frame = pool.fetch_page(page_id);

    assert(frame.is_occupied);
    assert(!frame.is_dirty);
    assert(frame.pin_count == 1);
    assert(frame.page_id == page_id);

    std::cout << "[PASS] fetch page on cache miss\n";
}

void test_fetch_page_cache_hit() {

    DiskManager disk("build/buffer_pool_cache_miss.db");
    DatabasePageManager page_manager(disk);

    //Since PageManager only allows reads of 'allocated' pages
    std::uint64_t page_id = page_manager.allocate_page();

    BufferPoolManager pool(page_manager, 2);

    Frame &frame1 = pool.fetch_page(page_id); //1st fetch encounters a cache miss and loads the page
    Frame &frame2 = pool.fetch_page(page_id); //2nd fetch encounters a cache hit and reuses the existing in-memory frame

    //both fetch should refer to the SAME cached frame
    assert(&frame1 == &frame2);

    //each fetch pins the page
    assert(frame1.pin_count == 2);

    std::cout << "[PASS] fetch page on cache hit\n";
}

void test_unpin_page() {
    DiskManager disk("build/buffer_pool_unpin.db");
    DatabasePageManager page_manager(disk);

    std::uint64_t page_id = page_manager.allocate_page();

    BufferPoolManager pool(page_manager, 2);

    Frame& frame = pool.fetch_page(page_id);

    assert(frame.pin_count == 1);
    assert(!frame.is_dirty);

    pool.unpin_page(page_id, true);

    assert(frame.pin_count == 0);
    assert(frame.is_dirty);

    std::cout << "PASS: unpin page and mark dirty\n";
}

void test_unpin_page_with_zero_pin_count() {
    DiskManager disk("build/buffer_pool_invalid_unpin.db");
    DatabasePageManager page_manager(disk);

    std::uint64_t page_id = page_manager.allocate_page();

    BufferPoolManager pool(page_manager, 2);

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
    DiskManager disk("build/buffer_pool_lru.db");
    DatabasePageManager page_manager(disk);

    const std::uint64_t page1 = page_manager.allocate_page();
    const std::uint64_t page2 = page_manager.allocate_page();
    const std::uint64_t page3 = page_manager.allocate_page();

    BufferPoolManager pool(page_manager, 2);

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
    DiskManager disk("build/buffer_pool_pinned.db");
    DatabasePageManager page_manager(disk);

    const std::uint64_t page1 = page_manager.allocate_page();
    const std::uint64_t page2 = page_manager.allocate_page();
    const std::uint64_t page3 = page_manager.allocate_page();

    BufferPoolManager pool(page_manager, 2);

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
    DiskManager disk("build/buffer_pool_dirty.db");
    DatabasePageManager page_manager(disk);

    const std::uint64_t page1 = page_manager.allocate_page();
    const std::uint64_t page2 = page_manager.allocate_page();

    BufferPoolManager pool(page_manager, 1);

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
    DiskManager disk("build/buffer_pool_flush.db");
    DatabasePageManager page_manager(disk);

    const std::uint64_t page1 = page_manager.allocate_page();

    BufferPoolManager pool(page_manager, 1);

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
    DiskManager disk("build/buffer_pool_flush_all.db");
    DatabasePageManager page_manager(disk);

    const std::uint64_t page1 = page_manager.allocate_page();
    const std::uint64_t page2 = page_manager.allocate_page();

    BufferPoolManager pool(page_manager, 2);

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

    DiskManager disk("build/buffer_pool_failed_fetch.db");
    DatabasePageManager page_manager(disk);

    const std::uint64_t page1 = page_manager.allocate_page();
    BufferPoolManager pool(page_manager, 1);

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
    DiskManager disk("build/buffer_pool_flush_clean.db");
    DatabasePageManager page_manager(disk);

    const std::uint64_t page_id = page_manager.allocate_page();

    BufferPoolManager pool(page_manager, 2);

    Frame& frame = pool.fetch_page(page_id);

    assert(!frame.is_dirty);

    pool.flush_page(page_id);

    assert(!frame.is_dirty);

    pool.unpin_page(page_id, false);

    std::cout << "[PASS] flushing clean page\n";
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

    std::cout << "[PASS] All Buffer Pool tests passed.\n";
    return 0;
}