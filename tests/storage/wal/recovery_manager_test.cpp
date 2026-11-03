
#include "minidb/storage/disk_manager.h"
#include "minidb/storage/database_page_manager.h"
#include "minidb/storage/page_format.h"

#include "minidb/storage/wal/log_manager.h"
#include "minidb/storage/wal/log_reader.h"
#include "minidb/storage/wal/recovery_manager.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace minidb::storage;
using namespace minidb::storage::wal;

namespace format = minidb::storage::format;

namespace {

// ------------------------------------------------------------
// Test infrastructure
// ------------------------------------------------------------

struct TestFiles {
    std::string db_path;
    std::string wal_path;

    explicit TestFiles(const std::string& name)
        : db_path("recovery_" + name + ".db"),
          wal_path("recovery_" + name + ".wal") {
        clean();
    }

    void clean() const {
        std::filesystem::remove(db_path);
        std::filesystem::remove(wal_path);
    }

    ~TestFiles() {
        clean();
    }
};

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::uint64_t create_page(DatabasePageManager& page_manager) {
    const auto page_id = page_manager.allocate_page();

    auto page = format::make_data_page(
        page_id,
        format::PageType::LSM_DATA);

    page_manager.write_page(page_id, page);

    return page_id;
}

DiskManager::Page make_test_image(
    std::uint64_t page_id,
    std::byte marker) {

    auto page = format::make_data_page(
        page_id,
        format::PageType::LSM_DATA);

    page[format::PAGE_HEADER_SIZE] = marker;

    return page;
}

LSN append_page_image(
    LogManager& log_manager,
    std::uint64_t page_id,
    const DiskManager::Page& image) {

    LogRecord record;
    record.type = LogRecordType::PAGE_IMAGE;
    record.page_id = page_id;
    record.payload.assign(image.begin(), image.end());

    return log_manager.append(record);
}

void recover(
    const std::string& wal_path,
    DatabasePageManager& page_manager) {

    LogReader reader(wal_path);
    RecoveryManager recovery(reader, page_manager);

    recovery.recover();
}

void verify_marker(
    const DiskManager::Page& page,
    std::byte expected,
    const std::string& message) {

    require(
        page[format::PAGE_HEADER_SIZE] == expected,
        message);
}

// ------------------------------------------------------------
// Test 1: PAGE_IMAGE redo
// ------------------------------------------------------------

void test_page_image_redo() {
    TestFiles files("redo");

    DiskManager disk(files.db_path);
    DatabasePageManager page_manager(disk);

    const auto page_id = create_page(page_manager);

    const auto image = make_test_image(
        page_id, std::byte{0x42});

    {
        LogManager log_manager(files.wal_path);
        log_manager.open();

        append_page_image(log_manager, page_id, image);

        log_manager.flush();
        log_manager.close();
    }

    recover(files.wal_path, page_manager);

    const auto result = page_manager.read_page(page_id);

    verify_marker(
        result,
        std::byte{0x42},
        "PAGE_IMAGE was not replayed");

    require(
        format::read_page_lsn(result) == 0,
        "Incorrect PageLSN after redo");
}

// ------------------------------------------------------------
// Test 2: Equal PageLSN is skipped
// ------------------------------------------------------------

void test_equal_page_lsn_is_skipped() {
    TestFiles files("equal_lsn");

    DiskManager disk(files.db_path);
    DatabasePageManager page_manager(disk);

    const auto page_id = create_page(page_manager);

    auto current = make_test_image(
        page_id, std::byte{0x11});

    format::write_page_lsn(current, 0);
    page_manager.write_page(page_id, current);

    auto incoming = make_test_image(
        page_id, std::byte{0x22});

    {
        LogManager log_manager(files.wal_path);
        log_manager.open();

        append_page_image(log_manager, page_id, incoming);

        log_manager.flush();
        log_manager.close();
    }

    recover(files.wal_path, page_manager);

    const auto result = page_manager.read_page(page_id);

    verify_marker(
        result,
        std::byte{0x11},
        "Recovery overwrote a page with equal PageLSN");
}

// ------------------------------------------------------------
// Test 3: Newer PageLSN is skipped
// ------------------------------------------------------------

void test_newer_page_lsn_is_skipped() {
    TestFiles files("newer_lsn");

    DiskManager disk(files.db_path);
    DatabasePageManager page_manager(disk);

    const auto page_id = create_page(page_manager);

    auto current = make_test_image(
        page_id, std::byte{0x11});

    format::write_page_lsn(current, 5);
    page_manager.write_page(page_id, current);

    auto incoming = make_test_image(
        page_id, std::byte{0x22});

    {
        LogManager log_manager(files.wal_path);
        log_manager.open();

        append_page_image(log_manager, page_id, incoming);

        log_manager.flush();
        log_manager.close();
    }

    recover(files.wal_path, page_manager);

    const auto result = page_manager.read_page(page_id);

    verify_marker(
        result,
        std::byte{0x11},
        "Recovery overwrote a page with newer PageLSN");
}

// ------------------------------------------------------------
// Test 4: Invalid PAGE_IMAGE payload size
// ------------------------------------------------------------

void test_invalid_payload_size() {
    TestFiles files("invalid_payload");

    DiskManager disk(files.db_path);
    DatabasePageManager page_manager(disk);

    const auto page_id = create_page(page_manager);

    {
        LogManager log_manager(files.wal_path);
        log_manager.open();

        LogRecord record;
        record.type = LogRecordType::PAGE_IMAGE;
        record.page_id = page_id;
        record.payload = {std::byte{0x01}};

        (void)log_manager.append(record);

        log_manager.flush();
        log_manager.close();
    }

    bool threw = false;

    try {
        recover(files.wal_path, page_manager);
    } catch (const std::runtime_error&) {
        threw = true;
    }

    require(threw, "Invalid PAGE_IMAGE payload was accepted");
}

// ------------------------------------------------------------
// Test 5: Metadata page ID is rejected
// ------------------------------------------------------------

void test_metadata_page_id_rejected() {
    TestFiles files("metadata_id");

    DiskManager disk(files.db_path);
    DatabasePageManager page_manager(disk);

    auto image = make_test_image(1, std::byte{0x42});

    {
        LogManager log_manager(files.wal_path);
        log_manager.open();

        LogRecord record;
        record.type = LogRecordType::PAGE_IMAGE;
        record.page_id = format::META_PAGE_ID;
        record.payload.assign(image.begin(), image.end());

        (void)log_manager.append(record);

        log_manager.flush();
        log_manager.close();
    }

    bool threw = false;

    try {
        recover(files.wal_path, page_manager);
    } catch (const std::runtime_error&) {
        threw = true;
    }

    require(threw, "PAGE_IMAGE targeting metadata page was accepted");
}

// ------------------------------------------------------------
// Test 6: Recovery is idempotent
// ------------------------------------------------------------

void test_recovery_is_idempotent() {
    TestFiles files("idempotent");

    DiskManager disk(files.db_path);
    DatabasePageManager page_manager(disk);

    const auto page_id = create_page(page_manager);

    auto image = make_test_image(
        page_id, std::byte{0x42});

    {
        LogManager log_manager(files.wal_path);
        log_manager.open();

        append_page_image(log_manager, page_id, image);

        log_manager.flush();
        log_manager.close();
    }

    recover(files.wal_path, page_manager);
    recover(files.wal_path, page_manager);

    const auto result = page_manager.read_page(page_id);

    verify_marker(
        result,
        std::byte{0x42},
        "Repeated recovery changed page contents");

    require(
        format::read_page_lsn(result) == 0,
        "Repeated recovery changed PageLSN");
}

// ------------------------------------------------------------
// Test 7: Crash and restart recovery
// ------------------------------------------------------------

void test_crash_restart_recovery() {
    TestFiles files("crash");

    std::uint64_t page_id = 0;

    // Create and persist the original database page.
    {
        DiskManager disk(files.db_path);
        DatabasePageManager page_manager(disk);

        page_id = create_page(page_manager);

        auto original = make_test_image(
            page_id, std::byte{0x11});

        page_manager.write_page(page_id, original);
        disk.flush();
    }

    // Persist WAL, but do not write the new page image.
    {
        auto image = make_test_image(
            page_id, std::byte{0x42});

        LogManager log_manager(files.wal_path);
        log_manager.open();

        append_page_image(log_manager, page_id, image);

        log_manager.flush();
        log_manager.close();
    }

    // Reopen database, simulating restart.
    {
        DiskManager disk(files.db_path);
        DatabasePageManager page_manager(disk);

        const auto before = page_manager.read_page(page_id);

        verify_marker(
            before,
            std::byte{0x11},
            "Database changed before recovery");

        recover(files.wal_path, page_manager);

        const auto result = page_manager.read_page(page_id);

        verify_marker(
            result,
            std::byte{0x42},
            "Crash recovery did not restore page");

        require(
            format::read_page_lsn(result) == 0,
            "Incorrect PageLSN after crash recovery");

        disk.flush();
    }

    // Reopen again and verify persistence.
    {
        DiskManager disk(files.db_path);
        DatabasePageManager page_manager(disk);

        const auto result = page_manager.read_page(page_id);

        verify_marker(
            result,
            std::byte{0x42},
            "Recovered page was not persisted");

        require(
            format::read_page_lsn(result) == 0,
            "Persisted PageLSN is incorrect");
    }
}

// ------------------------------------------------------------
// Test 8: Multiple PAGE_IMAGE records for the same page
// ------------------------------------------------------------

void test_multiple_images_same_page() {
    TestFiles files("multiple_same_page");

    DiskManager disk(files.db_path);
    DatabasePageManager page_manager(disk);

    const auto page_id = create_page(page_manager);

    const std::vector<std::byte> markers = {
        std::byte{0x11},
        std::byte{0x22},
        std::byte{0x33}
    };

    std::vector<LSN> lsns;

    {
        LogManager log_manager(files.wal_path);
        log_manager.open();

        for (const auto marker : markers) {
            auto image = make_test_image(page_id, marker);

            lsns.push_back(
                append_page_image(log_manager, page_id, image));
        }

        log_manager.flush();
        log_manager.close();
    }

    require(
        lsns[0] < lsns[1] && lsns[1] < lsns[2],
        "WAL LSNs are not increasing");

    recover(files.wal_path, page_manager);

    const auto result = page_manager.read_page(page_id);

    verify_marker(
        result,
        std::byte{0x33},
        "Latest PAGE_IMAGE was not applied");

    require(
        format::read_page_lsn(result) == lsns[2],
        "Latest PageLSN was not persisted");
}

// ------------------------------------------------------------
// Test 9: Multiple pages in one WAL stream
// ------------------------------------------------------------

void test_multiple_pages_recovery() {
    TestFiles files("multiple_pages");

    DiskManager disk(files.db_path);
    DatabasePageManager page_manager(disk);

    const auto page_a = create_page(page_manager);
    const auto page_b = create_page(page_manager);
    const auto page_c = create_page(page_manager);

    {
        LogManager log_manager(files.wal_path);
        log_manager.open();

        auto image_a = make_test_image(
            page_a, std::byte{0x11});

        auto image_b = make_test_image(
            page_b, std::byte{0x22});

        auto image_c = make_test_image(
            page_c, std::byte{0x33});

        append_page_image(log_manager, page_a, image_a);
        append_page_image(log_manager, page_b, image_b);
        append_page_image(log_manager, page_c, image_c);

        log_manager.flush();
        log_manager.close();
    }

    recover(files.wal_path, page_manager);

    verify_marker(
        page_manager.read_page(page_a),
        std::byte{0x11},
        "Page A was not recovered");

    verify_marker(
        page_manager.read_page(page_b),
        std::byte{0x22},
        "Page B was not recovered");

    verify_marker(
        page_manager.read_page(page_c),
        std::byte{0x33},
        "Page C was not recovered");
}

// ------------------------------------------------------------
// Test 10: Corrupt checksum
// ------------------------------------------------------------

void test_corrupt_wal_checksum() {
    TestFiles files("corrupt_checksum");

    DiskManager disk(files.db_path);
    DatabasePageManager page_manager(disk);

    const auto page_a = create_page(page_manager);
    const auto page_b = create_page(page_manager);

    auto original_a = make_test_image(
        page_a, std::byte{0x01});

    auto original_b = make_test_image(
        page_b, std::byte{0x02});

    page_manager.write_page(page_a, original_a);
    page_manager.write_page(page_b, original_b);

    LSN second_lsn = 0;

    {
        LogManager log_manager(files.wal_path);
        log_manager.open();

        auto image_a = make_test_image(
            page_a, std::byte{0x11});

        auto image_b = make_test_image(
            page_b, std::byte{0x22});

        append_page_image(log_manager, page_a, image_a);
        second_lsn = append_page_image(
            log_manager, page_b, image_b);

        log_manager.flush();
        log_manager.close();
    }

    // Corrupt one checksum byte of the second record.
    {
        std::fstream wal(
            files.wal_path,
            std::ios::in | std::ios::out | std::ios::binary);

        require(wal.is_open(), "Could not open WAL for corruption");

        const auto checksum_offset =
            second_lsn +
            LOG_RECORD_HEADER_SIZE +
            format::PAGE_SIZE;

        wal.seekg(static_cast<std::streamoff>(checksum_offset));

        char byte = 0;
        wal.read(&byte, 1);

        require(wal.good(), "Could not read WAL checksum");

        byte ^= static_cast<char>(0xFF);

        wal.seekp(static_cast<std::streamoff>(checksum_offset));
        wal.write(&byte, 1);
        wal.flush();

        require(wal.good(), "Could not corrupt WAL checksum");
    }

    bool threw = false;

    try {
        recover(files.wal_path, page_manager);
    } catch (const std::runtime_error&) {
        threw = true;
    }

    require(threw, "Corrupt WAL checksum was accepted");

    verify_marker(
        page_manager.read_page(page_a),
        std::byte{0x11},
        "First valid WAL record was not replayed");

    verify_marker(
        page_manager.read_page(page_b),
        std::byte{0x02},
        "Corrupted WAL record modified page B");
}

// ------------------------------------------------------------
// Test 11: Truncated WAL header
// ------------------------------------------------------------

void test_truncated_wal_header() {
    TestFiles files("truncated_header");

    DiskManager disk(files.db_path);
    DatabasePageManager page_manager(disk);

    const auto page_id = create_page(page_manager);

    {
        LogManager log_manager(files.wal_path);
        log_manager.open();

        auto image = make_test_image(
            page_id, std::byte{0x42});

        append_page_image(log_manager, page_id, image);

        log_manager.flush();
        log_manager.close();
    }

    // Leave only a partial record header.
    std::filesystem::resize_file(files.wal_path, 20);

    bool threw = false;

    try {
        recover(files.wal_path, page_manager);
    } catch (const std::runtime_error&) {
        threw = true;
    }

    require(threw, "Truncated WAL header was accepted");
}

// ------------------------------------------------------------
// Test 12: Truncated WAL payload
// ------------------------------------------------------------

void test_truncated_wal_payload() {
    TestFiles files("truncated_payload");

    DiskManager disk(files.db_path);
    DatabasePageManager page_manager(disk);

    const auto page_id = create_page(page_manager);

    {
        LogManager log_manager(files.wal_path);
        log_manager.open();

        auto image = make_test_image(
            page_id, std::byte{0x42});

        append_page_image(log_manager, page_id, image);

        log_manager.flush();
        log_manager.close();
    }

    // Keep the complete header and only part of the payload.
    const auto truncated_size =
        LOG_RECORD_HEADER_SIZE + 100;

    std::filesystem::resize_file(
        files.wal_path,
        truncated_size);

    bool threw = false;

    try {
        recover(files.wal_path, page_manager);
    } catch (const std::runtime_error&) {
        threw = true;
    }

    require(threw, "Truncated WAL payload was accepted");
}

// ------------------------------------------------------------
// Test 13: Unsupported record types are ignored for redo
// ------------------------------------------------------------

void test_unsupported_record_types_ignored() {
    TestFiles files("unsupported_types");

    DiskManager disk(files.db_path);
    DatabasePageManager page_manager(disk);

    const auto page_id = create_page(page_manager);

    auto original = make_test_image(
        page_id, std::byte{0x11});

    page_manager.write_page(page_id, original);

    {
        LogManager log_manager(files.wal_path);
        log_manager.open();

        LogRecord begin;
        begin.type = LogRecordType::BEGIN;

        LogRecord commit;
        commit.type = LogRecordType::COMMIT;

        (void)log_manager.append(begin);
        (void)log_manager.append(commit);

        log_manager.flush();
        log_manager.close();
    }

    recover(files.wal_path, page_manager);

    const auto result = page_manager.read_page(page_id);

    verify_marker(
        result,
        std::byte{0x11},
        "Unsupported WAL records unexpectedly modified the page");
}

// ------------------------------------------------------------
// Test 14: Embedded page ID mismatch
// ------------------------------------------------------------

void test_page_image_id_mismatch() {
    TestFiles files("page_id_mismatch");

    DiskManager disk(files.db_path);
    DatabasePageManager page_manager(disk);

    const auto page_a = create_page(page_manager);
    const auto page_b = create_page(page_manager);

    auto image = make_test_image(
        page_b, std::byte{0x42});

    {
        LogManager log_manager(files.wal_path);
        log_manager.open();

        // WAL targets page A, but image identifies page B.
        append_page_image(log_manager, page_a, image);

        log_manager.flush();
        log_manager.close();
    }

    bool threw = false;

    try {
        recover(files.wal_path, page_manager);
    } catch (const std::runtime_error&) {
        threw = true;
    }

    require(threw, "Mismatched PAGE_IMAGE page ID was accepted");
}

// ------------------------------------------------------------
// Test runner
// ------------------------------------------------------------

template <typename Test>
bool run_test(const std::string& name, Test test) {
    try {
        test();
        std::cout << "[PASS] " << name << '\n';
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] " << name << ": "
                  << e.what() << '\n';
        return false;
    }
}


