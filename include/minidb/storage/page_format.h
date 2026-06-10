#ifndef MINIDB_STORAGE_PAGE_FORMAT_H
#define MINIDB_STORAGE_PAGE_FORMAT_H

#include <cstddef>
#include <cstdint>
#include <ios>
#include <limits>
#include <type_traits>
#include <array>
#include <stdexcept>

namespace minidb::storage::format {

    inline constexpr std::size_t PAGE_SIZE = 4096;

    //Database metadata header offsets @page_0
    inline constexpr std::size_t META_MAGIC_OFFSET = 0;
    inline constexpr std::size_t META_VERSION_OFFSET = 4;
    inline constexpr std::size_t META_HEADER_SIZE_OFFSET = 6;
    inline constexpr std::size_t META_NEXT_PAGE_ID_OFFSET = 8;

    inline constexpr std::size_t META_HEADER_SIZE = 16;
    inline constexpr std::uint64_t META_PAGE_ID = 0;

    //Generic Page Header offsets
    inline constexpr std::size_t PAGE_ID_OFFSET = 0;
    inline constexpr std::size_t PAGE_TYPE_OFFSET = 8;
    inline constexpr std::size_t PAGE_VERSION_OFFSET = 10;
    inline constexpr std::size_t PAGE_FLAGS_OFFSET = 12;

    inline constexpr std::size_t PAGE_HEADER_SIZE = 16;
    inline constexpr std::size_t PAGE_PAYLOAD_SIZE = PAGE_SIZE - PAGE_HEADER_SIZE;

    //On-disk format version
    inline constexpr std::uint16_t FORMAT_VERSION = 1;

    //4-byte identifier for META_MAGIC: ASCII "MNDB" encoded in little-Endian format
    //META_MAGIC is used to identify a metadata page while opening a database.
    inline constexpr std::uint32_t META_MAGIC = 0x42444E4D;

    //page-type identifiers
    enum class PageType : std::uint16_t {
        META = 1,
        BTREE_LEAF = 2,
        BTREE_INTERNAL = 3,
        LSM_DATA = 4,
        FREE = 5
    };

    //convert interger to buffer bytes [LSB to MSB]
    template <typename UInt>
    void write_le(std::byte *destination, UInt value) {
        static_assert(std::is_unsigned_v<UInt>, 
                        "write_le requires an unsigned integer type");

        for (std::size_t i = 0; i < sizeof(UInt); i++) {
            destination[i] = static_cast<std::byte>(value & 0xFFu); //fetch only LSB

            value >>= 8;    //for next iteration
        }
    }

    //convert buffer bytes [LSB to MSB] to respective integer
    template <typename UInt>
    UInt read_le(const std::byte *source) {
        static_assert(std::is_unsigned_v<UInt>,
                        "read_le requires an unsigned integer type");

        UInt value = 0;
        for (std::size_t i = 0; i < sizeof(UInt); i++) {
            value |= static_cast<UInt>(std::to_integer<unsigned int>(source[i])) << (i * 8); 
        }
        return value;
    }

    //metadata-page(Page_0) serialization and validation - to remember metadata allocation state after reopening the database
    using RawPage = std::array<std::byte, PAGE_SIZE>;

    inline RawPage make_metadata_page(std::uint64_t next_page_id = 1) { //next_page_id = 0 is never accepted
        RawPage page{};

        write_le<std::uint32_t>(page.data() + META_MAGIC_OFFSET, META_MAGIC);
        write_le<std::uint16_t>(page.data() + META_VERSION_OFFSET, FORMAT_VERSION);
        write_le<std::uint16_t>(page.data() + META_HEADER_SIZE_OFFSET, META_HEADER_SIZE);
        write_le<std::uint64_t>(page.data() + META_NEXT_PAGE_ID_OFFSET, next_page_id);

        return page;
    }

    inline std::uint64_t read_next_page_id(const RawPage &page) {

        if (read_le<std::uint32_t>(page.data() + META_MAGIC_OFFSET) != META_MAGIC) {
            throw std::runtime_error("Invalid database metadata magic");
        }

        if (read_le<std::uint16_t>(page.data() + META_VERSION_OFFSET) != FORMAT_VERSION) {
            throw std::runtime_error("Unsupported database format version");
        }

        if (read_le<std::uint16_t>(page.data() + META_HEADER_SIZE_OFFSET) != META_HEADER_SIZE) {
            throw std::runtime_error("Invalid metadata header size");
        }

        uint64_t next_page_id = read_le<std::uint64_t>(page.data() + META_NEXT_PAGE_ID_OFFSET);
        if (next_page_id == META_PAGE_ID) { //next page can't be metadata-page(Page_0)
            throw std::runtime_error("Invalid next page ID in metadata");
        }

        //pageID is large to cause a file-offset overflow
        if (next_page_id > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max() / PAGE_SIZE)) {
            throw std::runtime_error("Next pageID exceeds file offset range");
        }

        return next_page_id;
    }

    //data-page(page_1 onwards) header serialization and validation
    inline RawPage make_data_page(std::uint64_t page_id, PageType page_type, std::uint32_t page_flags = 0) {

        if (page_id == META_PAGE_ID) {
            throw std::invalid_argument("Data page cannot use the metadata page ID");
        }

        if (page_type == PageType::META) {
            throw std::invalid_argument("Metadata page type is not valid for a data page");
        }

        RawPage page{};

        write_le<uint64_t>(page.data() + PAGE_ID_OFFSET, page_id);
        write_le<uint16_t>(page.data() + PAGE_TYPE_OFFSET, static_cast<std::uint16_t>(page_type));
        write_le<uint16_t>(page.data() + PAGE_VERSION_OFFSET, FORMAT_VERSION);
        write_le<uint32_t>(page.data() + PAGE_FLAGS_OFFSET, page_flags);

        return page;
    }

    inline void validate_data_page(
        const RawPage& page,
        std::uint64_t expected_page_id) {
    
        if (expected_page_id == META_PAGE_ID) {
            throw std::invalid_argument(
                "Expected data page ID cannot be zero");
        }
    
        const auto page_id = read_le<std::uint64_t>(
            page.data() + PAGE_ID_OFFSET);
    
        if (page_id != expected_page_id) {
            throw std::runtime_error("Data page ID mismatch");
        }
    
        const auto raw_type = read_le<std::uint16_t>(
            page.data() + PAGE_TYPE_OFFSET);

        switch(static_cast<PageType>(raw_type)) {
            case PageType::BTREE_INTERNAL:
            case PageType::BTREE_LEAF:
            case PageType::LSM_DATA:
            case PageType::FREE:
                break;
            default:
                throw std::runtime_error("Invalid data page type");
        }

        const auto version = read_le<std::uint16_t>(
            page.data() + PAGE_VERSION_OFFSET);
    
        if (version != FORMAT_VERSION) {
            throw std::runtime_error("Unsupported data page version");
        }
    }

} //namespace minidb::storage::format
#endif