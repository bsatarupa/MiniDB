#include "minidb/storage/wal/log_record.h"
#include "minidb/storage/page_format.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>
#include <span>

namespace minidb::storage::wal {

    namespace {

        //A WAL record does not need to fit inside a 4096-byte database page.
        // LogManager writes serialized records to the WAL file, and records can span disk blocks or filesystem writes.
        //LogRecord: 48 bytes header + payload + 4 bytes checksum
        inline constexpr std::size_t MAGIC_OFFSET = 0;
        inline constexpr std::size_t RECORD_LENGTH_OFFSET = 4;
        inline constexpr std::size_t RECORD_TYPE_OFFSET = 8;
        inline constexpr std::size_t RECORD_VERSION_OFFSET = 10;
        inline constexpr std::size_t RECORD_LSN_OFFSET = 12;
        inline constexpr std::size_t RECORD_TXN_ID_OFFSET = 20;
        inline constexpr std::size_t RECORD_PREV_LSN_OFFSET = 28;
        inline constexpr std::size_t RECORD_PAGE_ID_OFFSET = 36;
        inline constexpr std::size_t RECORD_PAYLOAD_LENGTH_OFFSET = 44;

        bool is_valid_record_type(LogRecordType type) {

            switch (type) {
                case LogRecordType::BEGIN:
                case LogRecordType::PAGE_INSERT:
                case LogRecordType::PAGE_UPDATE:
                case LogRecordType::PAGE_DELETE:
                case LogRecordType::PAGE_IMAGE:
                case LogRecordType::COMMIT:
                case LogRecordType::ABORT:
                case LogRecordType::CHECKPOINT_BEGIN:
                case LogRecordType::CHECKPOINT_END:
                case LogRecordType::METADATA_UPDATE:
                    return true;

                case LogRecordType::INVALID:    //a sentinel value, not an actual WAL operation; used for initializing a LogRecord
                default:
                    return false;
            }
        }
    }   //namespace


    std::uint32_t LogRecordSerializer::crc32(std::span<const std::byte> bytes) {

            std::uint32_t crc = 0xFFFFFFFFU;

            for (const std::byte byte : bytes) {
                crc ^= std::to_integer<std::uint8_t>(byte);

                for (int bit = 0; bit < 8; bit++) {

                    if (crc & 1U) {
                        crc = (crc >> 1U) ^ 0xEDB88320U; //0xEDB88320U = IEEE CRC-32 polynomial
                    } else {
                        crc >>= 1U;
                    }

                }
            }
            return ~crc;
    }

    //Serialization writes trusted in-memory data to bytes
    std::vector<std::byte> LogRecordSerializer::serialize(const LogRecord &record) {

        //1. Validate record type and payload size
        if (!is_valid_record_type(record.type)) {
            throw std::invalid_argument("Invalid WAL record type");
        }

        if (record.payload.size() > std::numeric_limits<std::uint32_t>::max() - LOG_RECORD_FIXED_SIZE) {
            throw std::length_error("WAL record payload is too large");
        }
    
        const auto payload_size = static_cast<std::uint32_t>(record.payload.size());
        const std::uint32_t record_size = static_cast<std::uint32_t>(LOG_RECORD_FIXED_SIZE + record.payload.size());

        //2. Allocate complete record buffer
        std::vector<std::byte> buffer(record_size);

        //3a. write header to buffer
        format::write_le<std::uint32_t>(buffer.data() + MAGIC_OFFSET, LOG_RECORD_MAGIC);
        format::write_le<std::uint32_t>(buffer.data() + RECORD_LENGTH_OFFSET, record_size);
        format::write_le<std::uint16_t>(buffer.data() + RECORD_TYPE_OFFSET, static_cast<std::uint16_t>(record.type));
        format::write_le<std::uint16_t>(buffer.data() + RECORD_VERSION_OFFSET, LOG_RECORD_VERSION);
        format::write_le<std::uint64_t>(buffer.data() + RECORD_LSN_OFFSET, record.lsn);
        format::write_le<std::uint64_t>(buffer.data() + RECORD_TXN_ID_OFFSET, record.txn_id);
        format::write_le<std::uint64_t>(buffer.data() + RECORD_PREV_LSN_OFFSET, record.prev_lsn);
        format::write_le<std::uint64_t>(buffer.data() + RECORD_PAGE_ID_OFFSET, record.page_id);
        format::write_le<std::uint32_t>(buffer.data() + RECORD_PAYLOAD_LENGTH_OFFSET, payload_size);

        //3b. append payload to buffer
        std::copy(record.payload.begin(), record.payload.end(), buffer.begin() + LOG_RECORD_HEADER_SIZE);

        //4a. calculate checksum for both header and payload
        const std::uint32_t checksum = crc32(std::span<const std::byte> (buffer.data(), LOG_RECORD_HEADER_SIZE + payload_size));

        //4b. append calculated checksum to buffer
        format::write_le<std::uint32_t>(buffer.data() + LOG_RECORD_HEADER_SIZE + payload_size, checksum);
        
        return buffer;
    }

