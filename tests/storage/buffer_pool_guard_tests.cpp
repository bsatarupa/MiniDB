#include "minidb/storage/buffer_pool_manager.h"
#include "minidb/storage/disk_manager.h"
#include "minidb/storage/database_page_manager.h"
#include "minidb/storage/wal/log_manager.h"

#include <cassert>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <iostream>
#include <thread>
#include <utility>

using namespace minidb::storage;

namespace {

using namespace std::chrono_literals;

// ------------------------------------------------------------
// Test environment
// ------------------------------------------------------------

struct TestEnvironment {
    std::string db_path;
    std::string wal_path;

    DiskManager disk;
    DatabasePageManager page_manager;
    wal::LogManager log_manager;
    BufferPoolManager pool;

    static std::string prepare_db(const std::string& name) {
        std::string path = "build/" + name + ".db";
        std::filesystem::remove(path);
        return path;
    }

    static std::string prepare_wal(const std::string& name) {
        std::string path = "build/" + name + ".wal";
        std::filesystem::remove(path);
        return path;
    }

    explicit TestEnvironment(const std::string& name, std::size_t capacity = 4)
        : db_path(prepare_db(name)),
          wal_path(prepare_wal(name)),
          disk(db_path),
          page_manager(disk),
          log_manager(wal_path),
          pool(page_manager, log_manager, capacity) {
        log_manager.open();
    }

