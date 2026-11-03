#ifndef MINIDB_STORAGE_WAL_LOG_RECORD_H
#define MINIDB_STORAGE_WAL_LOG_RECORD_H

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>
#include <span>

namespace minidb::storage::wal {

    /*
    A WAL record does not need to fit inside a 4096-byte database page.
    LogManager writes serialized records to the WAL file, and records can span disk blocks or filesystem writes.
    
    Log record layout = 48 bytes of header + data payload + 4 bytes of checksum
    with CRC32 checksum covering record header and data payload
    */

    //WAL types
    using LSN = std::uint64_t;  //consistent ID across WAL and pages
    using TxnID = std::uint64_t;  //consistent transaction ID
    using PageID = std::uint64_t;  //consistent page reference
    
    //Invalid Identifiers
    inline constexpr LSN INVALID_LSN = std::numeric_limits<LSN>::max();
    inline constexpr TxnID INVALID_TXN_ID = 0;
    inline constexpr PageID INVALID_PAGE_ID = std::numeric_limits<PageID>::max();
    
    //WAL Record Format
    inline constexpr std::uint32_t LOG_RECORD_MAGIC = 0x4D444257;   //detects invalid WAL data or wrong file format

    inline constexpr std::uint16_t LOG_RECORD_VERSION = 1;  //supports future WAL format evolution

    inline constexpr std::size_t LOG_RECORD_HEADER_SIZE = 48;
    inline constexpr std::size_t LOG_RECORD_CHECKSUM_SIZE = 4;
    inline constexpr std::size_t LOG_RECORD_FIXED_SIZE = LOG_RECORD_HEADER_SIZE + LOG_RECORD_CHECKSUM_SIZE;

    //WAL Record Types
    enum class LogRecordType : std::uint16_t {
        INVALID = 0,

        BEGIN = 1,

        PAGE_INSERT = 2,
        PAGE_UPDATE = 3,
        PAGE_DELETE = 4,

        PAGE_IMAGE = 5,

        COMMIT = 6,
        ABORT = 7,

        CHECKPOINT_BEGIN = 8,
        CHECKPOINT_END = 9,

        METADATA_UPDATE = 10
    };

    //A WAL LogRecord represents a DB operation that must be recorded for durability and recovery
    struct LogRecord {

        LogRecordType type{LogRecordType::INVALID};

        LSN lsn{INVALID_LSN};

        TxnID txn_id{INVALID_TXN_ID};

        LSN prev_lsn{INVALID_LSN};    //to track Transaction log chains

        PageID page_id{INVALID_PAGE_ID};

        std::vector<std::byte>  payload;
    };

    //Log Record Serializer: Separates record representation from Disk encoding
    class LogRecordSerializer {

        public:
        //serializes complete record including its CRC32 checksum
        //[[nodiscard]] : compiler issues warning if caller ignores the function's return value
        [[nodiscard]]
        static std::vector<std::byte> serialize(const LogRecord &record);

        //deserializes and validates one complete record
        //std::span : a light-weight, non-owning view on a contiguous object sequence(no memeory allocation/deallocation, 
        //only points to an in-memory array/vector); in this case, a deserialization input
        //copying a std::span is cheap(O(1)) and is passed to functions by value
        [[nodiscard]]
        static LogRecord deserialize(std::span<const std::byte> bytes);

        private:
        [[nodiscard]]
        static std::uint32_t crc32(std::span<const std::byte> bytes);
    };

}   //namespace

#endif