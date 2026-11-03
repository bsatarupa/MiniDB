
#include "minidb/storage/wal/log_manager.h"
#include "minidb/storage/wal/log_record.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace minidb::storage::wal;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Exception, typename Function>
void expect_throw(Function function, const std::string& message) {
    try {
        function();
    }
    catch (const Exception&) {
        return;
    }

    throw std::runtime_error(
        message + ": expected exception was not thrown"
    );
}

std::filesystem::path make_test_file() {
    const auto timestamp =
        std::chrono::steady_clock::now().time_since_epoch().count();

    return std::filesystem::temp_directory_path() /
        ("minidb_wal_test_" + std::to_string(timestamp) + ".log");
}

LogRecord make_record(LogRecordType type) {
    LogRecord record;
    record.type = type;
    record.txn_id = 10;
    record.page_id = 5;
    record.payload = {
        std::byte{0x11},
        std::byte{0x22},
        std::byte{0x33}
    };

    return record;
}

std::size_t serialized_size(const LogRecord& record) {
    return LogRecordSerializer::serialize(record).size();
}

// Test 1: WAL file creation
void test_wal_file_creation() {
    const auto path = make_test_file();

    {
        LogManager manager(path);
        manager.open();

        require(
            std::filesystem::exists(path),
            "WAL file was not created"
        );

        manager.close();
    }

    std::filesystem::remove(path);
}

// Test 2: Opening an already-open WAL
void test_open_twice() {
    const auto path = make_test_file();

    {
        LogManager manager(path);
        manager.open();
        manager.open();

        require(
            std::filesystem::exists(path),
            "WAL file does not exist"
        );

        manager.close();
    }

    std::filesystem::remove(path);
}

// Test 3: First LSN allocation
void test_first_lsn() {
    const auto path = make_test_file();

    {
        LogManager manager(path);
        manager.open();

        auto record = make_record(LogRecordType::BEGIN);

        const LSN lsn = manager.append(record);

        require(lsn == 0, "First LSN should be zero");
        require(record.lsn == 0, "Record LSN was not assigned");

        manager.close();
    }

    std::filesystem::remove(path);
}

// Test 4: Sequential LSN allocation
void test_sequential_lsn_allocation() {
    const auto path = make_test_file();

    {
        LogManager manager(path);
        manager.open();

        auto first = make_record(LogRecordType::BEGIN);
        auto second = make_record(LogRecordType::PAGE_UPDATE);
        auto third = make_record(LogRecordType::COMMIT);

        const LSN first_lsn = manager.append(first);
        const LSN second_lsn = manager.append(second);
        const LSN third_lsn = manager.append(third);

        require(first_lsn == 0, "First LSN mismatch");

        require(
            second_lsn == serialized_size(first),
            "Second LSN mismatch"
        );

        require(
            third_lsn == serialized_size(first) +
                         serialized_size(second),
            "Third LSN mismatch"
        );

        manager.flush();
        manager.close();
    }

    std::filesystem::remove(path);
}

// Test 5: Next LSN tracking
void test_get_next_lsn() {
    const auto path = make_test_file();

    {
        LogManager manager(path);
        manager.open();

        auto record = make_record(LogRecordType::BEGIN);

        (void)manager.append(record);

        require(
            manager.get_next_lsn() == serialized_size(record),
            "Next LSN mismatch"
        );

        manager.flush();
        manager.close();
    }

    std::filesystem::remove(path);
}

// Test 6: Buffering without flushing
void test_buffering_without_flush() {
    const auto path = make_test_file();

    {
        LogManager manager(path);
        manager.open();

        auto record = make_record(LogRecordType::BEGIN);
        (void)manager.append(record);

        require(
            std::filesystem::file_size(path) == 0,
            "WAL data was written before flush"
        );

        manager.close();
    }

    std::filesystem::remove(path);
}

// Test 7: Flush writes buffered data
void test_flush_writes_data() {
    const auto path = make_test_file();

    {
        LogManager manager(path);
        manager.open();

        auto first = make_record(LogRecordType::BEGIN);
        auto second = make_record(LogRecordType::PAGE_UPDATE);

        (void)manager.append(first);
        (void)manager.append(second);

        const auto expected_size =
            serialized_size(first) + serialized_size(second);

        manager.flush();

        require(
            std::filesystem::file_size(path) == expected_size,
            "WAL file size mismatch after flush"
        );

        manager.close();
    }

    std::filesystem::remove(path);
}

// Test 8: Flush empty buffer
void test_flush_empty_buffer() {
    const auto path = make_test_file();

    {
        LogManager manager(path);
        manager.open();

        manager.flush();

        require(
            std::filesystem::file_size(path) == 0,
            "Empty flush unexpectedly wrote data"
        );

        manager.close();
    }

    std::filesystem::remove(path);
}