    std::uint64_t allocate_page() {
        return page_manager.allocate_page();
    }
};

// ------------------------------------------------------------
// 1. Default guards
// ------------------------------------------------------------

void test_default_read_guard() {
    ReadPageGuard guard;
    std::cout << "[PASS] default read guard\n";
}

void test_default_write_guard() {
    WritePageGuard guard;
    std::cout << "[PASS] default write guard\n";
}

// ------------------------------------------------------------
// 2. ReadPageGuard
// ------------------------------------------------------------

void test_read_guard_fetch_and_access() {
    TestEnvironment env("guard_read_access");
    const auto page_id = env.allocate_page();

    {
        auto guard = env.pool.fetch_page_read(page_id);
        (void)guard.page();
    }

    std::cout << "[PASS] read guard fetch and access\n";
}

void test_read_guard_move_constructor() {
    TestEnvironment env("guard_read_move_ctor");
    const auto page_id = env.allocate_page();

    {
        auto guard1 = env.pool.fetch_page_read(page_id);
        auto guard2 = std::move(guard1);

        (void)guard2.page();
    }

    std::cout << "[PASS] read guard move constructor\n";
}

void test_read_guard_move_assignment() {
    TestEnvironment env("guard_read_move_assignment");
    const auto page1 = env.allocate_page();
    const auto page2 = env.allocate_page();

    {
        auto guard1 = env.pool.fetch_page_read(page1);
        auto guard2 = env.pool.fetch_page_read(page2);

        guard2 = std::move(guard1);

        (void)guard2.page();
    }

    std::cout << "[PASS] read guard move assignment\n";
}

void test_read_guard_repeated_move() {
    TestEnvironment env("guard_read_repeated_move");
    const auto page_id = env.allocate_page();

    {
        auto guard1 = env.pool.fetch_page_read(page_id);
        auto guard2 = std::move(guard1);
        auto guard3 = std::move(guard2);
        auto guard4 = std::move(guard3);

        (void)guard4.page();
    }

    std::cout << "[PASS] read guard repeated move\n";
}

void test_read_guard_move_assignment_releases_destination() {
    TestEnvironment env("guard_read_move_release");
    const auto page1 = env.allocate_page();
    const auto page2 = env.allocate_page();

    {
        auto guard1 = env.pool.fetch_page_read(page1);
        auto guard2 = env.pool.fetch_page_read(page2);

        guard2 = std::move(guard1);
    }

    // Both pages must have been unpinned exactly once.
    Frame& frame1 = env.pool.fetch_page(page1);
    assert(frame1.pin_count == 1);
    env.pool.unpin_page(page1, false);

    Frame& frame2 = env.pool.fetch_page(page2);
    assert(frame2.pin_count == 1);
    env.pool.unpin_page(page2, false);

    std::cout << "[PASS] read guard move assignment releases destination\n";
}

void test_read_guard_destruction_unpins() {
    TestEnvironment env("guard_read_unpin");
    const auto page_id = env.allocate_page();

    {
        auto guard = env.pool.fetch_page_read(page_id);
    }

    Frame& frame = env.pool.fetch_page(page_id);
    assert(frame.pin_count == 1);
    env.pool.unpin_page(page_id, false);

    std::cout << "[PASS] read guard destruction unpins\n";
}

// ------------------------------------------------------------
// 3. WritePageGuard
// ------------------------------------------------------------

void test_write_guard_fetch_and_access() {
    TestEnvironment env("guard_write_access");
    const auto page_id = env.allocate_page();

    {
        auto guard = env.pool.fetch_page_write(page_id);
        guard.page()[100] = std::byte{0x42};
        guard.mark_dirty();
    }

    std::cout << "[PASS] write guard fetch and access\n";
}

void test_write_guard_clean_without_mark_dirty() {
    TestEnvironment env("guard_write_clean");
    const auto page_id = env.allocate_page();

    {
        auto guard = env.pool.fetch_page_write(page_id);
        guard.page()[100] = std::byte{0x42};
        // Intentionally do not call mark_dirty().
    }

    Frame& frame = env.pool.fetch_page(page_id);
    assert(!frame.is_dirty);
    env.pool.unpin_page(page_id, false);

    std::cout << "[PASS] write guard remains clean without mark_dirty\n";
}

void test_write_guard_dirty_propagation() {
    TestEnvironment env("guard_write_dirty");
    const auto page_id = env.allocate_page();

    {
        auto guard = env.pool.fetch_page_write(page_id);
        guard.page()[100] = std::byte{0x42};
        guard.mark_dirty();
    }

    Frame& frame = env.pool.fetch_page(page_id);
    assert(frame.is_dirty);
    env.pool.unpin_page(page_id, false);

    std::cout << "[PASS] write guard dirty propagation\n";
}

void test_write_guard_move_constructor() {
    TestEnvironment env("guard_write_move_ctor");
    const auto page_id = env.allocate_page();

    {
        auto guard1 = env.pool.fetch_page_write(page_id);
        guard1.page()[100] = std::byte{0x42};
        guard1.mark_dirty();

        auto guard2 = std::move(guard1);
        (void)guard2.page();
    }

    std::cout << "[PASS] write guard move constructor\n";
}

void test_write_guard_move_assignment() {
    TestEnvironment env("guard_write_move_assignment");
    const auto page1 = env.allocate_page();
    const auto page2 = env.allocate_page();

    {
        auto guard1 = env.pool.fetch_page_write(page1);
        auto guard2 = env.pool.fetch_page_write(page2);

        guard1.page()[100] = std::byte{0x11};
        guard1.mark_dirty();

        guard2.page()[100] = std::byte{0x22};
        guard2.mark_dirty();

        guard2 = std::move(guard1);
        (void)guard2.page();
    }

    std::cout << "[PASS] write guard move assignment\n";
}

void test_write_guard_repeated_move() {
    TestEnvironment env("guard_write_repeated_move");
    const auto page_id = env.allocate_page();

    {
        auto guard1 = env.pool.fetch_page_write(page_id);
        guard1.mark_dirty();

        auto guard2 = std::move(guard1);
        auto guard3 = std::move(guard2);
        auto guard4 = std::move(guard3);

        guard4.page()[100] = std::byte{0x42};
    }

    std::cout << "[PASS] write guard repeated move\n";
}

void test_write_guard_move_assignment_releases_destination() {
    TestEnvironment env("guard_write_move_release");
    const auto page1 = env.allocate_page();
    const auto page2 = env.allocate_page();

    {
        auto guard1 = env.pool.fetch_page_write(page1);
        auto guard2 = env.pool.fetch_page_write(page2);

        guard1.page()[100] = std::byte{0x11};
        guard1.mark_dirty();

        guard2.page()[100] = std::byte{0x22};
        guard2.mark_dirty();

        guard2 = std::move(guard1);
    }

    Frame& frame1 = env.pool.fetch_page(page1);
    assert(frame1.pin_count == 1);
    assert(frame1.is_dirty);
    env.pool.unpin_page(page1, false);

    Frame& frame2 = env.pool.fetch_page(page2);
    assert(frame2.pin_count == 1);
    assert(frame2.is_dirty);
    env.pool.unpin_page(page2, false);

    std::cout << "[PASS] write guard move assignment releases destination\n";
}

void test_write_guard_destruction_unpins() {
    TestEnvironment env("guard_write_unpin");
    const auto page_id = env.allocate_page();

    {
        auto guard = env.pool.fetch_page_write(page_id);
        guard.page()[100] = std::byte{0x42};
        guard.mark_dirty();
    }

    Frame& frame = env.pool.fetch_page(page_id);
    assert(frame.pin_count == 1);
    assert(frame.is_dirty);
    env.pool.unpin_page(page_id, false);

    std::cout << "[PASS] write guard destruction unpins\n";
}

// ------------------------------------------------------------
// 4. Read/write latch blocking
// ------------------------------------------------------------

void test_reader_blocks_writer() {
    TestEnvironment env("guard_reader_blocks_writer");
    const auto page_id = env.allocate_page();

    auto read_guard = env.pool.fetch_page_read(page_id);

    std::promise<void> writer_started;
    std::promise<void> writer_acquired;
    auto acquired_future = writer_acquired.get_future();

    std::thread writer([&] {
        writer_started.set_value();

        auto write_guard = env.pool.fetch_page_write(page_id);
        writer_acquired.set_value();
    });

    writer_started.get_future().wait();

    assert(acquired_future.wait_for(100ms) == std::future_status::timeout);

    read_guard = ReadPageGuard{};

    assert(acquired_future.wait_for(2s) == std::future_status::ready);

    writer.join();

    std::cout << "[PASS] reader blocks writer\n";
}

void test_writer_blocks_reader() {
    TestEnvironment env("guard_writer_blocks_reader");
    const auto page_id = env.allocate_page();

    auto write_guard = env.pool.fetch_page_write(page_id);

    std::promise<void> reader_started;
    std::promise<void> reader_acquired;
    auto acquired_future = reader_acquired.get_future();

    std::thread reader([&] {
        reader_started.set_value();

        auto read_guard = env.pool.fetch_page_read(page_id);
        reader_acquired.set_value();
    });

    reader_started.get_future().wait();

    assert(acquired_future.wait_for(100ms) == std::future_status::timeout);

    write_guard = WritePageGuard{};

    assert(acquired_future.wait_for(2s) == std::future_status::ready);

    reader.join();

    std::cout << "[PASS] writer blocks reader\n";
}

void test_multiple_writers_are_serialized() {
    TestEnvironment env("guard_multiple_writers");
    const auto page_id = env.allocate_page();

    std::atomic<int> active_writers{0};
    std::atomic<int> max_active_writers{0};

    auto writer_task = [&] {
        auto guard = env.pool.fetch_page_write(page_id);

        const int active = ++active_writers;

        int current_max = max_active_writers.load();
        while (active > current_max &&
               !max_active_writers.compare_exchange_weak(current_max, active)) {
        }

        std::this_thread::sleep_for(50ms);

        --active_writers;
    };

    std::thread writer1(writer_task);
    std::thread writer2(writer_task);
    std::thread writer3(writer_task);

    writer1.join();
    writer2.join();
    writer3.join();

    assert(max_active_writers.load() == 1);

    std::cout << "[PASS] multiple writers serialized\n";
}

// ------------------------------------------------------------
// 5. Pin lifecycle and data persistence
// ------------------------------------------------------------

void test_guard_pin_lifecycle() {
    TestEnvironment env("guard_pin_lifecycle");
    const auto page_id = env.allocate_page();

    {
        auto read_guard = env.pool.fetch_page_read(page_id);

        Frame& frame = env.pool.fetch_page(page_id);
        assert(frame.pin_count == 2);
        env.pool.unpin_page(page_id, false);
    }

    Frame& frame = env.pool.fetch_page(page_id);
    assert(frame.pin_count == 1);
    env.pool.unpin_page(page_id, false);

    std::cout << "[PASS] guard pin lifecycle\n";
}

void test_guard_move_does_not_leak_pin() {
    TestEnvironment env("guard_move_no_pin_leak");
    const auto page_id = env.allocate_page();

    {
        auto guard1 = env.pool.fetch_page_read(page_id);
        auto guard2 = std::move(guard1);
        auto guard3 = std::move(guard2);
    }

    Frame& frame = env.pool.fetch_page(page_id);
    assert(frame.pin_count == 1);
    env.pool.unpin_page(page_id, false);

    std::cout << "[PASS] guard move does not leak pin\n";
}

void test_write_guard_data_persistence() {
    TestEnvironment env("guard_write_persistence");
    const auto page_id = env.allocate_page();

    {
        auto guard = env.pool.fetch_page_write(page_id);
        guard.page()[100] = std::byte{0x5A};
        guard.mark_dirty();
    }

    env.pool.flush_page(page_id);

    auto persisted = env.page_manager.read_page(page_id);
    assert(persisted[100] == std::byte{0x5A});

    std::cout << "[PASS] write guard data persistence\n";
}

void test_multiple_readers_access_same_page() {
    TestEnvironment env("guard_multiple_readers");
    const auto page_id = env.allocate_page();

    constexpr int NUM_READERS = 4;

    std::atomic<int> active_readers{0};
    std::atomic<int> max_active_readers{0};

    auto reader_task = [&] {
        auto guard = env.pool.fetch_page_read(page_id);

        const int active = ++active_readers;

        int current_max = max_active_readers.load();

        while (active > current_max &&
               !max_active_readers.compare_exchange_weak(
                   current_max, active)) {
        }

        std::this_thread::sleep_for(100ms);

        --active_readers;
    };

    std::vector<std::thread> readers;

    for (int i = 0; i < NUM_READERS; ++i) {
        readers.emplace_back(reader_task);
    }

    for (auto &reader : readers) {
        reader.join();
    }

    assert(max_active_readers.load() == NUM_READERS);

    std::cout << "[PASS] multiple readers access same page concurrently\n";
}

} // namespace

// ------------------------------------------------------------
// Main
// ------------------------------------------------------------

int main() {
    test_default_read_guard();
    test_default_write_guard();

    test_read_guard_fetch_and_access();
    test_read_guard_move_constructor();
    test_read_guard_move_assignment();
    test_read_guard_repeated_move();
    test_read_guard_move_assignment_releases_destination();
    test_read_guard_destruction_unpins();

    test_write_guard_fetch_and_access();
    test_write_guard_clean_without_mark_dirty();
    test_write_guard_dirty_propagation();
    test_write_guard_move_constructor();
    test_write_guard_move_assignment();
    test_write_guard_repeated_move();
    test_write_guard_move_assignment_releases_destination();
    test_write_guard_destruction_unpins();

    test_reader_blocks_writer();
    test_writer_blocks_reader();
    test_multiple_writers_are_serialized();

    test_guard_pin_lifecycle();
    test_guard_move_does_not_leak_pin();
    test_write_guard_data_persistence();

    test_multiple_readers_access_same_page();

    std::cout << "\n[PASS] All PageGuard tests passed.\n";
    return 0;
}