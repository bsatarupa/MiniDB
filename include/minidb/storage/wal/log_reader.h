//LogReader: WAL Read path, LogManager: WAL Write path
#ifndef MINIDB_STORAGE_WAL_LOG_READER_H
#define MINIDB_STORAGE_WAL_LOG_READER_H 

#include "minidb/storage/wal/log_record.h"

#include <filesystem>
#include <optional>

//WAL RD path:
//WAL file -> read 48bytes fixed header -> extract record length -> read remaining payload + checksum ->
// LogRecordSerializer::deserialize() -> Return LogRecord -> advance current_lsn
namespace minidb::storage::wal {

    //LogReader reads serialized WAL records sequentially, validates record boundaries and checksums, 
    //handles incomplete/corrupt trailing records
    class LogReader {
        public:
            explicit LogReader(const std::filesystem::path &log_file);

            ~LogReader();

            //copying is disabled to prevent accidental duplication of file descriptor ownership
            LogReader(const LogReader &) = delete;
            LogReader &operator=(const LogReader &) = delete;

            void open();
            void close();

            [[nodiscard]]
            std::optional<LogRecord> read_next();   //returns nullopt at clean EOF, when there is no new header to read

        private:
            std::filesystem::path log_file_;

            int fd_{-1};

            LSN current_lsn_{0}; //byte_offset of next record to_be_read

            bool is_open_{false};

            //no mutex needed as LogReader is a single-threaded sequential reader
    };

}   //namespace
#endif