std::vector<std::byte> make_page_update_payload(
    std::uint32_t offset,
    std::uint32_t length,
    const std::vector<std::byte>& replacement) {

    std::vector<std::byte> payload(8 + replacement.size());

    // Offset: 4-byte little-endian.
    for (int i = 0; i < 4; ++i) {
        payload[i] = static_cast<std::byte>((offset >> (i * 8)) & 0xFF);
    }

    // Length: 4-byte little-endian.
    for (int i = 0; i < 4; ++i) {
        payload[4 + i] =
            static_cast<std::byte>((length >> (i * 8)) & 0xFF);
    }

    std::copy(
        replacement.begin(),
        replacement.end(),
        payload.begin() + 8);

    return payload;
}

LSN append_page_update(
    LogManager& log_manager,
    std::uint64_t page_id,
    std::uint32_t offset,
    const std::vector<std::byte>& replacement) {

    LogRecord record;
    record.type = LogRecordType::PAGE_UPDATE;
    record.page_id = page_id;
    record.payload = make_page_update_payload(
        offset,
        static_cast<std::uint32_t>(replacement.size()),
        replacement);

    return log_manager.append(record);
}


// ------------------------------------------------------------
// PAGE_UPDATE tests
// ------------------------------------------------------------

// Test 15: Successful PAGE_UPDATE overwrite.
void test_page_update_redo() {
    TestFiles files("page_update_redo");

    DiskManager disk(files.db_path);
    DatabasePageManager page_manager(disk);

    const auto page_id = create_page(page_manager);

    auto original = make_test_image(page_id, std::byte{0x11});
    original[99] = std::byte{0xAA};
    original[100] = std::byte{0x01};
    original[101] = std::byte{0x02};
    original[102] = std::byte{0x03};
    original[103] = std::byte{0xBB};
    page_manager.write_page(page_id, original);

    const std::vector<std::byte> replacement = {
        std::byte{0xA1}, std::byte{0xA2}, std::byte{0xA3}
    };

    LSN update_lsn;

    {
        LogManager log_manager(files.wal_path);
        log_manager.open();

        update_lsn = append_page_update(
            log_manager, page_id, 100, replacement);

        log_manager.flush();
        log_manager.close();
    }

    recover(files.wal_path, page_manager);

    const auto result = page_manager.read_page(page_id);

    require(result[99] == std::byte{0xAA},
            "PAGE_UPDATE modified bytes before the update range");

    require(result[100] == std::byte{0xA1} &&
            result[101] == std::byte{0xA2} &&
            result[102] == std::byte{0xA3},
            "PAGE_UPDATE replacement bytes are incorrect");

    require(result[103] == std::byte{0xBB},
            "PAGE_UPDATE modified bytes after the update range");

    require(format::read_page_lsn(result) == update_lsn,
            "PAGE_UPDATE did not advance PageLSN");
}

