
#include "minidb/storage/wal/log_record.h"
#include "minidb/storage/page_format.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace minidb::storage;
using namespace minidb::storage::wal;

namespace {

// ---------------------------------------------------------
// Serialized WAL record field offsets
// ---------------------------------------------------------

inline constexpr std::size_t MAGIC_OFFSET = 0;
inline constexpr std::size_t RECORD_LENGTH_OFFSET = 4;
inline constexpr std::size_t RECORD_TYPE_OFFSET = 8;
inline constexpr std::size_t RECORD_VERSION_OFFSET = 10;
inline constexpr std::size_t RECORD_LSN_OFFSET = 12;
inline constexpr std::size_t RECORD_TXN_ID_OFFSET = 20;
inline constexpr std::size_t RECORD_PREV_LSN_OFFSET = 28;
inline constexpr std::size_t RECORD_PAGE_ID_OFFSET = 36;
inline constexpr std::size_t RECORD_PAYLOAD_LENGTH_OFFSET = 44;

// ---------------------------------------------------------
// Test utilities
// ---------------------------------------------------------

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Exception, typename Function>
void expect_throw(Function function, const std::string &message) {

    try {
        function();
    }
    catch (const Exception &) {
        return;
    }

    throw std::runtime_error(message + ": expected exception was not thrown");
}

// ---------------------------------------------------------
// Create a test LogRecord
// ---------------------------------------------------------

LogRecord make_record(LogRecordType type) {

    LogRecord record;

    record.type = type;
    record.lsn = 100;
    record.prev_lsn = 80;
    record.txn_id = 10;
    record.page_id = 5;

    record.payload = {
        std::byte{0x11},
        std::byte{0x22},
        std::byte{0x33},
        std::byte{0x44},
        std::byte{0x55}
    };

    return record;
}

// ---------------------------------------------------------
// Compare two LogRecords
// ---------------------------------------------------------

void require_records_equal(
    const LogRecord& actual,
    const LogRecord& expected) {

    require(
        actual.type == expected.type,
        "Record type mismatch");

    require(
        actual.lsn == expected.lsn,
        "LSN mismatch");

    require(
        actual.prev_lsn == expected.prev_lsn,
        "Previous LSN mismatch");

    require(
        actual.txn_id == expected.txn_id,
        "Transaction ID mismatch");

    require(
        actual.page_id == expected.page_id,
        "Page ID mismatch");

    require(
        actual.payload == expected.payload,
        "Payload mismatch");
}

// ---------------------------------------------------------
// Serialization / deserialization helpers
// ---------------------------------------------------------

std::vector<std::byte> serialize_record(
    const LogRecord& record) {

    return LogRecordSerializer::serialize(record);
}

LogRecord deserialize_record(
    const std::vector<std::byte>& bytes) {

    return LogRecordSerializer::deserialize(
        std::span<const std::byte>(
            bytes.data(),
            bytes.size()));
}

// ---------------------------------------------------------
// Test 1: Empty payload round trip
// ---------------------------------------------------------

void test_empty_payload_round_trip() {

    LogRecord original =
        make_record(LogRecordType::PAGE_UPDATE);

    original.payload.clear();

    const auto bytes = serialize_record(original);
    const auto decoded = deserialize_record(bytes);

    require_records_equal(decoded, original);

    require(
        bytes.size() == LOG_RECORD_FIXED_SIZE,
        "Incorrect serialized size for empty payload");
}

// ---------------------------------------------------------
// Test 2: Non-empty binary payload round trip
// ---------------------------------------------------------

void test_binary_payload_round_trip() {

    LogRecord original =
        make_record(LogRecordType::PAGE_IMAGE);

    original.payload = {
        std::byte{0x00},
        std::byte{0xFF},
        std::byte{0xAB},
        std::byte{0xCD},
        std::byte{0x10},
        std::byte{0x00}
    };

    const auto bytes = serialize_record(original);
    const auto decoded = deserialize_record(bytes);

    require_records_equal(decoded, original);
}

// ---------------------------------------------------------
// Test 3: All valid record types
// ---------------------------------------------------------

void test_all_valid_record_types() {

    const std::vector<LogRecordType> types = {
        LogRecordType::BEGIN,
        LogRecordType::PAGE_INSERT,
        LogRecordType::PAGE_UPDATE,
        LogRecordType::PAGE_DELETE,
        LogRecordType::PAGE_IMAGE,
        LogRecordType::COMMIT,
        LogRecordType::ABORT,
        LogRecordType::CHECKPOINT_BEGIN,
        LogRecordType::CHECKPOINT_END,
        LogRecordType::METADATA_UPDATE
    };

    for (const auto type : types) {

        LogRecord original = make_record(type);

        const auto bytes = serialize_record(original);
        const auto decoded = deserialize_record(bytes);

        require_records_equal(decoded, original);
    }
}

// ---------------------------------------------------------
// Test 4: Verify serialized binary layout
// ---------------------------------------------------------

void test_serialized_record_layout() {

    const LogRecord original =
        make_record(LogRecordType::PAGE_INSERT);

    const auto bytes = serialize_record(original);

    require(
        bytes.size() ==
            LOG_RECORD_FIXED_SIZE + original.payload.size(),
        "Incorrect total record size");

    require(
        format::read_le<std::uint32_t>(
            bytes.data() + MAGIC_OFFSET) == LOG_RECORD_MAGIC,
        "Incorrect magic number");

    require(
        format::read_le<std::uint32_t>(
            bytes.data() + RECORD_LENGTH_OFFSET) == bytes.size(),
        "Incorrect record length");

    require(
        format::read_le<std::uint16_t>(
            bytes.data() + RECORD_TYPE_OFFSET) ==
                static_cast<std::uint16_t>(original.type),
        "Incorrect serialized record type");

    require(
        format::read_le<std::uint16_t>(
            bytes.data() + RECORD_VERSION_OFFSET) ==
                LOG_RECORD_VERSION,
        "Incorrect format version");

    require(
        format::read_le<std::uint64_t>(
            bytes.data() + RECORD_LSN_OFFSET) == original.lsn,
        "Incorrect serialized LSN");

    require(
        format::read_le<std::uint64_t>(
            bytes.data() + RECORD_TXN_ID_OFFSET) == original.txn_id,
        "Incorrect serialized transaction ID");

    require(
        format::read_le<std::uint64_t>(
            bytes.data() + RECORD_PREV_LSN_OFFSET) == original.prev_lsn,
        "Incorrect serialized previous LSN");

    require(
        format::read_le<std::uint64_t>(
            bytes.data() + RECORD_PAGE_ID_OFFSET) == original.page_id,
        "Incorrect serialized page ID");

    require(
        format::read_le<std::uint32_t>(
            bytes.data() + RECORD_PAYLOAD_LENGTH_OFFSET) ==
                original.payload.size(),
        "Incorrect serialized payload length");

    for (std::size_t i = 0; i < original.payload.size(); ++i) {

        require(
            bytes[LOG_RECORD_HEADER_SIZE + i] ==
                original.payload[i],
            "Payload bytes are not at expected offsets");
    }
}

// ---------------------------------------------------------
// Test 5: Reject INVALID record type during serialization
// ---------------------------------------------------------

void test_reject_invalid_type_on_serialize() {

    LogRecord record =
        make_record(LogRecordType::PAGE_UPDATE);

    record.type = LogRecordType::INVALID;

    expect_throw<std::invalid_argument>([&]() {serialize_record(record);}, "Invalid record type serialization");
}

// ---------------------------------------------------------
// Test 6: Reject record smaller than minimum size
// ---------------------------------------------------------

void test_reject_too_small_record() {

    std::vector<std::byte> bytes(
        LOG_RECORD_FIXED_SIZE - 1);

    expect_throw<std::runtime_error>([&]() {deserialize_record(bytes);}, "Truncated minimum record");
}

// ---------------------------------------------------------
// Test 7: Reject invalid magic number
// ---------------------------------------------------------

void test_reject_invalid_magic() {

    auto bytes = serialize_record(
        make_record(LogRecordType::PAGE_UPDATE));

    format::write_le<std::uint32_t>(
        bytes.data() + MAGIC_OFFSET,
        0xDEADBEEFU);

    expect_throw<std::runtime_error>([&]() {deserialize_record(bytes);}, "Invalid magic number");
}

// ---------------------------------------------------------
// Test 8: Reject unsupported version
// ---------------------------------------------------------

void test_reject_unsupported_version() {

    auto bytes = serialize_record(
        make_record(LogRecordType::PAGE_UPDATE));

    format::write_le<std::uint16_t>(
        bytes.data() + RECORD_VERSION_OFFSET,
        static_cast<std::uint16_t>(LOG_RECORD_VERSION + 1));

    expect_throw<std::runtime_error>([&]() {deserialize_record(bytes);}, "Unsupported WAL version");
}

// ---------------------------------------------------------
// Test 9: Reject record length mismatch
// ---------------------------------------------------------

void test_reject_record_length_mismatch() {

    auto bytes = serialize_record(
        make_record(LogRecordType::PAGE_UPDATE));

    format::write_le<std::uint32_t>(
        bytes.data() + RECORD_LENGTH_OFFSET,
        static_cast<std::uint32_t>(bytes.size() - 1));

    expect_throw<std::runtime_error>([&]() {deserialize_record(bytes);}, "Record length mismatch");
}

// ---------------------------------------------------------
// Test 10: Reject invalid declared record length
// ---------------------------------------------------------

void test_reject_invalid_declared_record_length() {

    auto bytes = serialize_record(
        make_record(LogRecordType::PAGE_UPDATE));

    format::write_le<std::uint32_t>(
        bytes.data() + RECORD_LENGTH_OFFSET,
        static_cast<std::uint32_t>(
            LOG_RECORD_FIXED_SIZE - 1));

    expect_throw<std::runtime_error>([&]() {deserialize_record(bytes);}, "Invalid declared record length");
}

// ---------------------------------------------------------
// Test 11: Reject payload length mismatch
// ---------------------------------------------------------

void test_reject_payload_length_mismatch() {

    auto bytes = serialize_record(
        make_record(LogRecordType::PAGE_UPDATE));

    format::write_le<std::uint32_t>(
        bytes.data() + RECORD_PAYLOAD_LENGTH_OFFSET,
        6);

    expect_throw<std::runtime_error>([&]() {deserialize_record(bytes);}, "Payload length mismatch");
}

// ---------------------------------------------------------
// Test 12: Reject unknown record type
// ---------------------------------------------------------

void test_reject_unknown_record_type() {

    auto bytes = serialize_record(
        make_record(LogRecordType::PAGE_UPDATE));

    format::write_le<std::uint16_t>(
        bytes.data() + RECORD_TYPE_OFFSET,
        0xFFFF);

    expect_throw<std::runtime_error>([&]() {deserialize_record(bytes);}, "Unknown WAL record type");
}

// ---------------------------------------------------------
// Test 13: Reject corrupted payload
// ---------------------------------------------------------

void test_reject_corrupted_payload() {

    auto bytes = serialize_record(
        make_record(LogRecordType::PAGE_UPDATE));

    // Change one payload bit without updating CRC.
    bytes[LOG_RECORD_HEADER_SIZE] ^= std::byte{0x01};

    expect_throw<std::runtime_error>([&]() {deserialize_record(bytes);}, "Corrupted WAL payload");
}

// ---------------------------------------------------------
// Test 14: Reject corrupted checksum
// ---------------------------------------------------------

void test_reject_corrupted_checksum() {

    auto bytes = serialize_record(
        make_record(LogRecordType::PAGE_UPDATE));

    // Modify the final checksum byte.
    bytes.back() ^= std::byte{0x01};

    expect_throw<std::runtime_error>([&]() {deserialize_record(bytes);}, "Corrupted WAL checksum");
}

// ---------------------------------------------------------
// Test 15: Reject truncated complete record
// ---------------------------------------------------------

void test_reject_truncated_record() {

    auto bytes = serialize_record(
        make_record(LogRecordType::PAGE_UPDATE));

    // Remove the final byte.
    bytes.pop_back();

    expect_throw<std::runtime_error>([&]() {deserialize_record(bytes);}, "Truncated WAL record");
}

// ---------------------------------------------------------
// Test 16: Reject trailing bytes
// ---------------------------------------------------------

void test_reject_trailing_bytes() {

    auto bytes = serialize_record(
        make_record(LogRecordType::PAGE_UPDATE));

    bytes.push_back(std::byte{0xAB});

    expect_throw<std::runtime_error>([&]() {deserialize_record(bytes);}, "Trailing bytes after WAL record");
}

// ---------------------------------------------------------
// Test runner
// ---------------------------------------------------------

void run_test(const std::string &name, const std::function<void()> &test, int& failures) {

    try {
        test();
        std::cout << "[PASS] " << name << '\n';
    }
    catch (const std::exception &e) {

        ++failures;
        std::cerr << "[FAIL] " << name << ": " << e.what() << '\n';
    }
}

} // namespace

