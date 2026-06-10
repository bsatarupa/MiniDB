#include "minidb/storage/page_format.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {

void test_page_layout_constants() {

    using namespace minidb::storage::format;

    if (PAGE_SIZE != 4096) {
        throw std::runtime_error("Page size must be 4096 bytes");
    }

    if (PAGE_HEADER_SIZE != 16) {
        throw std::runtime_error("Page header must be 16 bytes");
    }

    if (PAGE_PAYLOAD_SIZE != 4080) {
        throw std::runtime_error("Page payload must be 4080 bytes");
    }

    //verifying most significant 4-bytes offset of page header
    if (PAGE_FLAGS_OFFSET + sizeof(std::uint32_t) != PAGE_HEADER_SIZE) {
        throw std::runtime_error("Dta page header offsets are inconsistent");
    }

    //verifying most significant 8-bytes offset of metadata header
    if (META_NEXT_PAGE_ID_OFFSET + sizeof(std::uint64_t) != META_HEADER_SIZE) {
        throw std::runtime_error("Metadata header offsets are inconsistent");
    }

    std::cout << "[PASS] Page layout constants\n";
}

void test_page_type_values() {
    using minidb::storage::format::PageType;

    if (static_cast<std::uint16_t>(PageType::META) != 1 ||
        static_cast<std::uint16_t>(PageType::BTREE_LEAF) != 2 ||
        static_cast<std::uint16_t>(PageType::BTREE_INTERNAL) != 3 ||
        static_cast<std::uint16_t>(PageType::LSM_DATA) != 4 ||
        static_cast<std::uint16_t>(PageType::FREE) != 5) {
        throw std::runtime_error("Page type IDs do not match the format");
    }

    std::cout << "[PASS] Stable page type IDs\n";
}

void test_format_version_and_magic() {
    using namespace minidb::storage::format;

    if (FORMAT_VERSION != 1) {
        throw std::runtime_error("Unexpected format version");
    }

    // META_MAGIC is encoded as the bytes "MNDB" on a little-endian host
    // only if written in native byte order. Serialization will explicitly
    // encode these bytes in the next step.
    if (META_MAGIC != 0x42444E4D) {
        throw std::runtime_error("Unexpected metadata magic constant");
    }

    std::cout << "[PASS] Format version and magic constant\n";
}

void test_little_endian_encoding() {
    using namespace minidb::storage::format;

    std::array<std::byte, 8> bytes{};

    write_le<uint32_t>(bytes.data(), 0x12345678);

    const std::array<std::byte, 4> expected {
        std::byte{0x78},
        std::byte{0x56},
        std::byte{0x34},
        std::byte{0x12}
    };

    for (std::size_t i = 0; i < sizeof(expected); i++) {
        if (bytes[i] != expected[i]) {
            throw std::runtime_error("Incorrect little-endian byte order");
        }
    }
}

void test_little_endian_round_trip() {
    using namespace minidb::storage::format;

    std::array<std::byte, 8> bytes{};

    constexpr std::uint64_t original = 0x123456789ABCDEFULL;

    write_le<uint64_t>(bytes.data(), original);

    const auto decoded = read_le<std::uint64_t>(bytes.data());

    if (original != decoded) {
        throw std::runtime_error("Little-endian round trip failed");
    }
}

void test_metadata_page_creation() {
    using namespace minidb::storage::format;
    
        const auto page = make_metadata_page();
    
        if (read_le<std::uint32_t>(
                page.data() + META_MAGIC_OFFSET) != META_MAGIC) {
            throw std::runtime_error("Metadata magic mismatch");
        }
    
        if (read_le<std::uint16_t>(
                page.data() + META_VERSION_OFFSET) != FORMAT_VERSION) {
            throw std::runtime_error("Metadata version mismatch");
        }
    
        if (read_next_page_id(page) != 1) {
            throw std::runtime_error("Initial next page ID should be 1");
        }
}

void test_invalid_metadata_magic_rejected() {

    using namespace minidb::storage::format;

    auto page = make_metadata_page();
    page[META_MAGIC_OFFSET] = std::byte{0};

    bool rejected = false;
    try {
        (void)read_next_page_id(page);
    } catch(const std::runtime_error &) {
        rejected = true;
    }

    if (!rejected) {
        throw std::runtime_error("Invalid metadata magic was accepted");
    }
}

void test_unsupported_metadata_version_rejected() {

    using namespace minidb::storage::format;

    auto page = make_metadata_page();
    write_le<uint16_t>(page.data() + META_VERSION_OFFSET, 99);

    bool rejected = false;
    try {
        (void)read_next_page_id(page);
    } catch(const std::runtime_error &) {
        rejected = true;
    }

    if (!rejected) {
        throw std::runtime_error("Unsupported metadata version was accepted");
    }
}