// Test 16: PAGE_UPDATE recovery is idempotent.
void test_page_update_idempotency() {
    TestFiles files("page_update_idempotent");

    DiskManager disk(files.db_path);
    DatabasePageManager page_manager(disk);

    const auto page_id = create_page(page_manager);

    const std::vector<std::byte> replacement = {
        std::byte{0x11}, std::byte{0x22}, std::byte{0x33}
    };

    LSN update_lsn;

    {
        LogManager log_manager(files.wal_path);
        log_manager.open();

        update_lsn = append_page_update(
            log_manager, page_id, 100, replacement);

        log_manager.flush();
        log_manager.close();
    }

    recover(files.wal_path, page_manager);
    const auto first_result = page_manager.read_page(page_id);

    recover(files.wal_path, page_manager);
    const auto second_result = page_manager.read_page(page_id);

    require(first_result == second_result,
            "Repeated PAGE_UPDATE recovery changed page contents");

    require(format::read_page_lsn(second_result) == update_lsn,
            "Repeated recovery changed PageLSN");
}

// Test 17: PAGE_UPDATE with older LSN is skipped.
void test_page_update_older_lsn_skipped() {
    TestFiles files("page_update_older_lsn");

    DiskManager disk(files.db_path);
    DatabasePageManager page_manager(disk);

    const auto page_id = create_page(page_manager);

    auto current = make_test_image(page_id, std::byte{0x11});
    current[100] = std::byte{0xAA};
    format::write_page_lsn(current, 100);
    page_manager.write_page(page_id, current);

    {
        LogManager log_manager(files.wal_path);
        log_manager.open();

        append_page_update(
            log_manager,
            page_id,
            100,
            {std::byte{0xBB}});

        log_manager.flush();
        log_manager.close();
    }

    recover(files.wal_path, page_manager);

    const auto result = page_manager.read_page(page_id);

    require(result[100] == std::byte{0xAA},
            "Older PAGE_UPDATE overwrote newer page contents");

    require(format::read_page_lsn(result) == 100,
            "Older PAGE_UPDATE changed PageLSN");
}