int main() {

    int failures = 0;

    run_test(
        "Empty payload round trip",
        test_empty_payload_round_trip,
        failures);

    run_test(
        "Binary payload round trip",
        test_binary_payload_round_trip,
        failures);

    run_test(
        "All valid record types",
        test_all_valid_record_types,
        failures);

    run_test(
        "Serialized record layout",
        test_serialized_record_layout,
        failures);

    run_test(
        "Reject invalid type on serialization",
        test_reject_invalid_type_on_serialize,
        failures);

    run_test(
        "Reject too-small record",
        test_reject_too_small_record,
        failures);

    run_test(
        "Reject invalid magic",
        test_reject_invalid_magic,
        failures);

    run_test(
        "Reject unsupported version",
        test_reject_unsupported_version,
        failures);

    run_test(
        "Reject record length mismatch",
        test_reject_record_length_mismatch,
        failures);

    run_test(
        "Reject invalid declared record length",
        test_reject_invalid_declared_record_length,
        failures);

    run_test(
        "Reject payload length mismatch",
        test_reject_payload_length_mismatch,
        failures);

    run_test(
        "Reject unknown record type",
        test_reject_unknown_record_type,
        failures);

    run_test(
        "Reject corrupted payload",
        test_reject_corrupted_payload,
        failures);

    run_test(
        "Reject corrupted checksum",
        test_reject_corrupted_checksum,
        failures);

    run_test(
        "Reject truncated record",
        test_reject_truncated_record,
        failures);

    run_test(
        "Reject trailing bytes",
        test_reject_trailing_bytes,
        failures);

    constexpr int TOTAL_TESTS = 16;

    std::cout << "\n--------------------------------\n";
    std::cout << "LogRecord test summary\n";
    std::cout << "Total: " << TOTAL_TESTS << '\n';
    std::cout << "Passed: " << TOTAL_TESTS - failures << '\n';
    std::cout << "Failed: " << failures << '\n';
    std::cout << "--------------------------------\n";

    return failures == 0 ? 0 : 1;
}