// Test 9: Multiple flushes
void test_multiple_flushes() {
    const auto path = make_test_file();

    {
        LogManager manager(path);
        manager.open();

        auto first = make_record(LogRecordType::BEGIN);
        (void)manager.append(first);
        manager.flush();

        const auto first_size = serialized_size(first);

        auto second = make_record(LogRecordType::COMMIT);
        (void)manager.append(second);
        manager.flush();

        require(
            std::filesystem::file_size(path) ==
                first_size + serialized_size(second),
            "Multiple flushes produced incorrect file size"
        );

        manager.close();
    }

    std::filesystem::remove(path);
}

// Test 10: Reopen existing WAL and continue LSN allocation
void test_reopen_wal() {
    const auto path = make_test_file();

    std::size_t expected_next_lsn = 0;

    {
        LogManager manager(path);
        manager.open();

        auto record = make_record(LogRecordType::BEGIN);

        (void)manager.append(record);
        expected_next_lsn = serialized_size(record);

        manager.flush();
        manager.close();
    }

    {
        LogManager manager(path);
        manager.open();

        auto record = make_record(LogRecordType::PAGE_UPDATE);

        const LSN lsn = manager.append(record);

        require(
            lsn == expected_next_lsn,
            "LSN was not recovered correctly"
        );

        manager.flush();
        manager.close();
    }

    std::filesystem::remove(path);
}

// Test 11: Close and reopen an empty WAL
void test_reopen_empty_wal() {
    const auto path = make_test_file();

    {
        LogManager manager(path);
        manager.open();
        manager.close();
    }

    {
        LogManager manager(path);
        manager.open();

        auto record = make_record(LogRecordType::BEGIN);

        require(
            manager.append(record) == 0,
            "LSN should be zero for an empty WAL"
        );

        manager.flush();
        manager.close();
    }

    std::filesystem::remove(path);
}

// Test 12: Append without opening
void test_append_without_open() {
    const auto path = make_test_file();

    {
        LogManager manager(path);

        auto record = make_record(LogRecordType::BEGIN);

        expect_throw<std::runtime_error>(
            [&manager, &record] {
                (void)manager.append(record);
            },
            "Append without open"
        );
    }

    std::filesystem::remove(path);
}

// Test 13: Flush without opening
void test_flush_without_open() {
    const auto path = make_test_file();

    {
        LogManager manager(path);

        expect_throw<std::runtime_error>(
            [&manager] {
                manager.flush();
            },
            "Flush without open"
        );
    }

    std::filesystem::remove(path);
}

// Test 14: Close without opening
void test_close_without_open() {
    const auto path = make_test_file();

    {
        LogManager manager(path);
        manager.close();
    }

    require(
        !std::filesystem::exists(path),
        "Close without open unexpectedly created a file"
    );
}

// Test 15: Repeated close
void test_close_twice() {
    const auto path = make_test_file();

    {
        LogManager manager(path);
        manager.open();

        manager.close();
        manager.close();
    }

    std::filesystem::remove(path);
}

// Test 16: Invalid record type
void test_invalid_record_type() {
    const auto path = make_test_file();

    {
        LogManager manager(path);
        manager.open();

        auto record = make_record(LogRecordType::INVALID);

        expect_throw<std::invalid_argument>(
            [&manager, &record] {(void)manager.append(record);
            },
            "Invalid record type"
        );

        manager.close();
    }

    std::filesystem::remove(path);
}

} // namespace

int main() {
    int failures = 0;
    int total = 0;

    auto run_test = [&failures, &total](
        const std::string& name,
        const std::function<void()>& test
    ) {
        ++total;

        try {
            test();
            std::cout << "PASS: " << name << '\n';
        }
        catch (const std::exception& e) {
            ++failures;

            std::cout << "FAIL: " << name
                      << " - " << e.what() << '\n';
        }
    };

    run_test("WAL file creation", test_wal_file_creation);
    run_test("Open twice", test_open_twice);
    run_test("First LSN", test_first_lsn);
    run_test("Sequential LSN allocation", test_sequential_lsn_allocation);
    run_test("Get next LSN", test_get_next_lsn);
    run_test("Buffering without flush", test_buffering_without_flush);
    run_test("Flush writes data", test_flush_writes_data);
    run_test("Flush empty buffer", test_flush_empty_buffer);
    run_test("Multiple flushes", test_multiple_flushes);
    run_test("Reopen WAL", test_reopen_wal);
    run_test("Reopen empty WAL", test_reopen_empty_wal);
    run_test("Append without open", test_append_without_open);
    run_test("Flush without open", test_flush_without_open);
    run_test("Close without open", test_close_without_open);
    run_test("Close twice", test_close_twice);
    run_test("Invalid record type", test_invalid_record_type);

    std::cout << "\n--------------------------------\n";
    std::cout << "Total: " << total << '\n';
    std::cout << "Passed: " << total - failures << '\n';
    std::cout << "Failed: " << failures << '\n';
    std::cout << "--------------------------------\n";

    return failures == 0 ? 0 : 1;
}