// Test 18: PAGE_UPDATE payload must contain more than its header.
void test_page_update_invalid_payload_size() {
    TestFiles files("page_update_invalid_payload");

    DiskManager disk(files.db_path);
    DatabasePageManager page_manager(disk);

    const auto page_id = create_page(page_manager);

    {
        LogManager log_manager(files.wal_path);
        log_manager.open();

        LogRecord record;
        record.type = LogRecordType::PAGE_UPDATE;
        record.page_id = page_id;
        record.payload.resize(8);

        (void)log_manager.append(record);

        log_manager.flush();
        log_manager.close();
    }

    bool threw = false;

    try {
        recover(files.wal_path, page_manager);
    } catch (const std::runtime_error&) {
        threw = true;
    }

    require(threw, "Empty PAGE_UPDATE replacement was accepted");
}

// Test 19: Declared length must match replacement size.
void test_page_update_length_mismatch() {
    TestFiles files("page_update_length_mismatch");

    DiskManager disk(files.db_path);
    DatabasePageManager page_manager(disk);

    const auto page_id = create_page(page_manager);

    {
        LogManager log_manager(files.wal_path);
        log_manager.open();

        LogRecord record;
        record.type = LogRecordType::PAGE_UPDATE;
        record.page_id = page_id;
        record.payload = make_page_update_payload(
            100,
            5,
            {std::byte{0x11}, std::byte{0x22}});

        (void)log_manager.append(record);

        log_manager.flush();
        log_manager.close();
    }

    bool threw = false;

    try {
        recover(files.wal_path, page_manager);
    } catch (const std::runtime_error&) {
        threw = true;
    }

    require(threw, "PAGE_UPDATE length mismatch was accepted");
}