void test_invalid_metadata_header_size_rejected() {

    using namespace minidb::storage::format;

    auto page = make_metadata_page();
    write_le<uint16_t>(page.data() + META_HEADER_SIZE_OFFSET, 0);

    bool rejected = false;
    try {
        (void)read_next_page_id(page);
    } catch(const std::runtime_error &) {
        rejected = true;
    }

    if (!rejected) {
        throw std::runtime_error("Invalid metadata header size was accepted");
    }
}

void test_zero_next_page_id_rejected() {

    using namespace minidb::storage::format;

    auto page = make_metadata_page(0);
    write_le<uint16_t>(page.data() + META_HEADER_SIZE_OFFSET, 0);

    bool rejected = false;
    try {
        (void)read_next_page_id(page);
    } catch(const std::runtime_error &) {
        rejected = true;
    }

    if (!rejected) {
        throw std::runtime_error("Zero next page ID was accepted");
    }
}

void test_metadata_next_page_id_round_trip() {

    using namespace minidb::storage::format;

    constexpr std::uint64_t expected_next_page_id = 42;
    const auto page = make_metadata_page(expected_next_page_id);

    if (expected_next_page_id != read_next_page_id(page)) {
        throw std::runtime_error("Metadata next page ID round trip failed");
    }
}

void test_data_page_round_trip() {
    using namespace minidb::storage::format;

    constexpr std::uint64_t page_id = 42;
    constexpr std::uint32_t flags = 0x12345678;

    const auto page = make_data_page(
        page_id, PageType::BTREE_LEAF, flags);

    validate_data_page(page, page_id);

    if (read_le<std::uint64_t>(
            page.data() + PAGE_ID_OFFSET) != page_id) {
        throw std::runtime_error("Data page ID mismatch");
    }

    if (read_le<std::uint16_t>(
            page.data() + PAGE_TYPE_OFFSET) !=
        static_cast<std::uint16_t>(PageType::BTREE_LEAF)) {
        throw std::runtime_error("Data page type mismatch");
    }

    if (read_le<std::uint32_t>(
            page.data() + PAGE_FLAGS_OFFSET) != flags) {
        throw std::runtime_error("Data page flags mismatch");
    }

    for (std::size_t i = PAGE_HEADER_SIZE; i < PAGE_SIZE; ++i) {
        if (page[i] != std::byte{0}) {
            throw std::runtime_error("Data page payload not zero-initialized");
        }
    }
}

void test_invalid_data_page_id_rejected() {
    using namespace minidb::storage::format;

    bool rejected = false;

    try {
        (void)make_data_page(0, PageType::BTREE_LEAF);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }

    if (!rejected) {
        throw std::runtime_error("Reserved page ID was accepted");
    }
}

void test_data_page_id_mismatch_rejected() {
    using namespace minidb::storage::format;

    const auto page = make_data_page(10, PageType::BTREE_LEAF);

    bool rejected = false;

    try {
        validate_data_page(page, 11);
    } catch (const std::runtime_error&) {
        rejected = true;
    }

    if (!rejected) {
        throw std::runtime_error("Mismatched page ID was accepted");
    }
}

void test_unknown_data_page_type_rejected() {
    using namespace minidb::storage::format;

    auto page = make_data_page(1, PageType::BTREE_LEAF);

    write_le<std::uint16_t>(
        page.data() + PAGE_TYPE_OFFSET, 999);

    bool rejected = false;

    try {
        validate_data_page(page, 1);
    } catch (const std::runtime_error&) {
        rejected = true;
    }

    if (!rejected) {
        throw std::runtime_error("Unknown page type was accepted");
    }
}

void test_unsupported_data_page_version_rejected() {
    using namespace minidb::storage::format;

    auto page = make_data_page(1, PageType::BTREE_LEAF);

    write_le<std::uint16_t>(
        page.data() + PAGE_VERSION_OFFSET, 99);

    bool rejected = false;

    try {
        validate_data_page(page, 1);
    } catch (const std::runtime_error&) {
        rejected = true;
    }

    if (!rejected) {
        throw std::runtime_error("Unsupported page version was accepted");
    }
}

void test_metadata_type_rejected_for_data_page() {
    using namespace minidb::storage::format;

    bool rejected = false;

    try {
        (void)make_data_page(1, PageType::META);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }

    if (!rejected) {
        throw std::runtime_error("Metadata type accepted for data page");
    }
}


} //end of namespace

int main() {
    try {
        test_page_layout_constants();
        test_page_type_values();
        test_format_version_and_magic();

        test_little_endian_encoding();
        test_little_endian_round_trip();
        test_metadata_next_page_id_round_trip();
        test_metadata_page_creation();

        test_invalid_metadata_magic_rejected();
        test_zero_next_page_id_rejected();
        test_invalid_metadata_header_size_rejected();
        test_unsupported_metadata_version_rejected();

        test_data_page_round_trip();
        test_invalid_data_page_id_rejected();
        test_data_page_id_mismatch_rejected();
        test_unknown_data_page_type_rejected();
        test_unsupported_data_page_version_rejected();
        test_metadata_type_rejected_for_data_page();

        std::cout << "\nAll page-format tests passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] Page-format tests: " << e.what() << '\n';
        return 1;
    }
}