
#include "minidb/storage/wal/log_reader.h"
#include "minidb/storage/wal/log_record.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace minidb::storage::wal;

namespace {

int passed = 0;
int failed = 0;
int file_counter = 0;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Exception, typename Function>
void expect_throw(Function function, const std::string& message) {
    try {
        function();
    } catch (const Exception&) {
        return;
    }

    throw std::runtime_error(message);
}

void run_test(
    const std::string& name,
    const std::function<void()>& test
) {
    try {
        test();
        ++passed;
        std::cout << "PASS: " << name << '\n';
    } catch (const std::exception& e) {
        ++failed;
        std::cout << "FAIL: " << name << " - " << e.what() << '\n';
    }
}

std::filesystem::path make_test_file() {
    auto path = std::filesystem::temp_directory_path() /
        ("minidb_log_reader_test_" +
         std::to_string(++file_counter) + ".wal");

    std::filesystem::remove(path);
    return path;
}

void write_bytes(
    const std::filesystem::path& path,
    const std::vector<std::byte>& bytes
) {
    std::ofstream file(path, std::ios::binary);

    if (!file) {
        throw std::runtime_error("Failed to create test WAL file");
    }

    file.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size())
    );

    if (!file) {
        throw std::runtime_error("Failed to write test WAL file");
    }
}

std::vector<std::byte> make_record(
    LogRecordType type,
    LSN lsn,
    std::vector<std::byte> payload = {}
) {
    LogRecord record;
    record.type = type;
    record.lsn = lsn;
    record.txn_id = 1;
    record.page_id = 10;
    record.payload = std::move(payload);

    return LogRecordSerializer::serialize(record);
}

std::vector<std::byte> concatenate(
    const std::vector<std::byte>& first,
    const std::vector<std::byte>& second
) {
    std::vector<std::byte> result = first;
    result.insert(result.end(), second.begin(), second.end());
    return result;
}

// 1. Read one record.
void test_read_single_record() {
    const auto path = make_test_file();

    const auto bytes = make_record(LogRecordType::BEGIN, 0);
    write_bytes(path, bytes);

    LogReader reader(path);
    reader.open();

    auto record = reader.read_next();

    require(record.has_value(), "Expected a WAL record");
    require(record->type == LogRecordType::BEGIN, "Incorrect record type");
    require(record->lsn == 0, "Incorrect record LSN");

    reader.close();
    std::filesystem::remove(path);
}

// 2. Read multiple records sequentially.
void test_read_multiple_records() {
    const auto path = make_test_file();

    const auto first = make_record(LogRecordType::BEGIN, 0);
    const auto second = make_record(
        LogRecordType::COMMIT,
        static_cast<LSN>(first.size())
    );

    write_bytes(path, concatenate(first, second));

    LogReader reader(path);
    reader.open();

    auto record1 = reader.read_next();
    auto record2 = reader.read_next();

    require(record1.has_value(), "Missing first record");
    require(record2.has_value(), "Missing second record");

    require(record1->type == LogRecordType::BEGIN,
            "Incorrect first record type");

    require(record2->type == LogRecordType::COMMIT,
            "Incorrect second record type");

    require(record2->lsn == first.size(),
            "Incorrect second record LSN");

    reader.close();
    std::filesystem::remove(path);
}

// 3. Clean EOF.
void test_clean_eof() {
    const auto path = make_test_file();

    const auto bytes = make_record(LogRecordType::BEGIN, 0);
    write_bytes(path, bytes);

    LogReader reader(path);
    reader.open();

    require(reader.read_next().has_value(), "Expected first record");
    require(!reader.read_next().has_value(), "Expected clean EOF");

    reader.close();
    std::filesystem::remove(path);
}

// 4. Empty WAL file.
void test_empty_wal_file() {
    const auto path = make_test_file();
    write_bytes(path, {});

    LogReader reader(path);
    reader.open();

    require(!reader.read_next().has_value(),
            "Expected EOF for empty WAL file");

    reader.close();
    std::filesystem::remove(path);
}

// 5. Read without opening.
void test_read_without_open() {
    const auto path = make_test_file();

    LogReader reader(path);

    expect_throw<std::runtime_error>(
        [&] { (void)reader.read_next(); },
        "Expected read without open to throw"
    );

    std::filesystem::remove(path);
}

// 6. Open missing file.
void test_open_missing_file() {
    const auto path = make_test_file();

    LogReader reader(path);

    expect_throw<std::system_error>(
        [&] { reader.open(); },
        "Expected opening missing file to throw"
    );
}

// 7. Open twice.
void test_open_twice() {
    const auto path = make_test_file();
    write_bytes(path, {});

    LogReader reader(path);

    reader.open();
    reader.open();

    require(!reader.read_next().has_value(),
            "Expected EOF after opening twice");

    reader.close();
    std::filesystem::remove(path);
}

// 8. Close without opening.
void test_close_without_open() {
    const auto path = make_test_file();

    LogReader reader(path);
    reader.close();

    std::filesystem::remove(path);
}

// 9. Close twice.
void test_close_twice() {
    const auto path = make_test_file();
    write_bytes(path, {});

    LogReader reader(path);
    reader.open();

    reader.close();
    reader.close();

    std::filesystem::remove(path);
}

// 10. Reopen resets reading position.
void test_reopen_resets_position() {
    const auto path = make_test_file();

    const auto bytes = make_record(LogRecordType::BEGIN, 0);
    write_bytes(path, bytes);

    LogReader reader(path);

    reader.open();
    require(reader.read_next().has_value(), "Expected first read");
    reader.close();

    reader.open();
    auto record = reader.read_next();

    require(record.has_value(), "Expected record after reopening");
    require(record->lsn == 0, "Reopen did not reset LSN");

    reader.close();
    std::filesystem::remove(path);
}