// Test 20: PAGE_UPDATE cannot overwrite the page header.
void test_page_update_header_overlap_rejected() {
    TestFiles files("page_update_header_overlap");

    DiskManager disk(files.db_path);
    DatabasePageManager page_manager(disk);

    const auto page_id = create_page(page_manager);

    {
        LogManager log_manager(files.wal_path);
        log_manager.open();

        append_page_update(
            log_manager,
            page_id,
            static_cast<std::uint32_t>(format::PAGE_HEADER_SIZE - 1),
            {std::byte{0x42}});

        log_manager.flush();
        log_manager.close();
    }

    bool threw = false;

    try {
        recover(files.wal_path, page_manager);
    } catch (const std::runtime_error&) {
        threw = true;
    }

    require(threw, "PAGE_UPDATE overwriting page header was accepted");
}

// Test 21: PAGE_UPDATE cannot exceed page boundaries.
void test_page_update_page_boundary_rejected() {
    TestFiles files("page_update_boundary");

    DiskManager disk(files.db_path);
    DatabasePageManager page_manager(disk);

    const auto page_id = create_page(page_manager);

    {
        LogManager log_manager(files.wal_path);
        log_manager.open();

        append_page_update(
            log_manager,
            page_id,
            static_cast<std::uint32_t>(format::PAGE_SIZE - 2),
            {std::byte{0x11}, std::byte{0x22},
             std::byte{0x33}, std::byte{0x44}});

        log_manager.flush();
        log_manager.close();
    }

    bool threw = false;

    try {
        recover(files.wal_path, page_manager);
    } catch (const std::runtime_error&) {
        threw = true;
    }

    require(threw, "Out-of-bounds PAGE_UPDATE was accepted");
}