    //Deserialization must assume the bytes read from disk could be corrupted, truncated, or malformed.
    LogRecord LogRecordSerializer::deserialize(std::span<const std::byte> bytes) {

        //1. validate minimum record size, magic number, format version, record length, payload length, record boundaries, record type, CRC32
        if (bytes.size() < LOG_RECORD_FIXED_SIZE) {
            throw std::runtime_error("WAL record is too small");
        }

        const auto magic = format::read_le<std::uint32_t>(bytes.data() + MAGIC_OFFSET);
        if (magic != LOG_RECORD_MAGIC) {
            throw std::runtime_error("Invalid WAL record magic");
        }

        const auto version = format::read_le<std::uint16_t>(bytes.data() + RECORD_VERSION_OFFSET);
        if (version != LOG_RECORD_VERSION) {
            throw std::runtime_error("Unsupported WAL record version");
        }

        //Deserialization should accept a span containing exactly one complete record, should not silently ignore trailing bytes
        const auto record_size = format::read_le<std::uint32_t>(bytes.data() + RECORD_LENGTH_OFFSET);
        if (record_size < LOG_RECORD_FIXED_SIZE) {
            throw std::runtime_error("Invalid WAL record length");
        }
        if (record_size != bytes.size()) {
            throw std::runtime_error("WAL record length mismatch");
        }

        const auto raw_type = format::read_le<std::uint16_t>(bytes.data() + RECORD_TYPE_OFFSET);
        const auto type = static_cast<LogRecordType>(raw_type);
        if (!is_valid_record_type(type)) {
            throw std::runtime_error("Invalid WAL record type");
        }
        
        const auto payload_size = format::read_le<std::uint32_t>(bytes.data() + RECORD_PAYLOAD_LENGTH_OFFSET); 
        if (payload_size != record_size - LOG_RECORD_FIXED_SIZE) {
            throw std::runtime_error("WAL payload length mismatch");
        }

        const auto stored_checksum = format::read_le<std::uint32_t>(bytes.data() + LOG_RECORD_HEADER_SIZE + payload_size);
        const auto calculated_checksum = crc32(std::span<const std::byte>(bytes.data(), LOG_RECORD_HEADER_SIZE + payload_size));
        if (stored_checksum != calculated_checksum) {
            throw std::runtime_error("WAL record checksum mismatch");
        }

        //2. reconstruct LogRecord
        LogRecord record;

        record.type = type;

        record.lsn = format::read_le<std::uint64_t>(bytes.data() + RECORD_LSN_OFFSET);
        record.txn_id = format::read_le<std::uint64_t>(bytes.data() + RECORD_TXN_ID_OFFSET);
        record.prev_lsn = format::read_le<std::uint64_t>(bytes.data() + RECORD_PREV_LSN_OFFSET);
        record.page_id = format::read_le<std::uint64_t>(bytes.data() + RECORD_PAGE_ID_OFFSET);

        //3. Extract payload
        const auto payload_begin = bytes.begin() + LOG_RECORD_HEADER_SIZE;
        const auto payload_end = payload_begin + payload_size;

        record.payload.resize(payload_size);
        std::copy(payload_begin, payload_end, record.payload.begin());

        return record;
    }


} //namespace minidb::storage::wal