// 11. Incomplete record header.
void test_incomplete_header() {
    const auto path = make_test_file();

    std::vector<std::byte> bytes(20);
    write_bytes(path, bytes);

    LogReader reader(path);
    reader.open();

    expect_throw<std::runtime_error>(
        [&] { (void)reader.read_next(); },
        "Expected incomplete header to throw"
    );

    reader.close();
    std::filesystem::remove(path);
}

// 12. Invalid record length.
void test_invalid_record_length() {
    const auto path = make_test_file();

    std::vector<std::byte> bytes(LOG_RECORD_HEADER_SIZE);

    // Little-endian record length = 10, which is invalid.
    bytes[4] = std::byte{10};

    write_bytes(path, bytes);

    LogReader reader(path);
    reader.open();

    expect_throw<std::runtime_error>(
        [&] { (void)reader.read_next(); },
        "Expected invalid record length to throw"
    );

    reader.close();
    std::filesystem::remove(path);
}

// 13. Incomplete record payload/checksum.
void test_incomplete_record() {
    const auto path = make_test_file();

    auto bytes = make_record(LogRecordType::BEGIN, 0);

    bytes.resize(bytes.size() - 2);

    write_bytes(path, bytes);

    LogReader reader(path);
    reader.open();

    expect_throw<std::runtime_error>(
        [&] { (void)reader.read_next(); },
        "Expected incomplete record to throw"
    );

    reader.close();
    std::filesystem::remove(path);
}

// 14. Invalid magic number.
void test_invalid_magic() {
    const auto path = make_test_file();

    auto bytes = make_record(LogRecordType::BEGIN, 0);
    bytes[0] ^= std::byte{0x01};

    write_bytes(path, bytes);

    LogReader reader(path);
    reader.open();

    expect_throw<std::runtime_error>(
        [&] { (void)reader.read_next(); },
        "Expected invalid magic to throw"
    );

    reader.close();
    std::filesystem::remove(path);
}

// 15. Unsupported record version.
void test_invalid_version() {
    const auto path = make_test_file();

    auto bytes = make_record(LogRecordType::BEGIN, 0);

    // Version occupies bytes 10 and 11.
    bytes[10] = std::byte{0xFF};

    write_bytes(path, bytes);

    LogReader reader(path);
    reader.open();

    expect_throw<std::runtime_error>(
        [&] { (void)reader.read_next(); },
        "Expected unsupported version to throw"
    );

    reader.close();
    std::filesystem::remove(path);
}

// 16. Checksum corruption.
void test_checksum_corruption() {
    const auto path = make_test_file();

    auto bytes = make_record(LogRecordType::BEGIN, 0);
    bytes.back() ^= std::byte{0x01};

    write_bytes(path, bytes);

    LogReader reader(path);
    reader.open();

    expect_throw<std::runtime_error>(
        [&] { (void)reader.read_next(); },
        "Expected checksum mismatch to throw"
    );

    reader.close();
    std::filesystem::remove(path);
}

// 17. LSN mismatch.
void test_lsn_mismatch() {
    const auto path = make_test_file();

    const auto bytes = make_record(LogRecordType::BEGIN, 100);
    write_bytes(path, bytes);

    LogReader reader(path);
    reader.open();

    expect_throw<std::runtime_error>(
        [&] { (void)reader.read_next(); },
        "Expected LSN mismatch to throw"
    );

    reader.close();
    std::filesystem::remove(path);
}

// 18. Payload round trip.
void test_payload_round_trip() {
    const auto path = make_test_file();

    const std::vector<std::byte> payload = {
        std::byte{0x10},
        std::byte{0x20},
        std::byte{0x30},
        std::byte{0x40}
    };

    const auto bytes = make_record(
        LogRecordType::PAGE_UPDATE,
        0,
        payload
    );

    write_bytes(path, bytes);

    LogReader reader(path);
    reader.open();

    auto record = reader.read_next();

    require(record.has_value(), "Expected record");
    require(record->payload == payload, "Payload mismatch");
    require(record->type == LogRecordType::PAGE_UPDATE,
            "Incorrect record type");

    reader.close();
    std::filesystem::remove(path);
}

} // namespace

int main() {
    run_test("Read single record", test_read_single_record);
    run_test("Read multiple records", test_read_multiple_records);
    run_test("Clean EOF", test_clean_eof);
    run_test("Empty WAL file", test_empty_wal_file);
    run_test("Read without open", test_read_without_open);
    run_test("Open missing file", test_open_missing_file);
    run_test("Open twice", test_open_twice);
    run_test("Close without open", test_close_without_open);
    run_test("Close twice", test_close_twice);
    run_test("Reopen resets position", test_reopen_resets_position);
    run_test("Incomplete header", test_incomplete_header);
    run_test("Invalid record length", test_invalid_record_length);
    run_test("Incomplete record", test_incomplete_record);
    run_test("Invalid magic", test_invalid_magic);
    run_test("Invalid version", test_invalid_version);
    run_test("Checksum corruption", test_checksum_corruption);
    run_test("LSN mismatch", test_lsn_mismatch);
    run_test("Payload round trip", test_payload_round_trip);

    std::cout << "\n========== LogReader Test Results ==========\n";
    std::cout << "Total:  " << passed + failed << '\n';
    std::cout << "Passed: " << passed << '\n';
    std::cout << "Failed: " << failed << '\n';

    return failed == 0 ? 0 : 1;
}