// Test 22: PAGE_UPDATE targeting metadata page is rejected.
void test_page_update_metadata_page_rejected() {
    TestFiles files("page_update_metadata");

    DiskManager disk(files.db_path);
    DatabasePageManager page_manager(disk);

    {
        LogManager log_manager(files.wal_path);
        log_manager.open();

        LogRecord record;
        record.type = LogRecordType::PAGE_UPDATE;
        record.page_id = format::META_PAGE_ID;
        record.payload = make_page_update_payload(
            static_cast<std::uint32_t>(format::PAGE_HEADER_SIZE),
            1,
            {std::byte{0x42}});

        (void)log_manager.append(record);

        log_manager.flush();
        log_manager.close();
    }

    bool threw = false;

    try {
        recover(files.wal_path, page_manager);
    } catch (const std::runtime_error&) {
        threw = true;
    }

    require(threw, "PAGE_UPDATE targeting metadata page was accepted");
}

// Test 23: PAGE_UPDATE targeting an unallocated page is rejected.
void test_page_update_invalid_page_id_rejected() {
    TestFiles files("page_update_invalid_page");

    DiskManager disk(files.db_path);
    DatabasePageManager page_manager(disk);

    const auto page_id = create_page(page_manager);

    {
        LogManager log_manager(files.wal_path);
        log_manager.open();

        append_page_update(
            log_manager,
            page_id + 100,
            100,
            {std::byte{0x42}});

        log_manager.flush();
        log_manager.close();
    }

    bool threw = false;

    try {
        recover(files.wal_path, page_manager);
    } catch (const std::out_of_range &) {
        threw = true;
    }

    require(threw, "PAGE_UPDATE targeting an unallocated page was accepted");
}

