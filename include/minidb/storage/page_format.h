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
    inline constexpr std::size_t PAGE_LSN_OFFSET = 16;

    inline constexpr std::size_t PAGE_HEADER_SIZE = 24;
    inline constexpr std::size_t PAGE_PAYLOAD_SIZE = PAGE_SIZE - PAGE_HEADER_SIZE;

    //Invalid PageLSN sentinel; since LSN_0 is a valid WAL record
    inline constexpr std::uint64_t INVALID_PAGE_LSN = std::numeric_limits<std::uint64_t>::max();

    //On-disk format version:
    //Format version: (1 -> 2) - added 8 bytes of lsn to page_header starting at page_lsn_offset = 16
    inline constexpr std::uint16_t FORMAT_VERSION = 2; 

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
    template <typename Int>
    void write_le(std::byte *destination, Int value) {
        static_assert(std::is_integral_v<Int>, "write_le requires an integral type");

        using UInt = std::make_unsigned_t<Int>;
        UInt bits = static_cast<UInt>(value);

        for (std::size_t i = 0; i < sizeof(Int); i++) {
            destination[i] = static_cast<std::byte>(bits & 0xFF); //fetch only LSB

            bits >>= 8;    //for next iteration
        }
    }

    //convert buffer bytes [LSB to MSB] to respective integer
    template <typename Int>
    Int read_le(const std::byte *source) {
        static_assert(std::is_integral_v<Int>, "read_le requires an integral type");

        using UInt = std::make_unsigned_t<Int>;
        UInt bits = 0;

        for (std::size_t i = 0; i < sizeof(Int); i++) {
            bits |= static_cast<UInt>(std::to_integer<unsigned int>(source[i])) << (i * 8); 
        }
        return bits;
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
        write_le<uint64_t>(page.data() + PAGE_LSN_OFFSET, INVALID_PAGE_LSN);

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

    //Index metadata header offsets @page_0
    inline constexpr std::size_t INDEX_META_MAGIC_OFFSET = 0;
    inline constexpr std::size_t INDEX_META_VERSION_OFFSET = 4;
    inline constexpr std::size_t INDEX_META_ROOT_PAGE_ID_OFFSET = 6;
    inline constexpr std::size_t INDEX_META_NEXT_PAGE_ID_OFFSET = 14;

    inline constexpr std::uint32_t INDEX_META_MAGIC = 0x58444E4D;
    inline constexpr std::uint64_t INVALID_PAGE_ID = std::numeric_limits<std::uint64_t>::max();

    inline RawPage make_index_metadata_page(std::uint64_t root_page_id = INVALID_PAGE_ID,
                                            std::uint64_t next_page_id = 1) { //next_page_id = 0 is never accepted
        RawPage page{};

        write_le<std::uint32_t>(page.data() + INDEX_META_MAGIC_OFFSET, INDEX_META_MAGIC);
        write_le<std::uint16_t>(page.data() + INDEX_META_VERSION_OFFSET, FORMAT_VERSION);
        write_le<std::uint64_t>(page.data() + INDEX_META_ROOT_PAGE_ID_OFFSET, root_page_id);
        write_le<std::uint64_t>(page.data() + INDEX_META_NEXT_PAGE_ID_OFFSET, next_page_id);

        return page;
    }

    //validate index metapage and recover its root_page_id and next_page_id
    inline void read_index_metadata_page(
        const RawPage& page,
        std::uint64_t& root_page_id,
        std::uint64_t& next_page_id) {
    
        if (read_le<std::uint32_t>(
                page.data() + INDEX_META_MAGIC_OFFSET) != INDEX_META_MAGIC) {
            throw std::runtime_error("Invalid index metadata magic");
        }
    
        if (read_le<std::uint16_t>(
                page.data() + INDEX_META_VERSION_OFFSET) != FORMAT_VERSION) {
            throw std::runtime_error("Unsupported index format version");
        }
    
        root_page_id = read_le<std::uint64_t>(
            page.data() + INDEX_META_ROOT_PAGE_ID_OFFSET);
    
        next_page_id = read_le<std::uint64_t>(
            page.data() + INDEX_META_NEXT_PAGE_ID_OFFSET);

        if (next_page_id == 0 || next_page_id == INVALID_PAGE_ID) {
            throw std::runtime_error("Invalid next pageID in Index Metadata");
        }

        if (root_page_id != INVALID_PAGE_ID &&
            (root_page_id == 0 || root_page_id >= next_page_id)) {
            throw std::runtime_error("Invalid root pageID in Index Metadata");
        }

        const auto max_page_id = static_cast<std::uint64_t>
                                                    (std::numeric_limits<std::streamoff>::max() / PAGE_SIZE);
        if (next_page_id > max_page_id) {
            throw std::runtime_error("Next pageID exceeds the file-offset range");
        }
    }

    //extract page lsn
    inline std::uint64_t read_page_lsn(const RawPage &page) {
        return read_le<std::uint64_t>(page.data() + PAGE_LSN_OFFSET);
    }

    inline void write_page_lsn(RawPage &page, std::uint64_t lsn) {
        write_le(page.data() + PAGE_LSN_OFFSET, lsn);
    }

} //namespace minidb::storage::format
#endif