// Test 24: Multiple PAGE_UPDATE records replay in LSN order.
void test_multiple_page_updates_same_page() {
    TestFiles files("multiple_page_updates");

    DiskManager disk(files.db_path);
    DatabasePageManager page_manager(disk);

    const auto page_id = create_page(page_manager);

    LSN first_lsn;
    LSN second_lsn;

    {
        LogManager log_manager(files.wal_path);
        log_manager.open();

        first_lsn = append_page_update(
            log_manager,
            page_id,
            100,
            {std::byte{0x11}});

        second_lsn = append_page_update(
            log_manager,
            page_id,
            100,
            {std::byte{0x22}});

        log_manager.flush();
        log_manager.close();
    }

    require(first_lsn < second_lsn,
            "PAGE_UPDATE WAL LSNs are not increasing");

    recover(files.wal_path, page_manager);

    const auto result = page_manager.read_page(page_id);

    require(result[100] == std::byte{0x22},
            "Latest PAGE_UPDATE was not applied");

    require(format::read_page_lsn(result) == second_lsn,
            "PageLSN does not match the latest PAGE_UPDATE");
}

} // namespace

int main() {
    int passed = 0;
    int failed = 0;

    const auto run = [&](const std::string& name, auto test) {
        if (run_test(name, test)) {
            ++passed;
        } else {
            ++failed;
        }
    };

    run("PAGE_IMAGE redo", test_page_image_redo);
    run("Equal PageLSN skipped", test_equal_page_lsn_is_skipped);
    run("Newer PageLSN skipped", test_newer_page_lsn_is_skipped);
    run("Invalid payload rejected", test_invalid_payload_size);
    run("Metadata page ID rejected", test_metadata_page_id_rejected);
    run("Recovery idempotency", test_recovery_is_idempotent);
    run("Crash/restart recovery", test_crash_restart_recovery);
    run("Multiple images same page", test_multiple_images_same_page);
    run("Multiple pages recovery", test_multiple_pages_recovery);
    run("Corrupt WAL checksum", test_corrupt_wal_checksum);
    run("Truncated WAL header", test_truncated_wal_header);
    run("Truncated WAL payload", test_truncated_wal_payload);
    run("Unsupported record types", test_unsupported_record_types_ignored);
    run("PAGE_IMAGE page ID mismatch", test_page_image_id_mismatch);

    run("PAGE_UPDATE redo", test_page_update_redo);
    run("PAGE_UPDATE idempotency", test_page_update_idempotency);
    run("PAGE_UPDATE older LSN skipped", test_page_update_older_lsn_skipped);
    run("PAGE_UPDATE invalid payload", test_page_update_invalid_payload_size);
    run("PAGE_UPDATE length mismatch", test_page_update_length_mismatch);
    run("PAGE_UPDATE page header overlap", test_page_update_header_overlap_rejected);
    run("PAGE_UPDATE page boundary", test_page_update_page_boundary_rejected);
    run("PAGE_UPDATE metadata page rejected", test_page_update_metadata_page_rejected);
    run("PAGE_UPDATE invalid page ID", test_page_update_invalid_page_id_rejected);
    run("Multiple PAGE_UPDATE records", test_multiple_page_updates_same_page);

    std::cout << "\n================================\n";
    std::cout << "RecoveryManager Test Summary\n";
    std::cout << "Passed: " << passed << '\n';
    std::cout << "Failed: " << failed << '\n';
    std::cout << "================================\n";

    return failed == 0 ? 0 : 1;
}
